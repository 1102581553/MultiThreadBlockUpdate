#pragma once

#include <ll/api/mod/NativeMod.h>
#include <ll/api/memory/Hook.h>
#include <mutex>
#include <queue>
#include <vector>
#include <random>
#include <unordered_set>
#include <unordered_map>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <functional>

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

// 简单的线程池实现
class SimpleThreadPool {
public:
    explicit SimpleThreadPool(size_t threadCount);
    ~SimpleThreadPool();

    void enqueue(std::function<void()> task);
    void waitForAll();

private:
    void worker();

    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queueMutex;
    std::condition_variable condition;
    std::atomic<bool> stop{false};
    std::atomic<size_t> pendingTasks{0};
    std::condition_variable allDone;
};

class MultiThreadBlockUpdate : public ll::mod::NativeMod {
public:
    static MultiThreadBlockUpdate& getInstance();

    explicit MultiThreadBlockUpdate(ll::mod::Manifest const& manifest);

    bool load() override;
    bool enable() override;
    bool disable() override;

private:
    // Hook LevelChunkTickingSystem::tick
    LL_AUTO_TYPE_INSTANCE_HOOK(LevelChunkTickingSystemTickHook,
                               ll::memory::HookPriority::Normal,
                               ::LevelChunkTickingSystem,
                               &LevelChunkTickingSystem::tick,
                               void,
                               ::EntityRegistry&);

    void onTickingSystemTick(::EntityRegistry& registry);

    // 收集所有已加载的区块（使用 ChunkSource::getStorage）
    std::vector<std::shared_ptr<::LevelChunk>> collectAllLoadedChunks(::Level& level);

    // 多线程处理所有区块
    void parallelProcessAllChunks(const std::vector<std::shared_ptr<::LevelChunk>>& allChunks,
                                  ::BlockSource& region);

    // 应用计划更新
    void applyScheduledUpdates(::BlockSource& region);

    std::mutex mQueueMutex;
    std::queue<ScheduledBlockUpdate> mUpdateQueue;

    // 线程局部随机数生成器
    static thread_local std::mt19937 mRng;

    // 预计算的方块哈希值（在 load 时初始化）
    std::unordered_map<std::string, uint64_t> mBlockHashes;

    // 需要处理的方块类型哈希集合（在 load 时填充）
    std::unordered_set<uint64_t> mTargetBlocks;

    // 缓存
    std::vector<std::shared_ptr<::LevelChunk>> mCachedChunks;
    int mCacheTickCounter = 0;
    static constexpr int CACHE_INVALIDATE_INTERVAL = 10;

    // 线程池
    std::unique_ptr<SimpleThreadPool> mThreadPool;
};
