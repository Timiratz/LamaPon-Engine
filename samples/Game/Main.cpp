#include "LamaPon/LamaPon.h"

#include <Windows.h>
#include <shellapi.h>

#include <exception>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace
{
    // 起動引数に指定のフラグがあるかを調べます。
    bool HasCommandLineFlag(const std::wstring_view flag)
    {
        int argumentCount{};
        auto** argumentValues = CommandLineToArgvW(
            GetCommandLineW(),
            &argumentCount);
        if (argumentValues == nullptr)
        {
            return false;
        }
        bool found = false;
        for (int index = 1; index < argumentCount; ++index)
        {
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

int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    int)
{
    LamaPon::CrashReporter::Install(
        LamaPon::ExecutableDirectory() / L"Crashes",
        "LamaPonGame");
    const bool validateStartup = HasCommandLineFlag(L"--validate-startup");
    try
    {
        // --warp: GPUを使わずCPUラスタライザ（WARP）で描画します。
        // 仮想マシンやGPUが正しく動かない環境で試すときに使います。
        if (HasCommandLineFlag(L"--warp"))
        {
            LamaPon::GraphicsDevice::SetPreferWarpAdapter(true);
        }

        const LamaPon::ProjectSettings settings =
            LamaPon::LoadProjectSettings(
                LamaPon::ExecutableDirectory()
                / L"LamaPonGame.json");
        LamaPon::Application application(
            LamaPon::Utf8ToWide(settings.gameName),
            settings.windowWidth,
            settings.windowHeight,
            settings.gameName);

        // D3D12 Experimentalは、D3D11前提のscene rendererを作らずに
        // swap chainのclear/presentだけを検証する起動プロファイルです。
        // 実際にD3D12 backendが作れない環境ではGraphicsDeviceがD3D11へ
        // フォールバックするため、その場合は従来の完全なゲームを起動します。
        const auto graphicsStartupProfile =
            settings.graphics.renderingApi
                == LamaPon::RenderingApi::DirectX12Experimental
            ? LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap
            : LamaPon::GraphicsStartupProfile::FullRenderer;
        // 描画APIはデバイス初期化時にだけ選択し、実行中は切り替えません。
        application.Initialize(
            instance,
            settings.graphics.renderingApi,
            graphicsStartupProfile);
        const bool d3d12ExperimentalBootstrap =
            application.Graphics().IsD3D12ExperimentalBootstrap();
        // Game Moduleが存在するのに互換性などで読めなかった場合、Sceneを
        // 続けて表示すると「背景だけで止まった」ように見えます。配布ゲーム
        // では起動を止め、既にApplicationが記録した具体的な理由を画面へ
        // 出します。C++を使わないゲーム（DLL自体が無い）は従来どおりです。
        const auto gameModulePath =
            LamaPon::ExecutableDirectory()
            / L"LamaPonGameModule.dll";
        if (!d3d12ExperimentalBootstrap
            && std::filesystem::is_regular_file(gameModulePath)
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

        if (d3d12ExperimentalBootstrap)
        {
            // 実効D3D12 backendではまだScene rendererを初期化しません。
            // --validate-startupは、最低限のclear/presentが成功することを
            // 1フレームだけ検証して終了します。
            if (validateStartup)
            {
                ShowWindow(application.WindowHandle(), SW_HIDE);
                constexpr float bootstrapClearColor[4]{
                    0.025f, 0.035f, 0.055f, 1.0f };
                application.Graphics().BeginFrame(
                    bootstrapClearColor);
                application.Graphics().EndFrame();
                return 0;
            }

            return application.Run();
        }

        application.ActiveScene().SetRegisteredTags(
            settings.tags);
        if (validateStartup)
        {
            // 配布物を別プロセスで検証するための無人実行です。
            // DLL・暗号鍵・シーンを実際に読み、失敗は終了コードへ返します。
            ShowWindow(application.WindowHandle(), SW_HIDE);
            auto& scene = application.ActiveScene();
            if (!scene.Scenes().RequestLoad(settings.startupScene)
                || !scene.Scenes().ProcessPending())
            {
                throw std::runtime_error(scene.Scenes().LastError());
            }
            scene.Update(1.0f / 60.0f);
            for (const auto& object : scene.GameObjects())
            {
                for (const auto& component : object->Components())
                {
                    if (const auto* script = dynamic_cast<const LamaPon::NativeScriptComponent*>(component.get());
                        script != nullptr && !script->LastError().empty())
                    {
                        throw std::runtime_error(script->LastError());
                    }
                }
            }
            return 0;
        }
        if (!application.ActiveScene().
            Scenes().RequestLoadAsync(
                settings.startupScene))
        {
            throw std::runtime_error(
                application.ActiveScene().
                    Scenes().LastError());
        }

        return application.Run();
    }
    catch (const std::exception& exception)
    {
        static_cast<void>(
            LamaPon::CrashReporter::WriteDiagnostic(
                exception.what()));
        std::ofstream log("LamaPonGame.log", std::ios::trunc);
        log << exception.what() << '\n';

        if (!validateStartup)
        {
            MessageBoxA(
                nullptr,
                exception.what(),
                "LamaPon Game error",
                MB_OK | MB_ICONERROR);
        }
        return 1;
    }
}
