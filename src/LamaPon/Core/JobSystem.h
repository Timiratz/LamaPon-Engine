#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace LamaPon
{
    // エンジン共通のワーカースレッドプールです。
    // まずはParallelFor（データ並列）だけを提供します。
    // 要素同士が完全に独立した読み取り中心の処理
    // （カリング、パーティクル等）に使ってください。
    class JobSystem final
    {
    public:
        [[nodiscard]] static JobSystem& Instance();

        JobSystem(const JobSystem&) = delete;
        JobSystem& operator=(const JobSystem&) = delete;
        ~JobSystem();

        // [0, count) をgrainSizeごとの区間へ分割し、
        // body(begin, end) をワーカー＋呼び出しスレッドで
        // 並列実行して完了まで待ちます。
        // - countが小さいときやワーカーが無いときは逐次実行
        // - ParallelForの中からの呼び出しは自動で逐次実行
        //   （ネストによるデッドロックを防ぎます）
        // - body内の例外は最初の1つが呼び出し元へ再送出されます
        void ParallelFor(
            std::size_t count,
            std::size_t grainSize,
            const std::function<
                void(std::size_t, std::size_t)>& body);

        [[nodiscard]] std::size_t
            WorkerCount() const noexcept
        {
            return m_workers.size();
        }

    private:
        JobSystem();

        void WorkerLoop();
        // 現在のジョブの区間を1つ引き受けて実行します。
        // 区間が残っていなければfalseを返します。
        bool RunChunk();

        std::vector<std::thread> m_workers;
        std::mutex m_mutex;
        std::condition_variable m_workAvailable;
        std::condition_variable m_workDone;
        bool m_shutdown{};

        // 実行中ジョブの状態（同時に走るジョブは1つだけ）。
        const std::function<
            void(std::size_t, std::size_t)>* m_body{};
        std::size_t m_count{};
        std::size_t m_grainSize{};
        std::uint64_t m_generation{};
        std::atomic<std::size_t> m_nextChunk{};
        std::atomic<std::size_t> m_pendingChunks{};
        std::exception_ptr m_firstException;
        std::mutex m_exceptionMutex;
    };
}
