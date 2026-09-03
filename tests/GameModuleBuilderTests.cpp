#include "LamaPon/Editor/GameModuleBuilder.h"
#include "LamaPon/Core/PathUtils.h"
#include "BuildDiagnostics.h"

#include <Windows.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    void Require(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    class TemporaryDirectory final
    {
    public:
        TemporaryDirectory()
        {
            m_path = std::filesystem::temp_directory_path()
                / (L"LamaPonGameModuleBuilderTests-"
                    + std::to_wstring(
                        std::chrono::steady_clock::now()
                            .time_since_epoch().count()));
            std::filesystem::create_directories(m_path);
        }

        ~TemporaryDirectory()
        {
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
        }

        [[nodiscard]] const std::filesystem::path& Path() const
        {
            return m_path;
        }

    private:
        std::filesystem::path m_path;
    };

    void Touch(const std::filesystem::path& path)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        Require(static_cast<bool>(output), "Could not create test input.");
    }
}

int main()
{
    try
    {
        Require(
            LamaPon::Cli::IsBuildErrorLine(
                "Game.cpp(42): error C2065: identifier not found"),
            "MSVC errors must be extracted from the build log.");
        Require(
            LamaPon::Cli::IsBuildErrorLine(
                "CMake Error at CMakeLists.txt:12 (add_library):"),
            "CMake errors must be extracted from the build log.");
        Require(
            !LamaPon::Cli::IsBuildErrorLine(
                "Note: including file: Windows Kits/shared/winerror.h"),
            "A header path containing 'error' must not be a diagnostic.");
        Require(
            !LamaPon::Cli::IsBuildErrorLine("0 Error(s)"),
            "A successful build summary must not be a diagnostic.");

        TemporaryDirectory temporary;
        const auto project = temporary.Path() / L"Project";
        const auto engine = temporary.Path() / L"Engine";
        const auto runtime = temporary.Path() / L"Runtime";
        const auto cache = temporary.Path() / L"LocalCache";
        std::filesystem::create_directories(project / L".lamapon");
        Touch(engine / L"tools" / L"ProjectGameModule"
            / L"CMakeLists.txt");
        Touch(runtime / L"LamaPonRuntime.lib");

        Require(
            SetEnvironmentVariableW(
                L"LAMAPON_GAME_MODULE_CACHE_ROOT",
                cache.c_str()) != FALSE,
            "Could not set the test cache root.");
        const auto command = LamaPon::MakeGameModuleBuildCommand(
            project,
            engine,
            runtime,
            "Release");
        static_cast<void>(SetEnvironmentVariableW(
            L"LAMAPON_GAME_MODULE_CACHE_ROOT",
            nullptr));

        Require(
            LamaPon::ShouldUseLocalGameModuleBuildCache(
                L"\\\\localhost@9843\\DavWWWRoot\\InkRidge"),
            "UNC/WebDAV project paths must use a local build cache.");
        Require(
            !LamaPon::ShouldUseLocalGameModuleBuildCache(project),
            "A normal local project must not require a local cache.");

        Require(
            command.usesLocalBuildCache,
            "An explicit local cache root must enable cached builds.");
        Require(
            command.buildDirectory.native().starts_with(cache.native()),
            "CMake intermediates must stay under the local cache root.");
        Require(
            command.outputModule
                == project / L".lamapon" / L"bin"
                    / L"LamaPonGameModule.dll",
            "The deployed DLL must remain inside the project.");
        Require(
            command.parameters.find(L"LAMAPON_MODULE_DEPLOY_DIR")
                    != std::wstring::npos
                && command.parameters.find(L"/v:on")
                    != std::wstring::npos,
            "A cached command must deploy only after a successful build.");
        Require(
            command.parameters.find(L" cd /d ")
                == std::wstring::npos,
            "The build command must not cd into a UNC project path.");
        Require(
            command.parameters.find(L"chcp 65001")
                != std::wstring::npos,
            "MSVC include output must be UTF-8 so Ninja records"
            " header dependencies.");
        Require(
            command.logPath
                == project / L".lamapon"
                    / L"game-module-build.log",
            "The final build log path must remain project-compatible.");

        const auto source = project / L"assets" / L"scripts"
            / L"LearningPlayer.cpp";
        const auto module = project / L".lamapon" / L"bin"
            / L"LamaPonGameModule.dll";
        Require(
            !LamaPon::InspectGameModuleBuildState(project).buildRequired,
            "A project without C++ sources must not auto-build.");
        Touch(source);
        auto buildState =
            LamaPon::InspectGameModuleBuildState(project);
        Require(
            buildState.hasSources
                && !buildState.outputExists
                && buildState.buildRequired,
            "A source without a Game Module must auto-build.");

        Touch(module);
        const auto now = std::filesystem::file_time_type::clock::now();
        std::filesystem::last_write_time(
            source,
            now - std::chrono::seconds(4));
        std::filesystem::last_write_time(
            module,
            now - std::chrono::seconds(2));
        buildState = LamaPon::InspectGameModuleBuildState(project);
        Require(
            buildState.outputExists
                && !buildState.buildRequired,
            "A module newer than every source must not auto-build.");
        std::filesystem::last_write_time(source, now);
        Require(
            LamaPon::InspectGameModuleBuildState(project).buildRequired,
            "A source newer than the module must auto-build.");

        // エンジン（実行ファイルの隣のLamaPonRuntime.dll）より古いDLLは
        // GameModuleHostに拒否されるため、自動ビルドの対象になる。
        // 判定側と同じ「exeの隣のDLL」を基準時刻に使う
        const auto runtimeDll =
            LamaPon::ExecutableDirectory() / L"LamaPonRuntime.dll";
        std::error_code runtimeError;
        const auto runtimeTime =
            std::filesystem::last_write_time(runtimeDll, runtimeError);
        if (!runtimeError)
        {
            std::filesystem::last_write_time(
                source,
                runtimeTime - std::chrono::hours(2));
            std::filesystem::last_write_time(
                module,
                runtimeTime - std::chrono::hours(1));
            const auto staleState =
                LamaPon::InspectGameModuleBuildState(project);
            Require(
                staleState.staleAgainstRuntime
                    && staleState.buildRequired,
                "A module older than the engine must auto-build.");
        }

        // WebDAVのmtimeキャッシュ対策: 内容が変わったのにmtimeが
        // 動いていないソースは、更新時刻が現在へ進められる
        {
            const auto buildDirectory =
                temporary.Path() / L"BuildDir";
            {
                std::ofstream output(
                    source, std::ios::binary | std::ios::trunc);
                output << "// v1";
            }
            Require(
                LamaPon::RefreshStaleGameModuleSources(
                    project, buildDirectory) == 0,
                "The first refresh only records hashes.");
            {
                std::ofstream output(
                    source, std::ios::binary | std::ios::trunc);
                output << "// v2";
            }
            // WebDAVが古いmtimeを見せる状況を再現する
            const auto stale =
                std::filesystem::file_time_type::clock::now()
                - std::chrono::hours(1);
            std::filesystem::last_write_time(source, stale);
            Require(
                LamaPon::RefreshStaleGameModuleSources(
                    project, buildDirectory) == 1,
                "A changed source with a stale mtime must be"
                " touched.");
            Require(
                std::filesystem::last_write_time(source)
                    > stale + std::chrono::minutes(30),
                "The touched source must have a fresh write time.");
            Require(
                LamaPon::RefreshStaleGameModuleSources(
                    project, buildDirectory) == 0,
                "An unchanged source must not be touched again.");
        }

        std::cout << "Game Module builder tests passed.\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& exception)
    {
        static_cast<void>(SetEnvironmentVariableW(
            L"LAMAPON_GAME_MODULE_CACHE_ROOT",
            nullptr));
        std::cerr << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
