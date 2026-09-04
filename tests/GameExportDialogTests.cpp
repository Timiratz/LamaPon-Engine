#include "LamaPon/Editor/GameExportDialog.h"
#include "LamaPon/Core/PathUtils.h"
#include <Windows.h>
#include <imgui.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    void Wait(LamaPon::WebExportJob& job, DWORD timeout = 10000)
    {
        const auto deadline = GetTickCount64() + timeout;
        while (!job.Poll())
        {
            if (GetTickCount64() > deadline) throw std::runtime_error("Web job timed out.");
            Sleep(10);
        }
    }
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        // 同じプロセス境界を、実SDKでの手動スモーク検査にも使えます。
        if (argc == 7 && std::wstring_view(argv[1]) == L"--export")
        {
            LamaPon::WebExportJob job;
            job.Start(argv[2], argv[3], argv[4], {argv[5], argv[6]});
            Wait(job, 600000);
            std::cout << job.Message() << "\nLog: " << LamaPon::PathToUtf8(job.LogPath()) << '\n';
            return job.Succeeded() ? 0 : 1;
        }
        Require(argc == 2, "Python executable argument is required.");
        const auto root = std::filesystem::current_path() / L"test-output"
            / (L"export 日本語 & paths-" + std::to_wstring(GetCurrentProcessId()));
        const auto engine = root / L"engine";
        const auto project = root / L"project" / L".lamapon" / L"project.json";
        const auto output = root / L"output & HTML";
        std::filesystem::create_directories(engine / L"tools");
        std::filesystem::create_directories(project.parent_path());
        std::ofstream(project) << "{}";
        std::ofstream(engine / L"tools" / L"editor_web_export.py") << R"PY(
import argparse, json, time
from pathlib import Path
p=argparse.ArgumentParser()
for flag in ('project','output','result','emsdk'): p.add_argument('--'+flag)
a=p.parse_args()
if Path(a.project).name == 'slow.json': time.sleep(60)
ok=Path(a.project).name != 'fail.json'
output=Path(a.output)
output.mkdir(parents=True, exist_ok=True)
page=output/'game.html'
if ok: page.write_text('<canvas></canvas>', encoding='utf-8')
Path(a.result).write_text(json.dumps({'ok':ok,'message':'completed' if ok else 'rejected',
    'htmlPath':str(page)}, ensure_ascii=False), encoding='utf-8')
raise SystemExit(0 if ok else 2)
)PY";
        const LamaPon::WebExportTools tools{argv[1], {}};
        LamaPon::WebExportJob job;
        job.Start(engine, project, output, tools);
        Require(job.Running(), "Start must return before the job is collected.");
        Wait(job);
        Require(job.Succeeded() && job.HtmlPath() == output / L"game.html",
            "UTF-8 paths and successful output must survive the process boundary.");
        Require(!job.Poll(), "Completion must only be delivered once.");
        job.Start(engine, project.parent_path() / L"fail.json", output, tools);
        Wait(job);
        Require(!job.Succeeded() && job.Message() == "rejected",
            "Failed result must replace the earlier success.");
        {
            LamaPon::WebExportJob interrupted;
            interrupted.Start(engine, project.parent_path() / L"slow.json", output, tools);
            const auto started = GetTickCount64();
            Require(!interrupted.Poll() && GetTickCount64() - started < 500,
                "Polling must not block the editor.");
        }

        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2{1280, 720};
        io.DeltaTime = 1.0f / 60.0f;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        ImGui::GetPlatformIO().Renderer_TextureMaxWidth = 4096;
        ImGui::GetPlatformIO().Renderer_TextureMaxHeight = 4096;
        {
            LamaPon::GameExportDialog dialog;
            const auto projectRoot = project.parent_path().parent_path();
            dialog.Open(projectRoot);
            Require(dialog.Target() == LamaPon::GameExportTarget::Windows,
                "Existing projects must default to Windows.");
            const LamaPon::GameExportDialogContext context{engine, engine, projectRoot / L"assets", project,
                {}, [] {}, [](std::string, bool) {}, [](const std::filesystem::path&)
                    -> std::optional<std::filesystem::path> { return std::nullopt; }};
            for (int frame = 0; frame < 4; ++frame)
            {
                if (frame == 2)
                {
                    dialog.SelectTarget(LamaPon::GameExportTarget::Web);
                    Require(dialog.OutputDirectory() == projectRoot / L"dist" / L"LamaPonWeb",
                        "Selecting Web must choose a separate default output directory.");
                }
                ImGui::NewFrame();
                dialog.Draw(context);
                ImGui::Render();
                for (auto* texture : ImGui::GetPlatformIO().Textures)
                {
                    if (texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_WantUpdates)
                    { texture->SetTexID(1); texture->SetStatus(ImTextureStatus_OK); }
                }
                // Modalの初回フレームはImGuiが寸法を計測するため描画を保留します。
                if (frame % 2 == 1)
                    Require(ImGui::GetDrawData()->TotalVtxCount > 0, "Both export targets must render independently of EditorLayer.");
            }
        }
        ImGui::DestroyContext();
        std::cout << "Export dialog and Web process tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
