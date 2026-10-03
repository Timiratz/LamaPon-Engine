#include "LamaPon/Editor/GameExporter.h"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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
    // RunExportedGame(executable: 起動する配布ゲーム)
    void RunExportedGame(const std::filesystem::path& executable)
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
            executable.parent_path().c_str(), &startup, &process) != FALSE,
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
        Require(wait == WAIT_OBJECT_0 && exitCode == 0,
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
            runtime, assets, root / "with-module", settings,
            runtime / "LamaPonGameModule.dll" };
        // モジュール同梱パッケージの出力結果
        auto result = LamaPon::ExportGamePackage(options);
        Require(std::filesystem::last_write_time(result.outputDirectory / "LamaPonRuntime.dll")
                == std::filesystem::last_write_time(runtime / "LamaPonRuntime.dll"),
            "Archive-key embedding must preserve runtime build time.");
        // name: 配布物へ同梱する各依存ライセンス
        for (const auto* name : { "LamaPon", "DirectXTK", "imgui", "ImGuizmo",
                 "nlohmann-json", "XAudio2Redist", "cgltf", "ufbx", "stb-vorbis" })
        {
            // 配布元からの相対ライセンスパス
            const auto relative = std::filesystem::path("licenses") / (std::string(name) + ".txt");
            Require(std::filesystem::file_size(result.outputDirectory / relative)
                == std::filesystem::file_size(runtime / relative),
                "Exported license text is missing or truncated.");
        }
        RunExportedGame(result.executablePath);

        // モジュールなしの起動にも対応
        std::ofstream(assets / settings.startupScene, std::ios::trunc)
            << R"({"format":"LamaPonScene","objects":[]})";
        options.gameModulePath = root / "absent" / "LamaPonGameModule.dll";
        options.outputDirectory = root / "without-module";
        // モジュールを含まない出力結果
        result = LamaPon::ExportGamePackage(options);
        Require(!std::filesystem::exists(result.outputDirectory / "LamaPonGameModule.dll"),
            "A project without a module must not acquire the sample module.");
        RunExportedGame(result.executablePath);

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
            result.executablePath.parent_path() / "LamaPon.log";
        std::filesystem::remove(runtimeLog);
        RunExportedGame(result.executablePath);
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
