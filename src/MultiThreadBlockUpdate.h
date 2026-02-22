#pragma once

#include <ll/api/mod/NativeMod.h>
#include <ll/api/memory/Hook.h>
#include <ll/api/coro/CoroTask.h>
#include <ll/api/thread/ServerThreadExecutor.h>
#include <ll/api/thread/ThreadPoolExecutor.h>
#include <ll/api/utils/Hash.h>
#include <mutex>
#include <queue>
#include <vector>
#include <random>
#include <unordered_set>
#include <algorithm>  // for std::sort, std::shuffle for load balancing

class EntityRegistry;
class BlockSource;
class LevelChunk;
class BlockPos;
class ChunkPos;
class Level;
class Dimension;

struct ScheduledBlockUpdate {
    ::BlockPos pos;
    int        delay = 0;
};

class MultiThreadBlockUpdate : public ll::mod::NativeMod {
public:
    static MultiThreadBlockUpdate& getInstance();

    explicit MultiThreadBlockUpdate(ll::mod::Manifest const& manifest);

    bool load() override;
    bool enable() override;
    bool disable() override;

private:
    // Hook LevelChunkTickingSystem::tick for integration
    LL_AUTO_TYPE_INSTANCE_HOOK(LevelChunkTickingSystemTickHook,
                               ll::memory::HookPriority::Normal,
                               ::LevelChunkTickingSystem,
                               &LevelChunkTickingSystem::tick,
                               void,
                               ::EntityRegistry&);

    void onTickingSystemTick(::EntityRegistry& registry);

    // Optimized collection of all loaded chunks (large grid check with getExistingChunk, cached unique)
    std::vector<std::shared_ptr<::LevelChunk>> collectAllLoadedChunks(::Level& level);

    // Multi-threaded processing of all chunks (batched, load balanced)
    ll::coro::CoroTask<void> parallelProcessAllChunks(const std::vector<std::shared_ptr<::LevelChunk>>& allChunks,
                                                      ::BlockSource& region);

    // Main thread apply updates (batch schedule to minimize calls)
    void applyScheduledUpdates(::BlockSource& region);

    std::mutex mQueueMutex;
    std::queue<ScheduledBlockUpdate> mUpdateQueue;

    thread_local static std::mt19937 mRng;

    // Optimization: Precomputed hash for common blocks
    static constexpr ll::hash::HashedString wheatHash = "minecraft:wheat"_h;
    static constexpr ll::hash::HashedString carrotsHash = "minecraft:carrots"_h;
    // ... (add all below similarly for performance)

    // Cache for last collected chunks to skip full scan every tick (invalidate every 10 ticks)
    std::vector<std::shared_ptr<::LevelChunk>> mCachedChunks;
    int mCacheTickCounter = 0;
    static constexpr int CACHE_INVALIDATE_INTERVAL = 10;
};
