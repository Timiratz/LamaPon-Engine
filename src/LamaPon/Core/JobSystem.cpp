#include "LamaPon/Core/JobSystem.h"

#include <algorithm>

namespace
{
    // 並列処理への再入防止
    thread_local bool t_insideParallelFor = false;
}

namespace LamaPon
{
    JobSystem& JobSystem::Instance()
    {
        // エンジン共通のスレッドプール
        static JobSystem instance;
        return instance;
    }

    JobSystem::JobSystem()
    {
        // 使用可能な論理コア数
        const auto hardware =
            std::thread::hardware_concurrency();
        // 呼び出し側も実行するため、ワーカー数は論理コア数から1を引き1〜8に制限する。
        // 起動するワーカー数
        const std::size_t workerCount = std::clamp<
            std::size_t>(
            hardware > 1 ? hardware - 1 : 1,
            1,
            8);
        m_workers.reserve(workerCount);
        // 起動するワーカー番号
        for (std::size_t index = 0;
            index < workerCount;
            ++index)
        {
            // ワーカー上でジョブの待機と区間処理を開始します。
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
            // 終了要求を書き込む間の保護
            std::scoped_lock lock(m_mutex);
            m_shutdown = true;
        }
        m_workAvailable.notify_all();
        // 終了を待つワーカー
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
        // このワーカーが確認した世代
        std::uint64_t seenGeneration = 0;
        for (;;)
        {
            {
                // ジョブ通知を待つための保護
                std::unique_lock lock(m_mutex);
                // 未処理の世代または終了要求が現れるまで待ちます。
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
        // このスレッドが引き受ける区間
        const auto chunkIndex =
            m_nextChunk.fetch_add(1);
        // 区間の開始添字
        const auto begin = chunkIndex * m_grainSize;
        if (begin >= m_count)
        {
            return false;
        }
        // 区間の終端添字、範囲に含めない
        const auto end =
            std::min(begin + m_grainSize, m_count);
        try
        {
            (*m_body)(begin, end);
        }
        catch (...)
        {
            // 最初の例外を記録する間の保護
            std::scoped_lock lock(m_exceptionMutex);
            if (!m_firstException)
            {
                m_firstException =
                    std::current_exception();
            }
        }
        // 起こし損ねを避けるため、完了通知は待機側と同じmutexの保持中に行う。
        if (m_pendingChunks.fetch_sub(1) == 1)
        {
            // 完了通知と待機開始の排他
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
        // 最低1要素とした区間サイズ
        const auto effectiveGrain =
            std::max<std::size_t>(grainSize, 1);
        // 今回のジョブの区間数
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
            // ジョブ状態を公開する間の保護
            std::scoped_lock lock(m_mutex);
            m_body = &body;
            m_count = count;
            m_grainSize = effectiveGrain;
            m_firstException = nullptr;
            m_pendingChunks.store(chunkCount);
            // 前ジョブのワーカーが離脱するまで区間枯渇を維持し、他の状態を整えた最後に再開する。
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
            // 全区間の完了を待つための保護
            std::unique_lock lock(m_mutex);
            // 未完了の区間がなくなるまで待ちます。
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
