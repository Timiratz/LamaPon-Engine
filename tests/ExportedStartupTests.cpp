#include "LamaPon/Editor/GameExporter.h"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>

namespace
{
    // 条件不成立ならテストを失敗させます。
    // Require(condition: 成立条件, message: 失敗理由)
    void Require(const bool condition, const char* message)
    {
        // 失敗理由を例外で通知
        if (!condition) throw std::runtime_error(message);
    }

    // executableを独立起動し、ローカル起動検証の終了状態を確認します。
    // RunExportedGame(executable: 起動する配布ゲーム, expectedExit: 期待コード, alternateExit: 許容するOS差)
    void RunExportedGame(const std::filesystem::path& executable,
        const DWORD expectedExit = 0, const std::optional<DWORD> alternateExit = std::nullopt)
    {
        // 親プロセスのRuntimeに依存しない起動を確認
        // 配布ゲーム起動コマンド
        auto command = L"\"" + executable.wstring()
            + L"\" --warp --validate-startup";
        // 子プロセスの非表示起動設定
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        // 起動したゲームプロセス情報
        PROCESS_INFORMATION process{};
        Require(CreateProcessW(executable.c_str(), command.data(),
            nullptr, nullptr, FALSE, 0, nullptr,
            executable.parent_path().parent_path().c_str(), &startup, &process) != FALSE,
            "Could not start exported game.");
        CloseHandle(process.hThread);
        // 起動検証プロセスの完了状態
        const auto wait = WaitForSingleObject(process.hProcess, 30000);
        // 制限時間を超えた場合は子プロセスを終了
        if (wait != WAIT_OBJECT_0)
        {
            // このテストが作成した子プロセスだけを終了します。
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
        }
        // 子プロセスの終了コード
        DWORD exitCode = 1;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hProcess);
        if (exitCode != expectedExit && (!alternateExit || exitCode != *alternateExit))
            std::cerr << "Actual exit: " << exitCode << ", expected: " << expectedExit << '\n';
        Require(wait == WAIT_OBJECT_0
                && (exitCode == expectedExit || (alternateExit && exitCode == *alternateExit)),
            "Exported game startup failed; inspect LamaPonGame.log in test-output/exported-startup.");
    }
}

// 出力パッケージの起動とUI描画を検証します。
int main()
{
    // テスト例外を失敗終了コードへ変換
    try
    {
        // 実行環境の作業ディレクトリ
        const auto runtime = std::filesystem::current_path();
        // テスト成果物のルート
        const auto root = runtime / "test-output" / "exported-startup";
        // 配布パッケージ用のアセットルート
        const auto assets = root / "project" / "assets";
        std::filesystem::create_directories(assets / "scenes");
        // 起動シーンとオンライン機能を持つテスト設定
        LamaPon::ProjectSettings settings;
        settings.gameName = "Startup Regression";
        settings.startupScene = "scenes/Main.scene.json";
        settings.splashScreenEnabled = false;
        settings.stripShaderSourceOnExport = false;
        // --validate-startupはオンライン設定時も外部接続しない
        settings.online.enabled = true;
        settings.online.serviceBaseUrl =
            "https://online.example.test";
        settings.online.gameId = "com.example.startup-test";
        settings.online.environmentId = "production";
        {
            // テスト用NativeScript入りシーン
            std::ofstream scene(assets / settings.startupScene);
            scene << R"({"format":"LamaPonScene","objects":[
                {"id":1,"name":"Probe","transform":{"position":[0,0,0],"scale":[1,1,1]},"components":[
                    {"type":"NativeScript","script":"Sample.FloatingAccent","properties":{}}
                ]}]})";
        }
        // ゲームモジュール同梱の出力設定
        LamaPon::GameExportOptions options{
            runtime, assets, root / L"with-module-日本語-☃", settings,
            runtime / "LamaPonGameModule.dll" };
        // モジュール同梱パッケージの出力結果
        auto result = LamaPon::ExportGamePackage(options);
        const auto dataName = L"Startup Regression_Data";
        Require(std::distance(std::filesystem::directory_iterator(result.outputDirectory),
                    std::filesystem::directory_iterator{}) == 2,
            "The export root must contain only the launcher and Data directory.");
        Require(std::filesystem::last_write_time(result.outputDirectory / dataName / "LamaPonRuntime.dll")
                == std::filesystem::last_write_time(runtime / "LamaPonRuntime.dll"),
            "Archive-key embedding must preserve runtime build time.");
        // name: 配布物へ同梱する各依存ライセンス
        for (const auto* name : { "LamaPon", "DirectXTK", "imgui", "ImGuizmo",
                 "nlohmann-json", "XAudio2Redist", "cgltf", "ufbx", "stb-vorbis" })
        {
            // 配布元からの相対ライセンスパス
            const auto relative = std::filesystem::path("licenses") / (std::string(name) + ".txt");
            Require(std::filesystem::file_size(result.outputDirectory / dataName / relative)
                == std::filesystem::file_size(runtime / relative),
                "Exported license text is missing or truncated.");
        }
        RunExportedGame(result.executablePath);
        Require(std::distance(std::filesystem::directory_iterator(result.outputDirectory),
                    std::filesystem::directory_iterator{}) == 2,
            "Game startup must keep logs and crash records inside Data.");

        // ランチャーがローダー失敗を無人で診断できることを確認します。
        // 検証対象のゲーム本体
        const auto game = result.outputDirectory / dataName / "LamaPonGame.exe";
        // 復旧用のゲーム本体
        const auto savedGame = game.wstring() + L".saved";
        std::filesystem::rename(game, savedGame);
        try
        {
            // 不正なexe形式を再現
            std::ofstream(game, std::ios::binary) << "Invalid executable";
            RunExportedGame(result.executablePath, ERROR_BAD_EXE_FORMAT, ERROR_EXE_MACHINE_TYPE_MISMATCH);
            // ランチャーによる診断ログ
            std::ifstream failure(result.outputDirectory / dataName / "StartupFailure.log",
                std::ios::binary);
            // UTF-8診断の全文
            const std::string text{ std::istreambuf_iterator<char>(failure),
                std::istreambuf_iterator<char>() };
            Require(text.find("0x000000C1") != std::string::npos
                    || text.find("0x000000D8") != std::string::npos,
                "Launcher must record the Win32 loader error in hexadecimal.");
            Require(text.find("with-module-日本語-☃") != std::string::npos,
                "Launcher report must preserve Unicode paths as UTF-8.");
            Require(text.find("ゲームを起動できませんでした") != std::string::npos,
                "Launcher report must identify process creation failures.");
        }
        catch (...)
        {
            std::filesystem::remove(game);
            std::filesystem::rename(savedGame, game);
            throw;
        }
        std::filesystem::remove(game);
        std::filesystem::rename(savedGame, game);

        // 本体のエントリ到達前のDLL失敗を確認します。
        // 配布先のエンジンDLL
        const auto runtimeDll = result.outputDirectory / dataName / "LamaPonRuntime.dll";
        // 復旧用のエンジンDLL
        const auto savedRuntimeDll = runtimeDll.wstring() + L".saved";
        std::filesystem::rename(runtimeDll, savedRuntimeDll);
        try
        {
            // 不正なDLL形式を再現
            std::ofstream(runtimeDll, std::ios::binary) << "Invalid runtime DLL";
            RunExportedGame(result.executablePath, 0xC000007Bu, 0xC000012Fu);
            // ローダー異常終了の診断ログ
            std::ifstream failure(result.outputDirectory / dataName / "StartupFailure.log",
                std::ios::binary);
            // 診断の全文
            const std::string text{ std::istreambuf_iterator<char>(failure),
                std::istreambuf_iterator<char>() };
            Require(text.find("0xC000007B") != std::string::npos
                    || text.find("0xC000012F") != std::string::npos,
                "Launcher must preserve NTSTATUS loader failure codes before runtime startup.");
        }
        catch (...)
        {
            std::filesystem::remove(runtimeDll);
            std::filesystem::rename(savedRuntimeDll, runtimeDll);
            throw;
        }
        std::filesystem::remove(runtimeDll);
        std::filesystem::rename(savedRuntimeDll, runtimeDll);

        // 起動後の例外終了もランチャーが記録することを確認します。
        // 配布先のゲーム設定
        const auto configuration = result.outputDirectory / dataName / "LamaPonGame.json";
        // 復旧用のゲーム設定
        const auto savedConfiguration = configuration.wstring() + L".saved";
        std::filesystem::rename(configuration, savedConfiguration);
        try
        {
            // JSON読み込みの例外を再現
            std::ofstream(configuration, std::ios::binary) << "{ invalid json";
            RunExportedGame(result.executablePath, 1);
            // 非ゼロ終了の診断ログ
            std::ifstream failure(result.outputDirectory / dataName / "StartupFailure.log",
                std::ios::binary);
            // 診断の全文
            const std::string text{ std::istreambuf_iterator<char>(failure),
                std::istreambuf_iterator<char>() };
            Require(text.find("0x00000001") != std::string::npos
                    && text.find("ゲームが正常に終了しませんでした") != std::string::npos,
                "Launcher must report nonzero child exit codes separately from creation failures.");
            Require(std::filesystem::is_regular_file(
                    result.outputDirectory / dataName / "LamaPonGame.log"),
                "Runtime startup exceptions must be recorded next to the game executable.");
        }
        catch (...)
        {
            std::filesystem::remove(configuration);
            std::filesystem::rename(savedConfiguration, configuration);
            throw;
        }
        std::filesystem::remove(configuration);
        std::filesystem::rename(savedConfiguration, configuration);

        // モジュールなしの起動にも対応
        std::ofstream(assets / settings.startupScene, std::ios::trunc)
            << R"({"format":"LamaPonScene","objects":[]})";
        options.gameModulePath = root / "absent" / "LamaPonGameModule.dll";
        options.outputDirectory = root / "without-module";
        // モジュールを含まない出力結果
        result = LamaPon::ExportGamePackage(options);
        Require(!std::filesystem::exists(result.outputDirectory / dataName / "LamaPonGameModule.dll"),
            "A project without a module must not acquire the sample module.");
        RunExportedGame(result.executablePath);
        Require(std::distance(std::filesystem::directory_iterator(result.outputDirectory),
                    std::filesystem::directory_iterator{}) == 2,
            "Game startup must keep logs and crash records inside Data.");

        // DirectX 12 ExperimentalでシーンのText/Imageを描画
        std::ofstream(assets / settings.startupScene, std::ios::trunc)
            << R"({"format":"LamaPonScene","objects":[
                {"id":1,"name":"Panel","transform":{"position":[0,0,0],"scale":[1,1,1]},"components":[
                    {"type":"UIRectTransform","anchorMin":[0.5,0.5],"anchorMax":[0.5,0.5],"pivot":[0.5,0.5],"anchoredPosition":[0,0],"sizeDelta":[320,120]},
                    {"type":"UIImage","color":[0.1,0.3,0.7,1.0],"sortOrder":1}
                ]},
                {"id":2,"name":"Label","transform":{"position":[0,0,0],"scale":[1,1,1]},"components":[
                    {"type":"UIRectTransform","anchorMin":[0.5,0.5],"anchorMax":[0.5,0.5],"pivot":[0.5,0.5],"anchoredPosition":[0,0],"sizeDelta":[280,64]},
                    {"type":"TextRenderer","text":"DirectX 12 Scene UI","fontFamily":"Segoe UI","fontSize":28,"color":[1,1,1,1],"layoutSize":[280,64],"sortOrder":2}
                ]}
            ]})";
        options.projectSettings.graphics.renderingApi =
            LamaPon::RenderingApi::DirectX12Experimental;
        options.projectSettings.splashScreenEnabled = false;
        options.outputDirectory = root / "d3d12-experimental";
        // DirectX 12 Experimental設定の出力結果
        result = LamaPon::ExportGamePackage(options);
        // 配布先ゲームのRuntimeログ
        const auto runtimeLog =
            result.outputDirectory / dataName / "LamaPon.log";
        std::filesystem::remove(runtimeLog);
        RunExportedGame(result.executablePath);
        Require(std::distance(std::filesystem::directory_iterator(result.outputDirectory),
                    std::filesystem::directory_iterator{}) == 2,
            "Game startup must keep logs and crash records inside Data.");
        // 起動後のRuntimeログファイル
        std::ifstream runtimeLogFile(runtimeLog, std::ios::binary);
        // Runtimeログ全体
        const std::string runtimeLogText{
            std::istreambuf_iterator<char>(runtimeLogFile),
            std::istreambuf_iterator<char>() };
        Require(runtimeLogText.find("DirectX 12 Experimental renderer")
                != std::string::npos,
            "The exported DirectX 12 Experimental game did not use its renderer startup path.");
        Require(runtimeLogText.find("Sceneの3Dと2D/UI描画を検証")
                != std::string::npos,
            "The exported DirectX 12 Experimental game did not render its scene.");
        std::cout << "Exported startup tests passed.\n";
        return 0;
    }
    // テスト例外を標準エラーと失敗終了コードへ変換
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
