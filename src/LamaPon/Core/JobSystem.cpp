#include "LamaPon/Core/JobSystem.h"

#include <algorithm>

namespace
{
    // ParallelForの中からParallelForが呼ばれたときに
    // 逐次実行へ切り替えるためのフラグです。
    thread_local bool t_insideParallelFor = false;
}

namespace LamaPon
{
    JobSystem& JobSystem::Instance()
    {
        static JobSystem instance;
        return instance;
    }

    JobSystem::JobSystem()
    {
        const auto hardware =
            std::thread::hardware_concurrency();
        // メインスレッドも区間実行へ参加するため、
        // ワーカーは論理コア数-1（1～8にクランプ）です。
        const std::size_t workerCount = std::clamp<
            std::size_t>(
            hardware > 1 ? hardware - 1 : 1,
            1,
            8);
        m_workers.reserve(workerCount);
        for (std::size_t index = 0;
            index < workerCount;
            ++index)
        {
            m_workers.emplace_back(
                [this]
                {
                    WorkerLoop();
                });
        }
    }

    JobSystem::~JobSystem()
    {
        {
            std::scoped_lock lock(m_mutex);
            m_shutdown = true;
        }
        m_workAvailable.notify_all();
        for (auto& worker : m_workers)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }
    }

    void JobSystem::WorkerLoop()
    {
        t_insideParallelFor = true;
        std::uint64_t seenGeneration = 0;
        for (;;)
        {
            {
                std::unique_lock lock(m_mutex);
                m_workAvailable.wait(
                    lock,
                    [this, seenGeneration]
                    {
                        return m_shutdown
                            || (m_body != nullptr
                                && m_generation
                                    != seenGeneration);
                    });
                if (m_shutdown)
                {
                    return;
                }
                seenGeneration = m_generation;
            }
            while (RunChunk())
            {
            }
        }
    }

    bool JobSystem::RunChunk()
    {
        const auto chunkIndex =
            m_nextChunk.fetch_add(1);
        const auto begin = chunkIndex * m_grainSize;
        if (begin >= m_count)
        {
            return false;
        }
        const auto end =
            std::min(begin + m_grainSize, m_count);
        try
        {
            (*m_body)(begin, end);
        }
        catch (...)
        {
            std::scoped_lock lock(m_exceptionMutex);
            if (!m_firstException)
            {
                m_firstException =
                    std::current_exception();
            }
        }
        if (m_pendingChunks.fetch_sub(1) == 1)
        {
            // 最後の区間が終わったら待機側を起こします。
            // 通知はmutex保持下で行い、待機側の「条件確認〜
            // スリープ」との競合による起こし損ねを防ぎます。
            std::scoped_lock lock(m_mutex);
            m_workDone.notify_all();
        }
        return true;
    }

    void JobSystem::ParallelFor(
        const std::size_t count,
        const std::size_t grainSize,
        const std::function<
            void(std::size_t, std::size_t)>& body)
    {
        if (count == 0)
        {
            return;
        }
        const auto effectiveGrain =
            std::max<std::size_t>(grainSize, 1);
        const auto chunkCount =
            (count + effectiveGrain - 1)
            / effectiveGrain;

        // ネスト時・分割の意味がない小ささなら逐次実行します。
        if (t_insideParallelFor
            || chunkCount <= 1
            || m_workers.empty())
        {
            body(0, count);
            return;
        }

        {
            std::scoped_lock lock(m_mutex);
            m_body = &body;
            m_count = count;
            m_grainSize = effectiveGrain;
            m_firstException = nullptr;
            m_pendingChunks.store(chunkCount);
            // 前ジョブの遅延ワーカーはm_nextChunkの枯渇で
            // 離脱するため、区間カウンタのリセットは他の状態を
            // すべて整えた最後に行います。
            m_nextChunk.store(0);
            ++m_generation;
        }
        m_workAvailable.notify_all();

        // 呼び出しスレッドも区間実行へ参加します。
        t_insideParallelFor = true;
        while (RunChunk())
        {
        }
        t_insideParallelFor = false;

        {
            std::unique_lock lock(m_mutex);
            m_workDone.wait(
                lock,
                [this]
                {
                    return m_pendingChunks.load() == 0;
                });
            m_body = nullptr;
        }

        if (m_firstException)
        {
            std::rethrow_exception(m_firstException);
        }
    }
}
