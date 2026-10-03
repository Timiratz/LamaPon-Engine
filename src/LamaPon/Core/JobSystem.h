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
    // 相互に独立した要素を並列処理し、外部スレッドからの同時ParallelFor呼び出しを禁止する。
    class JobSystem final
    {
    public:
        // エンジン共通のスレッドプールを返します。
        [[nodiscard]] static JobSystem& Instance();

        // ワーカースレッドの複製を禁止します。
        JobSystem(const JobSystem&) = delete;
        // ワーカースレッドのコピー代入を禁止します。
        JobSystem& operator=(const JobSystem&) = delete;
        // 全ワーカーを停止して終了を待ちます。
        ~JobSystem();

        // ネスト呼び出しは逐次実行し、全区間完了後に最初に記録した例外を再送出する。
        // 区間を並列処理して完了を待つ(count: 要素数, grainSize: 最低1要素の区間幅, body: 開始を含み終端を含まない処理)。
        void ParallelFor(
            std::size_t count,
            std::size_t grainSize,
            const std::function<
                void(std::size_t, std::size_t)>& body);

        // 呼び出しスレッドを除いたワーカー数を返します。
        [[nodiscard]] std::size_t
            WorkerCount() const noexcept
        {
            return m_workers.size();
        }

    private:
        // 論理コア数に応じてワーカーを起動します。
        JobSystem();

        // 新しいジョブを待ち、残り区間を処理します。
        void WorkerLoop();
        // 区間を1つ処理し、残っていなければfalseを返します。
        bool RunChunk();

        // 所有するワーカースレッド
        std::vector<std::thread> m_workers;
        // ジョブ状態の保護
        std::mutex m_mutex;
        // 新規ジョブと終了の通知
        std::condition_variable m_workAvailable;
        // 全区間の完了通知
        std::condition_variable m_workDone;
        // ワーカーの終了要求
        bool m_shutdown{};

        // bodyは全区間完了まで存続させる。
        // 借用する実行中ジョブの処理
        const std::function<
            void(std::size_t, std::size_t)>* m_body{};
        // 実行中ジョブの要素数
        std::size_t m_count{};
        // 1区間の要素数
        std::size_t m_grainSize{};
        // 新しいジョブを識別する世代
        std::uint64_t m_generation{};
        // 次に引き受ける区間番号
        std::atomic<std::size_t> m_nextChunk{};
        // 未完了の区間数
        std::atomic<std::size_t> m_pendingChunks{};
        // 呼び出し元へ再送出する例外
        std::exception_ptr m_firstException;
        // 最初の例外の保護
        std::mutex m_exceptionMutex;
    };
}
