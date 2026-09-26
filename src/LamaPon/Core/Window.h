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
        using ResizeCallback = std::function<void(std::uint32_t, std::uint32_t)>;
        using MessageCallback = std::function<bool(HWND, UINT, WPARAM, LPARAM)>;
        // ウィンドウを閉じてよいかを返すコールバック
        // （falseで閉じるのを中止。未保存確認などに使用）。
        using CloseCallback = std::function<bool()>;

        Window(std::wstring title, std::uint32_t width, std::uint32_t height);
        ~Window();

        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        void Create(HINSTANCE instance);
        // クライアント領域のサイズを変更します。作成前にも指定できます。
        [[nodiscard]] bool SetClientSize(std::uint32_t width, std::uint32_t height);
        void SetResizeCallback(ResizeCallback callback) { m_resizeCallback = std::move(callback); }
        void SetMessageCallback(MessageCallback callback) { m_messageCallback = std::move(callback); }
        void SetCloseCallback(CloseCallback callback) { m_closeCallback = std::move(callback); }

        [[nodiscard]] HWND Handle() const noexcept { return m_handle; }
        [[nodiscard]] std::uint32_t ClientWidth() const noexcept { return m_clientWidth; }
        [[nodiscard]] std::uint32_t ClientHeight() const noexcept { return m_clientHeight; }
        [[nodiscard]] bool IsMinimized() const noexcept { return m_minimized; }

    private:
        static LRESULT CALLBACK WindowProcedure(
            HWND window,
            UINT message,
            WPARAM wParam,
            LPARAM lParam);

        LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

        std::wstring m_title;
        std::uint32_t m_clientWidth;
        std::uint32_t m_clientHeight;
        HWND m_handle{};
        HINSTANCE m_instance{};
        bool m_minimized{};
        ResizeCallback m_resizeCallback;
        MessageCallback m_messageCallback;
        CloseCallback m_closeCallback;
    };
}
