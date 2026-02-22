#include "MultiThreadBlockUpdate.h"

#include <ll/api/service/Level.h>
#include <ll/api/chrono/GameChrono.h>
#include <mc/world/level/Level.h>
#include <mc/world/BlockSource.h>
#include <mc/world/level/chunk/LevelChunk.h>
#include <mc/math/ChunkPos.h>
#include <mc/world/level/block/Block.h>
#include <mc/world/level/block/BlockLegacy.h>
#include <mc/world/level/dimension/Dimension.h>
#include <mc/world/level/ChunkSource.h>

using namespace ll::chrono_literals;

thread_local std::mt19937 MultiThreadBlockUpdate::mRng{std::random_device{}()};

MultiThreadBlockUpdate& MultiThreadBlockUpdate::getInstance() {
    static MultiThreadBlockUpdate instance(ll::mod::NativeMod::currentManifest());
    return instance;
}

MultiThreadBlockUpdate::MultiThreadBlockUpdate(ll::mod::Manifest const& manifest)
    : ll::mod::NativeMod(manifest) {}

bool MultiThreadBlockUpdate::load() {
    getLogger().info("§aMultiThreadBlockUpdate 已加载 - 全局多线程方块更新优化插件");
    getLogger().warn("§c优化所有加载chunks，方块扫描全高度，计算多线程。测试后使用以避免滞后。");
    return true;
}

bool MultiThreadBlockUpdate::enable() {
    getLogger().info("§b启用全局多线程方块更新优化...");
    return true;
}

bool MultiThreadBlockUpdate::disable() {
    getLogger().info("§eMultiThreadBlockUpdate 已禁用");
    return true;
}

// Hook implementation
void MultiThreadBlockUpdate::LevelChunkTickingSystemTickHook::operator()(::EntityRegistry& registry) {
    origin(registry);  // Run vanilla first
    getInstance().onTickingSystemTick(registry);
}

void MultiThreadBlockUpdate::onTickingSystemTick(::EntityRegistry& registry) {
    ll::coro::keepThis([this, &registry]() -> ll::coro::CoroTask<> {
        co_await 1_tick;

        if (auto levelOpt = ll::service::getLevel()) {
            auto& level = *levelOpt;

            // Use cache if valid, else recollect
            std::vector<std::shared_ptr<::LevelChunk>> allChunks;
            if (mCacheTickCounter++ < CACHE_INVALIDATE_INTERVAL) {
                allChunks = mCachedChunks;
            } else {
                allChunks = collectAllLoadedChunks(level);
                mCachedChunks = allChunks;
                mCacheTickCounter = 0;
            }

            if (!allChunks.empty()) {
                if (auto* dim = level.getDimension(0)) {  // Default to Overworld region, extend for others if needed
                    auto& region = dim->getBlockSourceFromMainChunkSource();
                    co_await parallelProcessAllChunks(allChunks, region);
                    applyScheduledUpdates(region);
                }
            }
        }
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}

// Optimized collection: Iterate dimensions, large grid (-512 to 512 covers ~8km x 8km world), check getExistingChunk, unique set
std::vector<std::shared_ptr<::LevelChunk>> MultiThreadBlockUpdate::collectAllLoadedChunks(::Level& level) {
    std::vector<std::shared_ptr<::LevelChunk>> allChunks;
    std::unordered_set<::ChunkPos> uniquePositions;

    for (int dimId = 0; dimId < 3; ++dimId) {  // Overworld, Nether, End
        if (auto* dim = level.getDimension(dimId)) {
            auto& chunkSource = dim->getChunkSource();

            for (int cx = -512; cx <= 512; ++cx) {
                for (int cz = -512; cz <= 512; ++cz) {
                    ::ChunkPos cp(cx, cz);
                    if (uniquePositions.count(cp)) continue;

                    if (auto chunk = chunkSource.getExistingChunk(cp)) {
                        allChunks.push_back(chunk);
                        uniquePositions.insert(cp);
                    }
                }
            }
        }
    }

    getLogger().debug("§6收集到 {} 个全局加载 chunks", allChunks.size());
    return allChunks;
}

ll::coro::CoroTask<void> MultiThreadBlockUpdate::parallelProcessAllChunks(
    const std::vector<std::shared_ptr<::LevelChunk>>& allChunks,
    ::BlockSource& region) {

    co_await ll::thread::ThreadPoolExecutor::getDefault();  // Switch to pool

    std::vector<ScheduledBlockUpdate> localUpdates;

    // Optimization: Shuffle chunks for load balancing across threads (if pool has multiple)
    auto shuffledChunks = allChunks;
    std::shuffle(shuffledChunks.begin(), shuffledChunks.end(), mRng);

    for (const auto& chunkPtr : shuffledChunks) {
        if (!chunkPtr) continue;
        const ::ChunkPos& cpos = chunkPtr->getPosition();

        // Full height scan (-64 to 320)
        for (int x = 0; x < 16; ++x) {
            for (int z = 0; z < 16; ++z) {
                for (int y = -64; y < 320; ++y) {
                    ::BlockPos pos(cpos.x * 16 + x, y, cpos.z * 16 + z);

                    const ::Block& block = region.getBlock(pos);
                    const auto& blockLegacy = block.getBlockLegacy();

                    // Optimized hash comparison (precomputed)
                    auto nameHash = blockLegacy.getNameHash();
                    bool needsUpdate = false;

                    // Crops
                    if (nameHash == wheatHash || nameHash == carrotsHash || nameHash == "minecraft:potatoes"_h ||
                        nameHash == "minecraft:beetroots"_h || nameHash == "minecraft:farmland"_h || nameHash == "minecraft:cactus"_h ||
                        nameHash == "minecraft:sugar_cane"_h || nameHash == "minecraft:nether_wart"_h || nameHash == "minecraft:pumpkin_stem"_h ||
                        nameHash == "minecraft:melon_stem"_h || nameHash == "minecraft:bamboo"_h) {
                        if (mRng() % 12 == 0) needsUpdate = true;
                    }
                    // Containers/Block Entities
                    else if (nameHash == "minecraft:hopper"_h || nameHash == "minecraft:chest"_h || nameHash == "minecraft:trapped_chest"_h ||
                             nameHash == "minecraft:barrel"_h || nameHash == "minecraft:furnace"_h || nameHash == "minecraft:blast_furnace"_h ||
                             nameHash == "minecraft:smoker"_h || nameHash == "minecraft:dispenser"_h || nameHash == "minecraft:dropper"_h ||
                             nameHash == "minecraft:shulker_box"_h || nameHash == "minecraft:brewing_stand"_h) {
                        if (mRng() % 20 == 0) needsUpdate = true;
                    }
                    // Redstone/Mechanics
                    else if (nameHash == "minecraft:piston"_h || nameHash == "minecraft:sticky_piston"_h || nameHash == "minecraft:redstone_wire"_h ||
                             nameHash == "minecraft:redstone_torch"_h || nameHash == "minecraft:redstone_lamp"_h || nameHash == "minecraft:repeater"_h ||
                             nameHash == "minecraft:comparator"_h || nameHash == "minecraft:observer"_h || nameHash == "minecraft:daylight_detector"_h ||
                             nameHash == "minecraft:target"_h || nameHash == "minecraft:sculk_sensor"_h || nameHash == "minecraft:calibrated_sculk_sensor"_h) {
                        if (mRng() % 8 == 0) needsUpdate = true;
                    }
                    // Other updates (ice, fire, leaves, etc.)
                    else if (nameHash == "minecraft:ice"_h || nameHash == "minecraft:packed_ice"_h || nameHash == "minecraft:blue_ice"_h ||
                             nameHash == "minecraft:fire"_h || nameHash == "minecraft:soul_fire"_h || nameHash == "minecraft:lava"_h ||
                             nameHash == "minecraft:water"_h || nameHash == "minecraft:leaves"_h || nameHash == "minecraft:oak_leaves"_h ||
                             nameHash == "minecraft:spruce_leaves"_h || nameHash == "minecraft:birch_leaves"_h || nameHash == "minecraft:jungle_leaves"_h ||
                             nameHash == "minecraft:acacia_leaves"_h || nameHash == "minecraft:dark_oak_leaves"_h || nameHash == "minecraft:mangrove_leaves"_h ||
                             nameHash == "minecraft:cherry_leaves"_h || nameHash == "minecraft:azalea_leaves"_h || nameHash == "minecraft:flowering_azalea_leaves"_h ||
                             nameHash == "minecraft:sponge"_h || nameHash == "minecraft:wet_sponge"_h || nameHash == "minecraft:snow"_h ||
                             nameHash == "minecraft:powder_snow"_h || nameHash == "minecraft:grass_block"_h || nameHash == "minecraft:mycelium"_h ||
                             nameHash == "minecraft:podzol"_h || nameHash == "minecraft:dirt_path"_h) {
                        if (mRng() % 16 == 0) needsUpdate = true;
                    }

                    if (needsUpdate) {
                        localUpdates.emplace_back(ScheduledBlockUpdate{pos, static_cast<int>(mRng() % 5 + 1)});  // Random delay 1-5
                    }
                }
            }
        }
    }

    // Lock and push
    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        for (auto& u : localUpdates) {
            mUpdateQueue.push(std::move(u));
        }
    }
}

void MultiThreadBlockUpdate::applyScheduledUpdates(::BlockSource& region) {
    std::queue<ScheduledBlockUpdate> localQueue;
    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        std::swap(localQueue, mUpdateQueue);
    }

    while (!localQueue.empty()) {
        auto task = localQueue.front();
        localQueue.pop();
        region.scheduleBlockUpdate(task.pos, task.delay);
    }
}
