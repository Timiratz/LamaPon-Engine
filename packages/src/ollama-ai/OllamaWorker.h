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
    // 通信を別スレッドで実行し、結果をScriptのスレッドへ渡します。
    // エンジンのHttpSendは応答が届くまで戻らないので、Updateから直接呼ぶと
    // その間ゲームが止まります。
    //
    // スレッドはこのオブジェクトが持ち、破棄するときに必ず終了を待ちます。
    // Game Moduleを差し替えるとき、エンジンはScriptを破棄してからDLLを解放
    // します。切り離した（detach）スレッドは解放済みのコードを実行して落ちる
    // ので、使いません。HttpSendは途中で止められないため、通信中に破棄すると
    // 最大で受信タイムアウト（約30秒）のあいだ待ちます。
    //
    // Start / Busy / Ready / Take / Cancel は、持ち主のスレッドだけから呼びます。
    class ChatWorker final
    {
    public:
        using Job = std::function<ChatResult(std::stop_token)>;

        ChatWorker() = default;
        ChatWorker(const ChatWorker&) = delete;
        ChatWorker& operator=(const ChatWorker&) = delete;

        ~ChatWorker()
        {
            if (m_thread.joinable())
            {
                m_thread.request_stop();
                m_thread.join();
            }
        }

        // 通信中か、受け取っていない結果が残っている間はtrueです。
        // 取り消したあとは結果を捨てるので、通信が終わった時点でfalseに戻ります。
        [[nodiscard]] bool Busy() const
        {
            return m_cancelled
                ? m_thread.joinable() && !Finished()
                : m_thread.joinable() || Finished();
        }

        // 同時に実行するのは1件までです。Busyの間はfalseを返し、何もしません。
        [[nodiscard]] bool Start(Job job)
        {
            if (Busy())
            {
                return false;
            }
            if (m_thread.joinable())
            {
                // 取り消したあとに終わっていたスレッドです。結果ごと片付けます。
                m_thread.join();
            }
            // ここではスレッドが動いていないので、ロックなしで初期化できます。
            m_cancelled = false;
            m_finished = false;
            m_result.reset();
            try
            {
                m_thread = std::jthread(
                    [this, job = std::move(job)](const std::stop_token stop)
                    {
                        ChatResult result;
                        try
                        {
                            result = job(stop);
                        }
                        catch (const std::exception& exception)
                        {
                            result = Detail::Failure(ChatError::Failed, exception.what());
                        }
                        catch (...)
                        {
                            result = Detail::Failure(ChatError::Failed, "通信中に不明な例外が発生しました。");
                        }
                        const std::scoped_lock lock(m_mutex);
                        m_result = std::move(result);
                        m_finished = true;
                    });
            }
            catch (const std::system_error& exception)
            {
                // スレッドを作れなかった場合も、通信の失敗と同じ経路で伝えます。
                m_result = Detail::Failure(ChatError::Failed, exception.what());
                m_finished = true;
            }
            return true;
        }

        [[nodiscard]] bool Ready() const
        {
            return !m_cancelled && Finished();
        }

        // 結果を1度だけ取り出します。まだ届いていなければ空です。
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
            const std::scoped_lock lock(m_mutex);
            m_finished = false;
            return std::exchange(m_result, std::nullopt);
        }

        // 返答がいらなくなったときに呼びます。進行中の通信は止められないので、
        // 次の段階へ進まないよう伝え、届いた結果は捨てます。
        void Cancel() noexcept
        {
            m_thread.request_stop();
            m_cancelled = true;
        }

    private:
        [[nodiscard]] bool Finished() const
        {
            const std::scoped_lock lock(m_mutex);
            return m_finished;
        }

        mutable std::mutex m_mutex;
        // m_result と m_finished は m_mutex で守ります。
        std::optional<ChatResult> m_result;
        bool m_finished{};
        bool m_cancelled{};
        std::jthread m_thread;
    };
}
