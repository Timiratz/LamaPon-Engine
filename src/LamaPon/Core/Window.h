#pragma once

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <string>

namespace LamaPon
{
    class Window final
    {
    public:
        // 変更後のクライアント幅と高さをピクセルで通知します。
        using ResizeCallback = std::function<void(std::uint32_t, std::uint32_t)>;
        // Win32メッセージを渡し、trueなら既定処理を省略します。
        using MessageCallback = std::function<bool(HWND, UINT, WPARAM, LPARAM)>;
        // 閉じてよいかを返し、falseなら終了要求を取り消します。
        using CloseCallback = std::function<bool()>;

        // ウィンドウの初期設定を保持します(title: タイトル, width: 初期幅ピクセル, height: 初期高さピクセル)。
        Window(std::wstring title, std::uint32_t width, std::uint32_t height);
        // 所有するウィンドウを破棄し、ウィンドウクラスの登録を解除します。
        ~Window();

        // ウィンドウの所有権のコピーを禁止します。
        Window(const Window&) = delete;
        // ウィンドウのコピー代入を禁止します。
        Window& operator=(const Window&) = delete;

        // ネイティブウィンドウを作成して表示します(instance: 実行モジュールのハンドル)。
        // クラス登録またはウィンドウ作成の失敗にはruntime_errorを送出します。
        void Create(HINSTANCE instance);
        // クライアント領域を指定サイズにします(width: 幅ピクセル, height: 高さピクセル)。
        // 各寸法は1～16384に限り、作成前は設定だけ保存し、作成後は実測の一致を返します。
        [[nodiscard]] bool SetClientSize(std::uint32_t width, std::uint32_t height);
        // サイズ変更の通知先を設定します(callback: 通知関数、空なら解除)。
        void SetResizeCallback(ResizeCallback callback) { m_resizeCallback = std::move(callback); }
        // Win32メッセージの通知先を設定します(callback: メッセージ処理関数、空なら解除)。
        // ゲーム入力の転送とWM_SIZEの実測更新は、このコールバックより先に行います。
        void SetMessageCallback(MessageCallback callback) { m_messageCallback = std::move(callback); }
        // 終了要求の確認先を設定します(callback: 終了可否を返す関数、空なら確認を省略)。
        void SetCloseCallback(CloseCallback callback) { m_closeCallback = std::move(callback); }

        // ネイティブウィンドウのハンドルを返し、未作成や破棄後はnullを返します。
        [[nodiscard]] HWND Handle() const noexcept { return m_handle; }
        // クライアント領域の幅をピクセルで返します。
        [[nodiscard]] std::uint32_t ClientWidth() const noexcept { return m_clientWidth; }
        // クライアント領域の高さをピクセルで返します。
        [[nodiscard]] std::uint32_t ClientHeight() const noexcept { return m_clientHeight; }
        // ウィンドウが最小化されているかを返します。
        [[nodiscard]] bool IsMinimized() const noexcept { return m_minimized; }

    private:
        // メッセージを対応するWindowへ転送します(window: 受信ウィンドウ, message: メッセージ番号, wParam: メッセージ第1引数, lParam: メッセージ第2引数)。
        static LRESULT CALLBACK WindowProcedure(
            HWND window,
            UINT message,
            WPARAM wParam,
            LPARAM lParam);

        // 入力・サイズ変更・終了要求を処理します(message: メッセージ番号, wParam: メッセージ第1引数, lParam: メッセージ第2引数)。
        LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

        // ウィンドウのタイトル
        std::wstring m_title;
        // クライアント領域の幅ピクセル
        std::uint32_t m_clientWidth;
        // クライアント領域の高さピクセル
        std::uint32_t m_clientHeight;
        // 所有するウィンドウのハンドル
        HWND m_handle{};
        // クラスを登録した実行モジュール
        HINSTANCE m_instance{};
        // ウィンドウが最小化されているか
        bool m_minimized{};
        // サイズ変更の通知関数
        ResizeCallback m_resizeCallback;
        // メッセージの通知関数
        MessageCallback m_messageCallback;
        // 終了可否を確認する関数
        CloseCallback m_closeCallback;
    };
}
