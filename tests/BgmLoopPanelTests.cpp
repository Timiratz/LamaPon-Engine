#include "LamaPon/Editor/BgmLoopPanel.h"
#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Audio/AudioSystem.h"

#include <Windows.h>
#include <imgui.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
    // 条件不成立ならテストを失敗させます。
    // Require(value: 成立条件, message: 失敗理由)
    void Require(const bool value, const char* message)
    {
        // 失敗理由を例外として通知
        if (!value) throw std::runtime_error(message);
    }
}

// BGMパネルの描画と不正カタログを検証します。
int main()
{
    // COM初期化結果
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ImGui::CreateContext();
    // 例外を終了コードに変換する結果
    int result{};
    // テスト資源を破棄する前提で実行
    try
    {
        // ImGui入出力設定
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1280, 720);
        io.DeltaTime = 1.0f / 60.0f;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        ImGui::GetPlatformIO().Renderer_TextureMaxWidth = 4096;
        ImGui::GetPlatformIO().Renderer_TextureMaxHeight = 4096;
        // テスト実行ルート
        const auto root = std::filesystem::current_path();
        // テスト用カタログの出力先
        const auto catalog = root / "test-output" / "bgm-panel.json";
        std::filesystem::create_directories(catalog.parent_path());
        std::ofstream(catalog) << R"({"tracks":[{"name":"Startup","asset":"assets/audio/startup.wav"}]})";
        // BGM試聴に使う音声システム
        LamaPon::AudioSystem audio;
        // テクスチャ解決用の空資産管理
        LamaPon::AssetManager assets(nullptr, nullptr);
        // 通知結果の失敗有無
        bool failed{};
        // ステータス通知回数
        int notifications{};
        // status(message: 通知文, error: エラー通知か)
        auto status = [&](std::string, const bool error)
        {
            failed = failed || error;
            ++notifications;
        };
        // パネル破棄後にImGui資源を終了
        {
            // 正常カタログを表示するパネル
            LamaPon::BgmLoopPanel panel(audio, assets, root, catalog, status);
            Require(panel.Matches(catalog), "Panel must retain its catalog identity.");
            // 描画テスト中はパネルを開いた状態にする
            bool open = true;
            // EditorLayerを作らずに、カタログ読み込みから波形描画まで実行できることを確認します。
            // 音声の再生操作は行いません。
            // EditorLayerなしで2フレーム描画
            for (int frame = 0; frame < 2; ++frame)
            {
                ImGui::NewFrame();
                panel.Draw("BGM regression", open, {});
                ImGui::Render();
                // ImGuiプラットフォームテクスチャを確定
                for (auto* texture : ImGui::GetPlatformIO().Textures)
                {
                    // 作成または更新要求を正常状態へ進める
                    if (texture->Status == ImTextureStatus_WantCreate
                        || texture->Status == ImTextureStatus_WantUpdates)
                    {
                        texture->SetTexID(1);
                        texture->SetStatus(ImTextureStatus_OK);
                    }
                }
            }
            // 正常カタログの読み込みと描画を確認
            Require(!failed && notifications > 0,
                "Valid catalog and waveform must load without errors.");
            // 実描画コマンドが生成されたことを確認
            Require(ImGui::GetDrawData()->TotalVtxCount > 0,
                "BGM panel must emit drawing commands independently.");
            panel.StopPreview();
        }
        // 不正カタログに差し替え
        std::ofstream(catalog, std::ios::trunc) << R"({"tracks":[{"asset":false}]})";
        // 不正入力時のエラー通知と描画を確認
        {
            // 不正カタログを表示するパネル
            LamaPon::BgmLoopPanel panel(audio, assets, root, catalog, status);
            Require(failed, "Malformed catalog must produce an actionable error.");
            // エラー表示中もパネルを開く
            bool open = true;
            ImGui::NewFrame();
            panel.Draw("Invalid BGM regression", open, {});
            ImGui::Render();
        }
        std::cout << "BGM panel tests passed.\n";
    }
    // テスト例外を失敗コードに変換
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        result = 1;
    }
    ImGui::DestroyContext();
    // COM初期化に成功した場合だけ対応する終了処理を行う
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
