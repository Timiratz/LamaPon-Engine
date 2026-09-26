#include "LamaPon/Core/Window.h"
#include "LamaPon/Resources/WindowsResource.h"
#include "LamaPon/Input/InputSystem.h"

#include <Keyboard.h>

#include <stdexcept>

namespace
{
    constexpr wchar_t WindowClassName[] = L"LamaPonWindow";
}

namespace LamaPon
{
    Window::Window(
        std::wstring title,
        const std::uint32_t width,
        const std::uint32_t height)
        : m_title(std::move(title))
        , m_clientWidth(width)
        , m_clientHeight(height)
    {
    }

    Window::~Window()
    {
        if (m_handle != nullptr)
        {
            DestroyWindow(m_handle);
            m_handle = nullptr;
        }

        if (m_instance != nullptr)
        {
            UnregisterClassW(WindowClassName, m_instance);
        }
    }

    void Window::Create(const HINSTANCE instance)
    {
        m_instance = instance;

        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = WindowProcedure;
        windowClass.hInstance = m_instance;
        windowClass.hIcon = static_cast<HICON>(LoadImageW(
            m_instance,
            MAKEINTRESOURCEW(IDI_LAMAPON_ENGINE),
            IMAGE_ICON,
            GetSystemMetrics(SM_CXICON),
            GetSystemMetrics(SM_CYICON),
            LR_SHARED));
        windowClass.hIconSm = static_cast<HICON>(LoadImageW(
            m_instance,
            MAKEINTRESOURCEW(IDI_LAMAPON_ENGINE),
            IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON),
            LR_SHARED));
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        windowClass.lpszClassName = WindowClassName;

        if (RegisterClassExW(&windowClass) == 0)
        {
            throw std::runtime_error("RegisterClassExW failed.");
        }

        RECT windowRectangle{
            0,
            0,
            static_cast<LONG>(m_clientWidth),
            static_cast<LONG>(m_clientHeight)
        };
        AdjustWindowRect(&windowRectangle, WS_OVERLAPPEDWINDOW, FALSE);

        m_handle = CreateWindowExW(
            0,
            WindowClassName,
            m_title.c_str(),
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            windowRectangle.right - windowRectangle.left,
            windowRectangle.bottom - windowRectangle.top,
            nullptr,
            nullptr,
            m_instance,
            this);

        if (m_handle == nullptr)
        {
            throw std::runtime_error("CreateWindowExW failed.");
        }

        ShowWindow(m_handle, SW_SHOW);
        UpdateWindow(m_handle);
    }

    bool Window::SetClientSize(
        const std::uint32_t width,
        const std::uint32_t height)
    {
        if (width == 0 || height == 0
            || width > 16384 || height > 16384)
        {
            return false;
        }
        if (m_handle == nullptr)
        {
            m_clientWidth = width;
            m_clientHeight = height;
            return true;
        }

        if (IsZoomed(m_handle) || IsIconic(m_handle))
        {
            ShowWindow(m_handle, SW_RESTORE);
        }

        RECT rectangle{ 0, 0,
            static_cast<LONG>(width),
            static_cast<LONG>(height) };
        const auto style = static_cast<DWORD>(
            GetWindowLongPtrW(m_handle, GWL_STYLE));
        const auto extendedStyle = static_cast<DWORD>(
            GetWindowLongPtrW(m_handle, GWL_EXSTYLE));
        if (!AdjustWindowRectExForDpi(
                &rectangle, style, GetMenu(m_handle) != nullptr,
                extendedStyle, GetDpiForWindow(m_handle))
            || !SetWindowPos(
                m_handle, nullptr, 0, 0,
                rectangle.right - rectangle.left,
                rectangle.bottom - rectangle.top,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE))
        {
            return false;
        }
        // WM_SIZEで実測値と描画サイズが更新されます。
        RECT client{};
        return GetClientRect(m_handle, &client)
            && client.right - client.left == static_cast<LONG>(width)
            && client.bottom - client.top == static_cast<LONG>(height);
    }

    LRESULT CALLBACK Window::WindowProcedure(
        const HWND window,
        const UINT message,
        const WPARAM wParam,
        const LPARAM lParam)
    {
        Window* self = nullptr;

        if (message == WM_NCCREATE)
        {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<Window*>(create->lpCreateParams);
            self->m_handle = window;
            SetWindowLongPtrW(
                window,
                GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(self));
        }
        else
        {
            self = reinterpret_cast<Window*>(
                GetWindowLongPtrW(window, GWLP_USERDATA));
        }

        if (self != nullptr)
        {
            return self->HandleMessage(message, wParam, lParam);
        }

        return DefWindowProcW(window, message, wParam, lParam);
    }

    LRESULT Window::HandleMessage(
        const UINT message,
        const WPARAM wParam,
        const LPARAM lParam)
    {
        switch (message)
        {
        case WM_ACTIVATEAPP:
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            DirectX::Keyboard::ProcessMessage(message, wParam, lParam);
            InputSystem::ProcessWindowMessage(
                message,
                static_cast<std::uint64_t>(wParam),
                static_cast<std::int64_t>(lParam));
            break;

        // マウスイベントはメッセージコールバックより先に転送します。
        // エディターUIがメッセージを消費しても、1フレーム未満の押下が
        // ゲーム側へ届くようにするためです。
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        case WM_CHAR:
        case WM_IME_CHAR:
            InputSystem::ProcessWindowMessage(
                message,
                static_cast<std::uint64_t>(wParam),
                static_cast<std::int64_t>(lParam));
            break;

        default:
            break;
        }

        if (message == WM_SIZE)
        {
            m_minimized = wParam == SIZE_MINIMIZED;
            if (!m_minimized)
            {
                RECT client{};
                if (GetClientRect(m_handle, &client))
                {
                    m_clientWidth = static_cast<std::uint32_t>(
                        client.right - client.left);
                    m_clientHeight = static_cast<std::uint32_t>(
                        client.bottom - client.top);
                    if (m_resizeCallback)
                    {
                        m_resizeCallback(
                            m_clientWidth, m_clientHeight);
                    }
                }
            }
        }

        if (m_messageCallback
            && m_messageCallback(m_handle, message, wParam, lParam))
        {
            return 1;
        }

        switch (message)
        {
        case WM_SIZE:
            return 0;

        case WM_CLOSE:
            // 閉じてよいかをアプリ側へ確認します（未保存の警告など）。
            // falseが返ったら閉じるのを中止します。
            if (m_closeCallback && !m_closeCallback())
            {
                return 0;
            }
            break;

        case WM_DESTROY:
            m_handle = nullptr;
            PostQuitMessage(0);
            return 0;

        default:
            break;
        }

        return DefWindowProcW(m_handle, message, wParam, lParam);
    }
}
