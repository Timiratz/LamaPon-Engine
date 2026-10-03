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
    // 条件不成立ならテストを失敗させます。
    // Require(condition: 成立条件, message: 失敗理由)
    void Require(const bool condition, const std::string& message)
    {
        // assertion失敗を例外で通知
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    class TemporaryDirectory final
    {
    public:
        // 一意な空のGame Module test領域を作ります。
        TemporaryDirectory()
        {
            m_path = std::filesystem::temp_directory_path()
                / (L"LamaPonGameModuleBuilderTests-"
                    + std::to_wstring(
                        std::chrono::steady_clock::now()
                            .time_since_epoch().count()));
            std::filesystem::create_directories(m_path);
        }

        // temporary directoryとその内容を削除します。
        ~TemporaryDirectory()
        {
            // directory削除時のエラー状態
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
        }

        // 一時領域のroot pathを返します。
        [[nodiscard]] const std::filesystem::path& Path() const
        {
            return m_path;
        }

    private:
        // 一時領域のroot path
        std::filesystem::path m_path;
    };

    // pathに空のテスト入力ファイルを作成します。
    // Touch(path: 作成先ファイル)
    void Touch(const std::filesystem::path& path)
    {
        std::filesystem::create_directories(path.parent_path());
        // テスト入力を作るバイナリ出力
        std::ofstream output(path, std::ios::binary);
        Require(static_cast<bool>(output), "Could not create test input.");
    }
}

// Game Moduleのbuild command・cache・staleness判定を検証します。
int main()
{
    // テスト例外を失敗終了コードへ変換
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

        // メモリ計算用の1GiB
        constexpr std::uint64_t gib = 1024ull * 1024 * 1024;
        Require(
            LamaPon::SelectGameModuleBuildParallelJobs(16, 32 * gib, 64 * gib) == 2,
            "Many CPUs and abundant memory must not exceed two jobs.");
        Require(
            LamaPon::SelectGameModuleBuildParallelJobs(16, 6 * gib - 1, 64 * gib) == 1,
            "Insufficient physical memory for two jobs must select one job.");
        Require(
            LamaPon::SelectGameModuleBuildParallelJobs(16, 6 * gib, 64 * gib) == 2,
            "Two jobs require their memory budget plus the system reserve.");
        Require(
            LamaPon::SelectGameModuleBuildParallelJobs(16, 32 * gib, 4 * gib) == 1,
            "A low commit budget must limit parallelism even with free RAM.");
        Require(
            LamaPon::SelectGameModuleBuildParallelJobs(2, 32 * gib, 64 * gib) == 1,
            "Small CPUs must leave execution capacity for other applications.");
        Require(
            LamaPon::SelectGameModuleBuildParallelJobs(0, 0, 0) == 1,
            "Unknown resources must conservatively select one job.");

        // 各テスト用ファイルを配置する一時root
        TemporaryDirectory temporary;
        // テストproject配置先
        const auto project = temporary.Path() / L"Project";
        // Game Module build tool配置先
        const auto engine = temporary.Path() / L"Engine";
        // Runtime import library配置先
        const auto runtime = temporary.Path() / L"Runtime";
        // project外のlocal build cache配置先
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
        // CMake・MSBuild引数と出力先を含むbuild command
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

        // package依存がなくてもCMake include用定義を出力
        Require(
            std::filesystem::is_regular_file(
                project / L".lamapon" / L"package-native.cmake"),
            "The package native settings must always be generated.");

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
            command.parameters.find(
                L"LAMAPON_RUNTIME_API_VERSION:STRING=")
                != std::wstring::npos,
            "The Game Module build must receive the Runtime API version"
            " so an engine update recompiles its descriptor.");
        Require(
            command.logPath
                == project / L".lamapon"
                    / L"game-module-build.log",
            "The final build log path must remain project-compatible.");

        // 編集向けコマンドの設定
        const auto fastCommand = LamaPon::MakeGameModuleBuildCommand(
            project, engine, runtime, "Release", true);
        Require(
            fastCommand.parameters.find(L"LAMAPON_MODULE_FAST_BUILD:BOOL=ON")
                != std::wstring::npos,
            "Editor builds must enable fast compilation and linking.");
        Require(
            command.parameters.find(L"LAMAPON_MODULE_FAST_BUILD:BOOL=OFF")
                != std::wstring::npos,
            "CLI and export builds must explicitly reset cached fast settings.");
        Require(
            fastCommand.outputModule == command.outputModule,
            "Fast builds must preserve the deployed module path.");
        Require(
            fastCommand.parallelJobs >= 1 && fastCommand.parallelJobs <= 2
                && fastCommand.parameters.find(
                    L"--parallel " + std::to_wstring(fastCommand.parallelJobs) + L" >> ")
                    != std::wstring::npos,
            "Editor commands must explicitly enforce their resource limit.");
        Require(
            command.parallelJobs >= 1 && command.parallelJobs <= 2
                && command.parameters.find(
                    L"--parallel " + std::to_wstring(command.parallelJobs) + L" >> ")
                    != std::wstring::npos,
            "Cached CLI commands must enforce the same resource limit.");

        // staleness判定に使うGame Module source
        const auto source = project / L"assets" / L"scripts"
            / L"LearningPlayer.cpp";
        // staleness判定に使うdeploy済みmodule
        const auto module = project / L".lamapon" / L"bin"
            / L"LamaPonGameModule.dll";
        Require(
            !LamaPon::InspectGameModuleBuildState(project).buildRequired,
            "A project without C++ sources must not auto-build.");
        Touch(source);
        // projectの現行build要否
        auto buildState =
            LamaPon::InspectGameModuleBuildState(project);
        Require(
            buildState.hasSources
                && !buildState.outputExists
                && buildState.buildRequired,
            "A source without a Game Module must auto-build.");

        Touch(module);
        // ファイル時刻比較の基準
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

        // Runtimeより古いmoduleはload不可なのでbuild対象
        // 実行ファイル隣のRuntime DLL
        const auto runtimeDll =
            LamaPon::ExecutableDirectory() / L"LamaPonRuntime.dll";
        // Runtime DLLの更新時刻取得結果
        std::error_code runtimeError;
        // build staleness判定の基準時刻
        const auto runtimeTime =
            std::filesystem::last_write_time(runtimeDll, runtimeError);
        // Runtime DLLの更新時刻を取得できる場合だけ比較
        if (!runtimeError)
        {
            std::filesystem::last_write_time(
                source,
                runtimeTime - std::chrono::hours(2));
            std::filesystem::last_write_time(
                module,
                runtimeTime - std::chrono::hours(1));
            // Runtimeより古いmoduleのbuild状態
            const auto staleState =
                LamaPon::InspectGameModuleBuildState(project);
            Require(
                staleState.staleAgainstRuntime
                    && staleState.buildRequired,
                "A module older than the engine must auto-build.");
        }

        // WebDAVのstale mtimeをcontent hashで補正
        {
            // ソースhash記録を保存するbuild directory
            const auto buildDirectory =
                temporary.Path() / L"BuildDir";
            {
                // 初回hashを記録するsource内容
                std::ofstream output(
                    source, std::ios::binary | std::ios::trunc);
                output << "// v1";
            }
            Require(
                LamaPon::RefreshStaleGameModuleSources(
                    project, buildDirectory) == 0,
                "The first refresh only records hashes.");
            {
                // 同じmtimeで内容だけ変えるsource
                std::ofstream output(
                    source, std::ios::binary | std::ios::trunc);
                output << "// v2";
            }
            // WebDAVの古いmtimeを再現
            // WebDAVが示す過去の更新時刻
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
    // テスト例外を標準エラーと失敗終了コードへ変換
    catch (const std::exception& exception)
    {
        static_cast<void>(SetEnvironmentVariableW(
            L"LAMAPON_GAME_MODULE_CACHE_ROOT",
            nullptr));
        std::cerr << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
