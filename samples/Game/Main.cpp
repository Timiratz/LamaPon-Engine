#include "LamaPon/LamaPon.h"

#include <Windows.h>
#include <shellapi.h>

#include <exception>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace
{
    // 起動引数にflagがあるか調べます(flag: 照合するフラグ)。
    bool HasCommandLineFlag(const std::wstring_view flag)
    {
        // Windowsから受け取る引数の個数
        int argumentCount{};
        // Windowsが確保した引数配列
        auto** argumentValues = CommandLineToArgvW(
            GetCommandLineW(),
            &argumentCount);
        // 引数配列が取得できなければ不一致です。
        if (argumentValues == nullptr)
        {
            return false;
        }
        // 指定フラグが見つかったか
        bool found = false;
        // 実行ファイル名を除く引数を探します。
        for (int index = 1; index < argumentCount; ++index)
        {
            // 現在の引数が指定フラグか
            if (argumentValues[index] == flag)
            {
                found = true;
                break;
            }
        }
        LocalFree(argumentValues);
        return found;
    }
}

// Windowsエントリからゲームを起動します(instance: Win32アプリケーション識別子)。
int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    int)
{
    // Startup例外のCrash診断出力先を設定します。
    LamaPon::CrashReporter::Install(
        LamaPon::ExecutableDirectory() / L"Crashes",
        "LamaPonGame");
    // Startup検証モードの指定
    const bool validateStartup = HasCommandLineFlag(L"--validate-startup");
    // 起動時例外を診断ファイルへ記録します。
    try
    {
        // GPUの代わりにWARPで描画します。
        if (HasCommandLineFlag(L"--warp"))
        {
            LamaPon::GraphicsDevice::SetPreferWarpAdapter(true);
        }

        // 実行ファイルの隣から読み込んだゲーム設定
        const LamaPon::ProjectSettings settings =
            LamaPon::LoadProjectSettings(
                LamaPon::ExecutableDirectory()
                / L"LamaPonGame.json");
        // 設定に基づくゲームアプリケーション
        LamaPon::Application application(
            LamaPon::Utf8ToWide(settings.gameName),
            settings.windowWidth,
            settings.windowHeight,
            settings.gameName);

        LamaPon::SetGraphicsBackendPackageAssetRoot(
            LamaPon::ExecutableDirectory() / L"assets");
        // 描画APIを初期化時に選択し、D3D12失敗時はD3D11へ戻します。
        application.Initialize(
            instance,
            settings.graphics.renderingApi);
        static_cast<void>(application.Network().Configure(settings.network));
        // 起動した描画バックエンドが実験版D3D12か
        const bool d3d12ExperimentalRenderer =
            application.Graphics().IsD3D12ExperimentalRenderer();
        // 通常起動時だけ公開オンライン設定を適用します。
        if (!validateStartup && settings.online.enabled)
        {
            // オンラインサービス用の接続設定
            LamaPon::OnlineServiceConfiguration online;
            online.serviceBaseUrl =
                settings.online.serviceBaseUrl;
            online.allowInsecureLoopback =
                settings.online.allowInsecureLoopback;
            online.gameId = settings.online.gameId;
            online.environmentId =
                settings.online.environmentId;
            online.openAuthorizationBrowser =
                settings.online.openAuthorizationBrowser;
            application.Online().Configure(std::move(online));
        }
        // Discord Rich PresenceはOnlineログインと独立して有効化できます。
        if (!validateStartup
            && settings.online.discordPresence.enabled)
        {
            // Discord Presence用の公開接続設定
            LamaPon::DiscordPresenceConfiguration presence;
            presence.enabled = true;
            presence.applicationId =
                settings.online.discordPresence.applicationId;
            presence.defaultLargeImageKey =
                settings.online.discordPresence
                    .defaultLargeImageKey;
            presence.defaultLargeImageText =
                settings.online.discordPresence
                    .defaultLargeImageText;
            application.Online().ConfigureDiscordPresence(
                std::move(presence));
        }
        // C++ Game Moduleの配置先
        const auto gameModulePath =
            LamaPon::ExecutableDirectory()
            / L"LamaPonGameModule.dll";
        // DLLが存在して読み込めない場合は、具体的な理由で起動を止めます。
        // DLL不在のC++以外のゲームは従来どおり起動します。
        if (std::filesystem::is_regular_file(gameModulePath)
            && !application.GameModule().IsLoaded())
        {
            throw std::runtime_error(
                "LamaPonGameModule.dll could not be loaded. "
                + application.GameModule().LastError());
        }
        application.Graphics().SetGraphicsSettings(
            settings.graphics);
        application.SetStartupSplashScreenEnabled(
            settings.splashScreenEnabled);
        LamaPon::SetActivePhysicsSettings(
            settings.physics);
        application.Input().SetActions(
            settings.inputActions);

        application.ActiveScene().SetRegisteredTags(
            settings.tags);
        // UI Buttonや起動シーンなど、非同期のシーン切り替えで表示する読み込み画面です。
        application.ActiveScene().Scenes().LoadingScreen() =
            settings.loadingScreen;
        // 無人のStartup検証を通常起動から分けます。
        if (validateStartup)
        {
            // 検証中のOnlineServiceが未設定かを調べます。
            if (application.Online().State()
                != LamaPon::OnlineAccountState::Unconfigured)
            {
                throw std::runtime_error(
                    "Startup validation unexpectedly configured online services.");
            }
            // 無人検証中はウィンドウを隠します。
            ShowWindow(application.WindowHandle(), SW_HIDE);
            // 起動したSceneへの参照
            auto& scene = application.ActiveScene();
            // 起動Sceneのロードを同期完了させます。
            if (!scene.Scenes().RequestLoad(settings.startupScene)
                || !scene.Scenes().ProcessPending())
            {
                throw std::runtime_error(scene.Scenes().LastError());
            }
            // NativeScriptの初回更新で実行エラーを検出します。
            scene.Update(1.0f / 60.0f);
            // 起動Scene内のGameObjectを調べます。
            for (const auto& object : scene.GameObjects())
            {
                // 各GameObjectが持つComponentを調べます。
                for (const auto& component : object->Components())
                {
                    // NativeScriptに記録された実行エラー
                    if (const auto* script = dynamic_cast<const LamaPon::NativeScriptComponent*>(component.get());
                        script != nullptr && !script->LastError().empty())
                    {
                        throw std::runtime_error(script->LastError());
                    }
                }
            }
            // 実験版D3D12ではScene描画まで無人検証します。
        if (d3d12ExperimentalRenderer)
            {
                // 検証フレームの背景色
                constexpr float experimentalClearColor[4]{
                    0.025f, 0.035f, 0.055f, 1.0f };
                application.Graphics().BeginFrame(
                    experimentalClearColor);
                scene.RenderMainCamera(
                    application.Graphics().AspectRatio(),
                    false,
                    nullptr);
                scene.Render2D();
                application.Graphics().EndFrame();
                LamaPon::Logger::Instance().Info(
                    "DirectX 12 ExperimentalでSceneの3Dと2D/UI描画を検証しました。");
            }
            // 無人検証の成功を終了コードへ返します。
            return 0;
        }
        // 通常起動ではScene読み込みを非同期で始めます。
        if (!application.ActiveScene().
            Scenes().RequestLoadAsync(
                settings.startupScene))
        {
            throw std::runtime_error(
                application.ActiveScene().
                    Scenes().LastError());
        }

        // 初期化済みApplicationの実行ループ
        return application.Run();
    }
    // 起動失敗を記録してエラーコードを返します(exception: 起動例外)。
    catch (const std::exception& exception)
    {
        static_cast<void>(
            LamaPon::CrashReporter::WriteDiagnostic(
                exception.what()));
        // 例外の詳細をログファイルへ保存します。
        std::ofstream log("LamaPonGame.log", std::ios::trunc);
        log << exception.what() << '\n';

        // 無人検証以外では利用者へエラーを表示します。
        if (!validateStartup)
        {
            MessageBoxA(
                nullptr,
                exception.what(),
                "LamaPon Game error",
                MB_OK | MB_ICONERROR);
        }
        // 起動失敗を終了コードへ返します。
        return 1;
    }
}
