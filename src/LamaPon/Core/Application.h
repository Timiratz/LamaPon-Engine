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
#include <utility>

namespace LamaPon
{
    class GameModuleHost;
    class InputSystem;
    class OnlineServices;
    class NetworkSession;
    class NetworkSceneBridge;
    class PlayerPrefs;
    class SaveDataStore;
    class Scene;

    // Initialize完了後に各サービスを参照し、ウィンドウ操作とメインループを同じスレッドで行う。
    class Application final
    {
    public:
        // 起動設定を保持します(title: 窓のタイトル, width: 横幅px, height: 高さpx, persistenceName: 保存領域名、空ならタイトル)。
        LAMAPON_API Application(
            std::wstring title = L"LamaPon",
            std::uint32_t width = 1280,
            std::uint32_t height = 720,
            std::string persistenceName = {});
        // 登録済みサービスを解除し、保存して描画資源を破棄します。
        LAMAPON_API ~Application();

        // ウィンドウと各サービスの複製を禁止します。
        Application(const Application&) = delete;
        // ウィンドウと各サービスのコピー代入を禁止します。
        Application& operator=(const Application&) = delete;

        // DirectX 11で起動します(instance: プロセスのインスタンス)。
        LAMAPON_API void Initialize(HINSTANCE instance);
        // 指定した描画APIで起動します(instance: プロセスのインスタンス, requestedApi: 起動する描画API)。
        LAMAPON_API void Initialize(
            HINSTANCE instance,
            RenderingApi requestedApi);
        // 指定APIで起動します(instance: プロセスのインスタンス, requestedApi: 起動する描画API, startupProfile: 互換用、現在は未使用)。
        LAMAPON_API void Initialize(
            HINSTANCE instance,
            RenderingApi requestedApi,
            GraphicsStartupProfile startupProfile);
        // 初期化後に画面レイヤーを受け取ります(layer: 空でない所有レイヤー)。
        LAMAPON_API void AttachLayer(
            std::unique_ptr<ApplicationLayer> layer);
        // 初回シーン読み込み中のロゴ表示を設定します(enabled: ロゴを表示するか)。
        LAMAPON_API void SetStartupSplashScreenEnabled(
            bool enabled) noexcept;
        // 初期化後に終了まで更新と描画を繰り返し、終了コードを返します。
        LAMAPON_API int Run();

        // 実行中のシーンを返します。
        [[nodiscard]] LAMAPON_API Scene&
            ActiveScene() const;
        // 描画サービスを返します。
        [[nodiscard]] GraphicsDevice& Graphics() noexcept { return m_graphics; }
        // メインウィンドウのハンドルを返します。
        [[nodiscard]] HWND WindowHandle() const noexcept
        {
            return m_window.Handle();
        }
        // ゲームの表示サイズを変更します(width: 横幅px, height: 高さpx)。
        // 1〜16384pxの各辺を受け付け、レイヤー装着時は再生中のみゲームビューへ反映する。
        [[nodiscard]] LAMAPON_API bool SetWindowSize(
            std::uint32_t width, std::uint32_t height);
        // ゲームの表示サイズをピクセル単位で返します。
        [[nodiscard]] LAMAPON_API std::pair<std::uint32_t, std::uint32_t>
            WindowSize() const noexcept;
        // ゲーム入力のサービスを返します。
        [[nodiscard]] LAMAPON_API InputSystem& Input() const;
        // C++スクリプトの読み込みサービスを返します。
        [[nodiscard]] LAMAPON_API GameModuleHost&
            GameModule() const;
        // 小規模な設定値の保存領域を返します。
        [[nodiscard]] LAMAPON_API PlayerPrefs&
            Preferences() const;
        // セーブ文書の保存サービスを返します。
        [[nodiscard]] LAMAPON_API SaveDataStore& Saves() const;
        // オンライン機能のサービスを返します。
        [[nodiscard]] LAMAPON_API OnlineServices& Online() const;
        // ネットワークセッションを返します。
        [[nodiscard]] LAMAPON_API NetworkSession& Network() const;
        // シーンとネットワークの同期サービスを返します。
        [[nodiscard]] LAMAPON_API NetworkSceneBridge& NetworkScene() const;
        // 現フレームのキーボード状態を返します。
        [[nodiscard]] const DirectX::Keyboard::State&
            KeyboardState() const;

        // 画面の背景色を設定します(red: 赤成分, green: 緑成分, blue: 青成分, alpha: 不透明度)。
        LAMAPON_API void SetClearColor(
            float red,
            float green,
            float blue,
            float alpha = 1.0f) noexcept;

    private:
        // 同じ描画エラーの連続出力を抑止します(message: エラー内容)。
        void ReportRenderFailure(const std::string& message);
        // 保留したウィンドウサイズ変更を描画サービスへ適用します。
        void ApplyPendingResize();

        // メインウィンドウ
        Window m_window;
        // 描画サービス
        GraphicsDevice m_graphics;
        // F1で切り替える実行時表示
        DebugOverlay m_debugOverlay;
        // 実行中のシーン
        std::unique_ptr<Scene> m_scene;
        // 所有する画面レイヤー
        std::unique_ptr<ApplicationLayer> m_layer;
        // C++スクリプトの読み込み管理
        std::unique_ptr<GameModuleHost> m_gameModule;
        // 設定値の保存管理
        std::unique_ptr<PlayerPrefs> m_playerPrefs;
        // セーブ文書の保存管理
        std::unique_ptr<SaveDataStore> m_saveData;
        // オンライン機能の管理
        std::unique_ptr<OnlineServices> m_onlineServices;
        // ネットワーク接続の管理
        std::unique_ptr<NetworkSession> m_networkSession;
        // シーン同期の管理
        std::unique_ptr<NetworkSceneBridge> m_networkSceneBridge;
        // 永続データの保存領域名
        std::string m_persistenceName;
        // 初回読み込み中のロゴ表示
        bool m_startupSplashScreenEnabled{};
        // 直前に通知した描画エラー
        std::string m_lastRenderFailure;
        // 画面の背景色RGBA
        float m_clearColor[4]{ 0.025f, 0.035f, 0.055f, 1.0f };
        // このインスタンスのCOM初期化
        bool m_comInitialized{};
        // サイズ変更の保留状態
        bool m_resizePending{};
        // 保留中の横幅px
        std::uint32_t m_pendingWidth{};
        // 保留中の高さpx
        std::uint32_t m_pendingHeight{};
    };
}
