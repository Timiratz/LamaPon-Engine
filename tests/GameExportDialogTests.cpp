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
    // 条件不成立ならテストを失敗させます。
    // Require(condition: 成立条件, message: 失敗理由)
    void Require(bool condition, const char* message)
    {
        // 失敗理由を例外で通知
        if (!condition) throw std::runtime_error(message);
    }

    // 完了までjobを待ち、timeout超過を失敗にします。
    // Wait(job: Web出力, timeout: 待機上限ミリ秒)
    void Wait(LamaPon::WebExportJob& job, DWORD timeout = 10000)
    {
        // 待機期限
        const auto deadline = GetTickCount64() + timeout;
        // job完了まで短時間ずつ状態を確認
        while (!job.Poll())
        {
            // 待機期限を超えたjobを失敗させる
            if (GetTickCount64() > deadline) throw std::runtime_error("Web job timed out.");
            Sleep(10);
        }
    }
}

// Web出力プロセス境界とダイアログ描画を検証します。
// wmain(argc: 引数数, argv: コマンドライン引数)
int wmain(int argc, wchar_t** argv)
{
    // テスト例外を終了コードへ変換
    try
    {
        // 同じプロセス境界を、実SDKでの手動スモーク検査にも使えます。
        // --export指定時は実SDKの手動スモークを行う
        if (argc == 7 && std::wstring_view(argv[1]) == L"--export")
        {
            // 実SDKを使うWeb出力job
            LamaPon::WebExportJob job;
            job.Start(argv[2], argv[3], argv[4], {argv[5], argv[6]});
            Wait(job, 600000);
            std::cout << job.Message() << "\nLog: " << LamaPon::PathToUtf8(job.LogPath()) << '\n';
            return job.Succeeded() ? 0 : 1;
        }
        // 通常テストではPython実行ファイルを1件受け取る
        Require(argc == 2, "Python executable argument is required.");
        // 空白・日本語を含むテスト作業ルート
        const auto root = std::filesystem::current_path() / L"test-output"
            / (L"export 日本語 & paths-" + std::to_wstring(GetCurrentProcessId()));
        // Pythonスタブの配置先
        const auto engine = root / L"engine";
        // テストプロジェクト設定ファイル
        const auto project = root / L"project" / L".lamapon" / L"project.json";
        // Web出力先
        const auto output = root / L"output & HTML";
        std::filesystem::create_directories(engine / L"tools");
        std::filesystem::create_directories(project.parent_path());
        std::ofstream(project) << "{}";
        std::ofstream(engine / L"tools" / L"editor_web_export.py") << R"PY(
import argparse, json, time
from pathlib import Path
# p: スタブ用引数解析器
p=argparse.ArgumentParser()
# flag: CLIが受け取る出力条件
for flag in ('project','output','result','emsdk'): p.add_argument('--'+flag)
# a: スタブへ渡された引数
a=p.parse_args()
# slow.jsonは非同期停止検査用に長時間待つ
if Path(a.project).name == 'slow.json': time.sleep(60)
# ok: プロジェクト名から決める出力成否
ok=Path(a.project).name != 'fail.json'
# output: スタブのWeb出力先
output=Path(a.output)
output.mkdir(parents=True, exist_ok=True)
# page: 生成HTMLの出力パス
page=output/'game.html'
# 成功ケースだけHTMLを生成
if ok: page.write_text('<canvas></canvas>', encoding='utf-8')
Path(a.result).write_text(json.dumps({'ok':ok,'message':'completed' if ok else 'rejected',
    'htmlPath':str(page)}, ensure_ascii=False), encoding='utf-8')
raise SystemExit(0 if ok else 2)
)PY";
        // Python子プロセスへ渡す実行環境
        const LamaPon::WebExportTools tools{argv[1], {}};
        // Web出力の成功・失敗を確認するjob
        LamaPon::WebExportJob job;
        job.Start(engine, project, output, tools);
        // 非同期開始直後に呼び出しが戻ることを確認
        Require(job.Running(), "Start must return before the job is collected.");
        Wait(job);
        Require(job.Succeeded() && job.HtmlPath() == output / L"game.html",
            "UTF-8 paths and successful output must survive the process boundary.");
        // 完了結果を一度だけ受け取れることを確認
        Require(!job.Poll(), "Completion must only be delivered once.");
        job.Start(engine, project.parent_path() / L"fail.json", output, tools);
        Wait(job);
        // 失敗結果で直前の成功状態が置き換わることを確認
        Require(!job.Succeeded() && job.Message() == "rejected",
            "Failed result must replace the earlier success.");
        {
            // 終了前に破棄する非同期job
            LamaPon::WebExportJob interrupted;
            interrupted.Start(engine, project.parent_path() / L"slow.json", output, tools);
            // Pollが返るまでの経過時間
            const auto started = GetTickCount64();
            Require(!interrupted.Poll() && GetTickCount64() - started < 500,
                "Polling must not block the editor.");
        }

        // ダイアログ描画用ImGuiコンテキスト
        ImGui::CreateContext();
        // 描画フレームのImGui設定
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2{1280, 720};
        io.DeltaTime = 1.0f / 60.0f;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        ImGui::GetPlatformIO().Renderer_TextureMaxWidth = 4096;
        ImGui::GetPlatformIO().Renderer_TextureMaxHeight = 4096;
        {
            // エクスポート操作ダイアログ
            LamaPon::GameExportDialog dialog;
            // テストプロジェクトのルート
            const auto projectRoot = project.parent_path().parent_path();
            dialog.Open(projectRoot);
            Require(dialog.Target() == LamaPon::GameExportTarget::Windows,
                "Existing projects must default to Windows.");
            // ダイアログへ渡すプロジェクト・出力・通知依存
            const LamaPon::GameExportDialogContext context{engine, engine, projectRoot / L"assets", project,
                {}, [] {}, [](std::string, bool) {}, [](const std::filesystem::path&)
                    -> std::optional<std::filesystem::path> { return std::nullopt; }};
            // 全出力形式を切り替え、各形式の入力欄を実際に描画します。
            const LamaPon::GameExportTarget targets[]{
                LamaPon::GameExportTarget::Windows, LamaPon::GameExportTarget::Web,
                LamaPon::GameExportTarget::LinuxBuildProject,
                LamaPon::GameExportTarget::AndroidBuildProject,
                LamaPon::GameExportTarget::AndroidApk };
            const wchar_t* const outputNames[]{ L"LamaPonGame", L"LamaPonWeb",
                L"LamaPonLinuxBuild", L"LamaPonAndroidBuild", L"LamaPonAndroidApk" };
            for (int frame = 0; frame < 10; ++frame)
            {
                if (frame % 2 == 0)
                {
                    dialog.SelectTarget(targets[frame / 2]);
                    Require(dialog.OutputDirectory() == projectRoot / L"dist" / outputNames[frame / 2],
                        "Each export target must choose its own default output directory.");
                }
                ImGui::NewFrame();
                dialog.Draw(context);
                ImGui::Render();
                // ImGuiプラットフォームテクスチャを確定
                for (auto* texture : ImGui::GetPlatformIO().Textures)
                {
                    // 作成・更新待ちを正常状態へ進める
                    if (texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_WantUpdates)
                    { texture->SetTexID(1); texture->SetStatus(ImTextureStatus_OK); }
                }
                // Modalの初回フレームはImGuiが寸法を計測するため描画を保留します。
                // 計測完了後は各ターゲットの描画を確認
                if (frame % 2 == 1)
                    Require(ImGui::GetDrawData()->TotalVtxCount > 0, "All export targets must render independently of EditorLayer.");
            }
        }
        ImGui::DestroyContext();
        std::cout << "Export dialog and Web process tests passed.\n";
        return 0;
    }
    // テスト例外を標準エラーと失敗終了コードに変換
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
