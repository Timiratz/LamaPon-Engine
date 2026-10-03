#pragma once

#include "OllamaClient.h"

#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <system_error>
#include <thread>
#include <utility>

namespace LamaPonOllama
{
    // 同期HttpSendを別スレッドで行い、DLL解放前にjoinします。
    // 通信は中断できず破棄時に最大30秒待つことがあり、API操作は所有スレッド限定です。
    class ChatWorker final
    {
    public:
        using Job = std::function<ChatResult(std::stop_token)>;

        // 通信Workerを作成します。
        ChatWorker() = default;
        // 実行スレッドの所有権は複製できません。
        ChatWorker(const ChatWorker&) = delete;
        ChatWorker& operator=(const ChatWorker&) = delete;

        // 停止要求を送り、Workerスレッドの終了を待ちます。
        ~ChatWorker()
        {
            // join対象の通信スレッド
            if (m_thread.joinable())
            {
                m_thread.request_stop();
                m_thread.join();
            }
        }

        // 通信中または未取得の結果があるか返します。
        [[nodiscard]] bool Busy() const
        {
            // Cancel後は通信終了までBusyを維持します。
            return m_cancelled
                ? m_thread.joinable() && !Finished()
                : m_thread.joinable() || Finished();
        }

        // 通信を開始します(job: stop tokenを受け取る処理)。
        // 実行中なら開始せずfalseを返します。
        [[nodiscard]] bool Start(Job job)
        {
            // 同時実行を拒否します。
            if (Busy())
            {
                return false;
            }
            if (m_thread.joinable())
            {
                // 取り消したあとに終わっていたスレッドです。
                // 結果ごと片付けます。
                m_thread.join();
            }
            // ここではスレッドが動いていないので、ロックなしで初期化できます。
            m_cancelled = false;
            m_finished = false;
            m_result.reset();
            try
            {
                // 完了結果を共有状態へ保存する通信処理(stop: 停止要求)。
                m_thread = std::jthread(
                    [this, job = std::move(job)](const std::stop_token stop)
                    {
                        // Jobが返す通信結果
                        ChatResult result;
                        try
                        {
                            result = job(stop);
                        }
                        // Jobが失敗した理由
                        catch (const std::exception& exception)
                        {
                            result = Detail::Failure(ChatError::Failed, exception.what());
                        }
                        catch (...)
                        {
                            result = Detail::Failure(ChatError::Failed, "通信中に不明な例外が発生しました。");
                        }
                        // 結果状態を保護するMutexロック
                        const std::scoped_lock lock(m_mutex);
                        m_result = std::move(result);
                        m_finished = true;
                    });
            }
            // Workerスレッド作成失敗の詳細
            catch (const std::system_error& exception)
            {
                // スレッドを作れなかった場合も、通信の失敗と同じ経路で伝えます。
                m_result = Detail::Failure(ChatError::Failed, exception.what());
                m_finished = true;
            }
            return true;
        }

        // 取消されておらず結果取得が可能か返します。
        [[nodiscard]] bool Ready() const
        {
            return !m_cancelled && Finished();
        }

        // 完了結果を一度だけ取得します(未完了なら空)。
        [[nodiscard]] std::optional<ChatResult> Take()
        {
            if (!Ready())
            {
                return std::nullopt;
            }
            if (m_thread.joinable())
            {
                // 結果を書き終えた直後のスレッドなので、すぐに戻ります。
                m_thread.join();
            }
            // 共有結果状態を保護するMutexロック
            const std::scoped_lock lock(m_mutex);
            m_finished = false;
            return std::exchange(m_result, std::nullopt);
        }

        // 結果を破棄するよう通知します(進行中のHttpSendは完了まで継続します)。
        void Cancel() noexcept
        {
            m_thread.request_stop();
            m_cancelled = true;
        }

    private:
        // 通信スレッドが結果を書き終えたか返します。
        [[nodiscard]] bool Finished() const
        {
            // 完了状態を読むMutexロック
            const std::scoped_lock lock(m_mutex);
            return m_finished;
        }

        // 完了状態と結果を守るMutex
        mutable std::mutex m_mutex;
        // Workerスレッドが生成した結果
        std::optional<ChatResult> m_result;
        // Jobが結果を書き終えたか
        bool m_finished{};
        // 取消後に結果を破棄する状態
        bool m_cancelled{};
        // このWorkerが所有する通信スレッド
        std::jthread m_thread;
    };
}
