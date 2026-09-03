#pragma once

#include <coroutine>
#include <exception>
#include <functional>
#include <utility>

namespace LamaPon
{
    // ---- co_awaitで使う待機条件 ----

    // 指定秒数待ちます（timeScale適用後のゲーム時間）。
    struct WaitForSeconds final
    {
        float seconds{};
    };

    // 次のフレームまで待ちます。
    struct WaitForNextFrame final
    {
    };

    // 条件がtrueになるまで待ちます。
    struct WaitUntil final
    {
        std::function<bool()> condition;
    };

    // 条件がtrueの間待ちます。
    struct WaitWhile final
    {
        std::function<bool()> condition;
    };

    // Scriptのメンバー関数を「Coroutine戻り値＋co_await」で書くと、
    // 途中で待機できる処理になります。
    //
    //   LamaPon::Coroutine Intro()
    //   {
    //       co_await LamaPon::WaitForSeconds{ 1.0f };
    //       // 1秒後の処理
    //   }
    //   // 開始はScript::StartCoroutine(Intro());
    class Coroutine final
    {
    public:
        struct promise_type final
        {
            // 現在の待機状態（Script::TickCoroutinesが参照）。
            float remainingSeconds{};
            std::function<bool()> condition;
            std::exception_ptr exception;

            Coroutine get_return_object()
            {
                return Coroutine{
                    std::coroutine_handle<
                        promise_type>::from_promise(
                            *this) };
            }
            std::suspend_always
                initial_suspend() noexcept
            {
                return {};
            }
            std::suspend_always
                final_suspend() noexcept
            {
                return {};
            }
            void return_void() noexcept
            {
            }
            void unhandled_exception() noexcept
            {
                exception = std::current_exception();
            }

            std::suspend_always await_transform(
                const WaitForSeconds wait) noexcept
            {
                remainingSeconds = wait.seconds;
                condition = nullptr;
                return {};
            }
            std::suspend_always await_transform(
                WaitForNextFrame) noexcept
            {
                remainingSeconds = 0.0f;
                condition = nullptr;
                return {};
            }
            std::suspend_always await_transform(
                WaitUntil wait)
            {
                remainingSeconds = 0.0f;
                condition = std::move(wait.condition);
                return {};
            }
            std::suspend_always await_transform(
                WaitWhile wait)
            {
                remainingSeconds = 0.0f;
                condition =
                    [inner = std::move(wait.condition)]
                    {
                        return !inner();
                    };
                return {};
            }
        };

        using Handle =
            std::coroutine_handle<promise_type>;

        Coroutine() = default;
        explicit Coroutine(const Handle handle) noexcept
            : m_handle(handle)
        {
        }
        Coroutine(Coroutine&& other) noexcept
            : m_handle(
                std::exchange(other.m_handle, {}))
        {
        }
        Coroutine& operator=(Coroutine&& other) noexcept
        {
            if (this != &other)
            {
                Destroy();
                m_handle =
                    std::exchange(other.m_handle, {});
            }
            return *this;
        }
        Coroutine(const Coroutine&) = delete;
        Coroutine& operator=(const Coroutine&) = delete;
        ~Coroutine()
        {
            Destroy();
        }

        // 所有権を呼び出し側（Script::StartCoroutine）へ渡します。
        [[nodiscard]] Handle Release() noexcept
        {
            return std::exchange(m_handle, {});
        }

    private:
        void Destroy() noexcept
        {
            if (m_handle)
            {
                m_handle.destroy();
                m_handle = {};
            }
        }

        Handle m_handle;
    };
}
