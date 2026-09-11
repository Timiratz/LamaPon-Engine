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
    void Require(const bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    void RunExportedGame(const std::filesystem::path& executable)
    {
        // 親プロセスが読み込んだRuntimeの影響を避け、配布先のDLLと
        // 埋め込み鍵だけで起動できることを確かめます。
        auto command = L"\"" + executable.wstring()
            + L"\" --warp --validate-startup";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        Require(CreateProcessW(executable.c_str(), command.data(),
            nullptr, nullptr, FALSE, 0, nullptr,
            executable.parent_path().c_str(), &startup, &process) != FALSE,
            "Could not start exported game.");
        CloseHandle(process.hThread);
        const auto wait = WaitForSingleObject(process.hProcess, 30000);
        if (wait != WAIT_OBJECT_0)
        {
            // このテストが作成した子プロセスだけを終了します。
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
        }
        DWORD exitCode = 1;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hProcess);
        Require(wait == WAIT_OBJECT_0 && exitCode == 0,
            "Exported game startup failed; inspect LamaPonGame.log in test-output/exported-startup.");
    }
}

int main()
{
    try
    {
        const auto runtime = std::filesystem::current_path();
        const auto root = runtime / "test-output" / "exported-startup";
        const auto assets = root / "project" / "assets";
        std::filesystem::create_directories(assets / "scenes");
        LamaPon::ProjectSettings settings;
        settings.gameName = "Startup Regression";
        settings.startupScene = "scenes/Main.scene.json";
        settings.splashScreenEnabled = false;
        settings.stripShaderSourceOnExport = false;
        {
            std::ofstream scene(assets / settings.startupScene);
            scene << R"({"format":"LamaPonScene","objects":[
                {"id":1,"name":"Probe","transform":{"position":[0,0,0],"scale":[1,1,1]},"components":[
                    {"type":"NativeScript","script":"Sample.FloatingAccent","properties":{}}
                ]}]})";
        }
        LamaPon::GameExportOptions options{
            runtime, assets, root / "with-module", settings,
            runtime / "LamaPonGameModule.dll" };
        auto result = LamaPon::ExportGamePackage(options);
        Require(std::filesystem::last_write_time(result.outputDirectory / "LamaPonRuntime.dll")
                == std::filesystem::last_write_time(runtime / "LamaPonRuntime.dll"),
            "Archive-key embedding must preserve runtime build time.");
        for (const auto* name : { "LamaPon", "DirectXTK", "imgui", "ImGuizmo",
                 "nlohmann-json", "XAudio2Redist", "cgltf", "ufbx", "stb-vorbis" })
        {
            const auto relative = std::filesystem::path("licenses") / (std::string(name) + ".txt");
            Require(std::filesystem::file_size(result.outputDirectory / relative)
                == std::filesystem::file_size(runtime / relative),
                "Exported license text is missing or truncated.");
        }
        RunExportedGame(result.executablePath);

        // DLLを使わないプロジェクトも従来どおり起動できます。
        std::ofstream(assets / settings.startupScene, std::ios::trunc)
            << R"({"format":"LamaPonScene","objects":[]})";
        options.gameModulePath = root / "absent" / "LamaPonGameModule.dll";
        options.outputDirectory = root / "without-module";
        result = LamaPon::ExportGamePackage(options);
        Require(!std::filesystem::exists(result.outputDirectory / "LamaPonGameModule.dll"),
            "A project without a module must not acquire the sample module.");
        RunExportedGame(result.executablePath);

        // DirectX 12 Experimentalでも、実シーンを読み込んでText/Imageの
        // Componentを初期化し、2D/UI Spriteを1フレーム描画します。
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
        result = LamaPon::ExportGamePackage(options);
        const auto runtimeLog =
            result.executablePath.parent_path() / "LamaPon.log";
        std::filesystem::remove(runtimeLog);
        RunExportedGame(result.executablePath);
        std::ifstream runtimeLogFile(runtimeLog, std::ios::binary);
        const std::string runtimeLogText{
            std::istreambuf_iterator<char>(runtimeLogFile),
            std::istreambuf_iterator<char>() };
        Require(runtimeLogText.find("DirectX 12 Experimental bootstrap")
                != std::string::npos,
            "The exported DirectX 12 Experimental game did not use its bootstrap startup path.");
        Require(runtimeLogText.find("Sceneの2D/UI描画を検証")
                != std::string::npos,
            "The exported DirectX 12 Experimental game did not render its scene UI.");
        std::cout << "Exported startup tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
