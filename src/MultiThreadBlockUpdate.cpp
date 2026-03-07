#include "MultiThreadBlockUpdate.h"

#include <mc/world/level/Level.h>
#include <ll/api/chrono/GameChrono.h>
#include <mc/world/level/Level.h>
#include <mc/world/BlockSource.h>
#include <mc/world/level/chunk/LevelChunk.h>
#include <mc/math/ChunkPos.h>
#include <mc/world/level/block/Block.h>
#include <mc/world/level/block/BlockLegacy.h>
#include <mc/world/level/dimension/Dimension.h>
#include <mc/world/level/ChunkSource.h>
#include <mc/common/HashedString.h>  // 用于获取哈希值

#include <random>
#include <algorithm>
#include <chrono>

using namespace ll::chrono_literals;

thread_local std::mt19937 MultiThreadBlockUpdate::mRng{std::random_device{}()};

// ==================== SimpleThreadPool 实现 ====================

SimpleThreadPool::SimpleThreadPool(size_t threadCount) {
    for (size_t i = 0; i < threadCount; ++i) {
        workers.emplace_back(&SimpleThreadPool::worker, this);
    }
}

SimpleThreadPool::~SimpleThreadPool() {
    {
        std::unique_lock lock(queueMutex);
        stop = true;
    }
    condition.notify_all();
    for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
    }
}

void SimpleThreadPool::enqueue(std::function<void()> task) {
    {
        std::unique_lock lock(queueMutex);
        tasks.push(std::move(task));
        pendingTasks++;
    }
    condition.notify_one();
}

void SimpleThreadPool::waitForAll() {
    std::unique_lock lock(queueMutex);
    allDone.wait(lock, [this] { return pendingTasks == 0; });
}

void SimpleThreadPool::worker() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(queueMutex);
            condition.wait(lock, [this] { return stop || !tasks.empty(); });
            if (stop && tasks.empty()) return;
            task = std::move(tasks.front());
            tasks.pop();
        }
        task();
        pendingTasks--;
        if (pendingTasks == 0) {
            allDone.notify_one();
        }
    }
}

// ==================== MultiThreadBlockUpdate 实现 ====================

MultiThreadBlockUpdate& MultiThreadBlockUpdate::getInstance() {
    static MultiThreadBlockUpdate instance(ll::mod::NativeMod::currentManifest());
    return instance;
}

MultiThreadBlockUpdate::MultiThreadBlockUpdate(ll::mod::Manifest const& manifest)
    : ll::mod::NativeMod(manifest) {
    // 预计算需要处理的方块哈希值
    std::vector<std::string> targetNames = {
        // 作物
        "minecraft:wheat", "minecraft:carrots", "minecraft:potatoes", "minecraft:beetroots",
        "minecraft:farmland", "minecraft:cactus", "minecraft:sugar_cane", "minecraft:nether_wart",
        "minecraft:pumpkin_stem", "minecraft:melon_stem", "minecraft:bamboo",

        // 容器/方块实体
        "minecraft:hopper", "minecraft:chest", "minecraft:trapped_chest", "minecraft:barrel",
        "minecraft:furnace", "minecraft:blast_furnace", "minecraft:smoker", "minecraft:dispenser",
        "minecraft:dropper", "minecraft:shulker_box", "minecraft:brewing_stand",

        // 红石/机械
        "minecraft:piston", "minecraft:sticky_piston", "minecraft:redstone_wire",
        "minecraft:redstone_torch", "minecraft:redstone_lamp", "minecraft:repeater",
        "minecraft:comparator", "minecraft:observer", "minecraft:daylight_detector",
        "minecraft:target", "minecraft:sculk_sensor", "minecraft:calibrated_sculk_sensor",

        // 其他
        "minecraft:ice", "minecraft:packed_ice", "minecraft:blue_ice", "minecraft:fire",
        "minecraft:soul_fire", "minecraft:lava", "minecraft:water",
        "minecraft:leaves", "minecraft:oak_leaves", "minecraft:spruce_leaves",
        "minecraft:birch_leaves", "minecraft:jungle_leaves", "minecraft:acacia_leaves",
        "minecraft:dark_oak_leaves", "minecraft:mangrove_leaves", "minecraft:cherry_leaves",
        "minecraft:azalea_leaves", "minecraft:flowering_azalea_leaves",
        "minecraft:sponge", "minecraft:wet_sponge", "minecraft:snow",
        "minecraft:powder_snow", "minecraft:grass_block", "minecraft:mycelium",
        "minecraft:podzol", "minecraft:dirt_path"
    };

    for (const auto& name : targetNames) {
        mBlockHashes[name] = HashedString(name).getHash();
        mTargetBlocks.insert(mBlockHashes[name]);
    }
}

bool MultiThreadBlockUpdate::load() {
    getLogger().info("§aMultiThreadBlockUpdate 已加载 - 全局多线程方块更新优化插件");
    getLogger().warn("§c优化所有加载chunks，模拟原版随机刻，多线程处理。");
    return true;
}

bool MultiThreadBlockUpdate::enable() {
    getLogger().info("§b启用全局多线程方块更新优化...");
    // 创建线程池，线程数 = CPU核心数
    size_t threadCount = std::thread::hardware_concurrency();
    mThreadPool = std::make_unique<SimpleThreadPool>(threadCount);
    getLogger().info("线程池已创建，线程数: {}", threadCount);
    return true;
}

bool MultiThreadBlockUpdate::disable() {
    getLogger().info("§eMultiThreadBlockUpdate 已禁用");
    mThreadPool.reset();
    return true;
}

// Hook 实现
void MultiThreadBlockUpdate::LevelChunkTickingSystemTickHook::operator()(::EntityRegistry& registry) {
    origin(registry);
    getInstance().onTickingSystemTick(registry);
}

void MultiThreadBlockUpdate::onTickingSystemTick(::EntityRegistry& registry) {
    auto level = ll::service::getLevel();
    if (!level) return;

    // 收集所有区块（使用缓存）
    std::vector<std::shared_ptr<::LevelChunk>> allChunks;
    if (mCacheTickCounter++ < CACHE_INVALIDATE_INTERVAL && !mCachedChunks.empty()) {
        allChunks = mCachedChunks;
    } else {
        allChunks = collectAllLoadedChunks(*level);
        mCachedChunks = allChunks;
        mCacheTickCounter = 0;
    }

    if (allChunks.empty()) return;

    // 默认使用主世界的 BlockSource（后续可扩展）
    auto* dim = level->getDimension(0);
    if (!dim) return;
    auto& region = dim->getBlockSourceFromMainChunkSource();

    // 并行处理区块
    parallelProcessAllChunks(allChunks, region);

    // 应用计划更新
    applyScheduledUpdates(region);
}

std::vector<std::shared_ptr<::LevelChunk>> MultiThreadBlockUpdate::collectAllLoadedChunks(::Level& level) {
    std::vector<std::shared_ptr<::LevelChunk>> chunks;
    std::unordered_set<ChunkPos> uniquePositions;

    // 遍历所有维度
    for (int dimId = 0; dimId < 3; ++dimId) {
        auto* dim = level.getDimension(dimId);
        if (!dim) continue;
        auto& chunkSource = dim->getChunkSource();

        // 尝试获取存储的区块映射（假设存在 getStorage 方法）
        // 若没有，可使用其他方法，这里以 getStorage 为例
        auto& storage = chunkSource.getStorage();  // 返回 std::unordered_map<ChunkPos, std::weak_ptr<LevelChunk>> const&
        for (const auto& [pos, weakChunk] : storage) {
            auto chunk = weakChunk.lock();
            if (chunk) {
                if (uniquePositions.insert(pos).second) {
                    chunks.push_back(chunk);
                }
            }
        }
    }

    getLogger().debug("§6收集到 {} 个全局加载 chunks", chunks.size());
    return chunks;
}

void MultiThreadBlockUpdate::parallelProcessAllChunks(
    const std::vector<std::shared_ptr<::LevelChunk>>& allChunks,
    ::BlockSource& region) {

    const size_t numThreads = mThreadPool ? mThreadPool->workers.size() : 1;
    const size_t chunkCount = allChunks.size();
    if (chunkCount == 0) return;

    // 将区块均匀分给每个线程
    size_t chunkPerThread = (chunkCount + numThreads - 1) / numThreads;

    for (size_t t = 0; t < numThreads; ++t) {
        size_t start = t * chunkPerThread;
        size_t end = std::min(start + chunkPerThread, chunkCount);
        if (start >= end) break;

        mThreadPool->enqueue([this, start, end, &allChunks, &region]() {
            std::vector<ScheduledBlockUpdate> localUpdates;

            for (size_t i = start; i < end; ++i) {
                auto& chunkPtr = allChunks[i];
                if (!chunkPtr) continue;
                const ChunkPos& cpos = chunkPtr->getPosition();

                // 模拟原版随机刻：每个区块随机选择 3 个位置
                for (int attempt = 0; attempt < 3; ++attempt) {
                    int x = mRng() % 16;
                    int z = mRng() % 16;
                    int y = 64 + (mRng() % 256);  // 在 64~319 之间随机，避免超出世界范围

                    BlockPos pos(cpos.x * 16 + x, y, cpos.z * 16 + z);

                    const Block& block = region.getBlock(pos);
                    const BlockLegacy& blockLegacy = block.getBlockLegacy();
                    uint64_t nameHash = blockLegacy.getNameHash();

                    if (mTargetBlocks.find(nameHash) != mTargetBlocks.end()) {
                        // 随机延迟 1-5 tick
                        localUpdates.emplace_back(ScheduledBlockUpdate{pos, static_cast<int>(mRng() % 5 + 1)});
                    }
                }
            }

            if (!localUpdates.empty()) {
                std::lock_guard<std::mutex> lock(mQueueMutex);
                for (auto& u : localUpdates) {
                    mUpdateQueue.push(std::move(u));
                }
            }
        });
    }

    // 等待所有任务完成
    mThreadPool->waitForAll();
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

// 注册模组
LL_REGISTER_MOD(MultiThreadBlockUpdate, MultiThreadBlockUpdate::getInstance());
