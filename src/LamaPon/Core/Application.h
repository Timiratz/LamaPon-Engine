#pragma once

#include "LamaPon/Core/Api.h"
#include "LamaPon/Core/ApplicationLayer.h"
#include "LamaPon/Core/DebugOverlay.h"
#include "LamaPon/Core/Window.h"
#include "LamaPon/Graphics/GraphicsDevice.h"

#include <Keyboard.h>

#include <cstdint>
#include <memory>
#include <string>

namespace LamaPon
{
    class GameModuleHost;
    class InputSystem;
    class PlayerPrefs;
    class SaveDataStore;
    class Scene;

    class Application final
    {
    public:
        LAMAPON_API Application(
            std::wstring title = L"LamaPon",
            std::uint32_t width = 1280,
            std::uint32_t height = 720,
            std::string persistenceName = {});
        LAMAPON_API ~Application();

        Application(const Application&) = delete;
        Application& operator=(const Application&) = delete;

        LAMAPON_API void Initialize(HINSTANCE instance);
        LAMAPON_API void AttachLayer(
            std::unique_ptr<ApplicationLayer> layer);
        // 最初のシーンを読み込み終えるまで起動ロゴを表示します。
        LAMAPON_API void SetStartupSplashScreenEnabled(
            bool enabled) noexcept;
        LAMAPON_API int Run();

        [[nodiscard]] LAMAPON_API Scene&
            ActiveScene() const;
        [[nodiscard]] GraphicsDevice& Graphics() noexcept { return m_graphics; }
        [[nodiscard]] HWND WindowHandle() const noexcept
        {
            return m_window.Handle();
        }
        [[nodiscard]] LAMAPON_API InputSystem& Input() const;
        [[nodiscard]] LAMAPON_API GameModuleHost&
            GameModule() const;
        [[nodiscard]] LAMAPON_API PlayerPrefs&
            Preferences() const;
        [[nodiscard]] LAMAPON_API SaveDataStore& Saves() const;
        [[nodiscard]] const DirectX::Keyboard::State&
            KeyboardState() const;

        LAMAPON_API void SetClearColor(
            float red,
            float green,
            float blue,
            float alpha = 1.0f) noexcept;

    private:
        // 描画が失敗したフレームを1回だけ知らせます。落とさずに
        // 続けるので、知らせないと「絵が出ない理由が分からない」に
        // なります。
        void ReportRenderFailure(const std::string& message);

        Window m_window;
        GraphicsDevice m_graphics;
        // エディターなしゲームでF1切り替えのデバッグ表示。
        DebugOverlay m_debugOverlay;
        std::unique_ptr<Scene> m_scene;
        std::unique_ptr<ApplicationLayer> m_layer;
        std::unique_ptr<GameModuleHost> m_gameModule;
        std::unique_ptr<PlayerPrefs> m_playerPrefs;
        std::unique_ptr<SaveDataStore> m_saveData;
        std::string m_persistenceName;
        bool m_startupSplashScreenEnabled{};
        // 直前に知らせた描画エラー。同じ内容は繰り返し出しません。
        std::string m_lastRenderFailure;
        float m_clearColor[4]{ 0.025f, 0.035f, 0.055f, 1.0f };
        bool m_comInitialized{};
    };
}
