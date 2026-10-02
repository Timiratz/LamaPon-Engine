#pragma once

#include <coroutine>
#include <exception>
#include <functional>
#include <utility>

namespace LamaPon
{


    // 指定秒数待ちます（timeScale適用後のゲーム時間）。
    struct WaitForSeconds final
    {
        // 倍率適用後の待機秒数
        float seconds{};
    };

    // 次のフレームまで待ちます。
    struct WaitForNextFrame final
    {
    };

    // 条件がtrueになるまで待ちます。
    struct WaitUntil final
    {
        // 再開を許可する待機条件
        std::function<bool()> condition;
    };

    // 条件がtrueの間待ちます。
    struct WaitWhile final
    {
        // 待機を継続する空でない判定
        std::function<bool()> condition;
    };

    // 最初の実行はScriptの開始処理に任せ、Release後の再開・破棄は移譲先に任せる。
    class Coroutine final
    {
    public:
        struct promise_type final
        {

            // 再開までの残りゲーム秒数
            float remainingSeconds{};
            // 再開を許可する待機条件
            std::function<bool()> condition;
            // Scriptへ再送出する例外
            std::exception_ptr exception;

            // 現在のpromiseを所有するコルーチンを返す。
            Coroutine get_return_object()
            {
                return Coroutine{
                    std::coroutine_handle<
                        promise_type>::from_promise(
                            *this) };
            }
            // Scriptが開始するまで本体を実行しない。
            std::suspend_always
                initial_suspend() noexcept
            {
                return {};
            }
            // 完了後もScriptまたは所有者がハンドルを破棄するまで保持する。
            std::suspend_always
                final_suspend() noexcept
            {
                return {};
            }
            // 戻り値を持たず本体を終了する。
            void return_void() noexcept
            {
            }
            // 本体の例外を保持しScriptの再開元へ再送出させる。
            void unhandled_exception() noexcept
            {
                exception = std::current_exception();
            }

            // 時間待ちを設定して中断する(wait: 倍率適用後の待機秒数)。
            std::suspend_always await_transform(
                const WaitForSeconds wait) noexcept
            {
                remainingSeconds = wait.seconds;
                condition = nullptr;
                return {};
            }
            // 次のゲーム更新まで中断する。
            std::suspend_always await_transform(
                WaitForNextFrame) noexcept
            {
                remainingSeconds = 0.0f;
                condition = nullptr;
                return {};
            }
            // 条件がtrueになるまで中断する(wait: 再開を許可する条件)。
            std::suspend_always await_transform(
                WaitUntil wait)
            {
                remainingSeconds = 0.0f;
                condition = std::move(wait.condition);
                return {};
            }
            // 条件がfalseになるまで中断する(wait: 空でない待機条件)。
            std::suspend_always await_transform(
                WaitWhile wait)
            {
                remainingSeconds = 0.0f;
                // 待機条件を反転して再開を判定する(inner: 空でない元の待機条件)。
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

        // ハンドルを持たない空のコルーチンを作る。
        Coroutine() = default;
        // ハンドルの所有権を受け取る(handle: 未所有のコルーチンハンドル)。
        explicit Coroutine(const Handle handle) noexcept
            : m_handle(handle)
        {
        }
        // 移動元を空にしてハンドルの所有を引き継ぐ(other: 移動元)。
        Coroutine(Coroutine&& other) noexcept
            : m_handle(
                std::exchange(other.m_handle, {}))
        {
        }
        // 現在のハンドルを破棄して所有を移す(other: 移動元)。
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
        // コルーチンハンドルの二重所有を禁止する。
        Coroutine(const Coroutine&) = delete;
        // コルーチンハンドルのコピー代入を禁止する。
        Coroutine& operator=(const Coroutine&) = delete;
        // 未移譲のコルーチンハンドルを破棄する。
        ~Coroutine()
        {
            Destroy();
        }

        // Scriptなどへハンドルの所有と破棄責任を移す。
        [[nodiscard]] Handle Release() noexcept
        {
            return std::exchange(m_handle, {});
        }

    private:
        // 所有中のコルーチンハンドルを破棄して空にする。
        void Destroy() noexcept
        {
            if (m_handle)
            {
                m_handle.destroy();
                m_handle = {};
            }
        }

        // 所有するコルーチンハンドル
        Handle m_handle;
    };
}
