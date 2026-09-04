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

        application.Initialize(instance);
        // Game Moduleが存在するのに互換性などで読めなかった場合、Sceneを
        // 続けて表示すると「背景だけで止まった」ように見えます。配布ゲーム
        // では起動を止め、既にApplicationが記録した具体的な理由を画面へ
        // 出します。C++を使わないゲーム（DLL自体が無い）は従来どおりです。
        const auto gameModulePath =
            LamaPon::ExecutableDirectory()
            / L"LamaPonGameModule.dll";
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
