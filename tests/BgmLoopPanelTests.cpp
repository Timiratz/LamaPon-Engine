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
    void Require(const bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }
}

int main()
{
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ImGui::CreateContext();
    int result{};
    try
    {
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1280, 720);
        io.DeltaTime = 1.0f / 60.0f;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        ImGui::GetPlatformIO().Renderer_TextureMaxWidth = 4096;
        ImGui::GetPlatformIO().Renderer_TextureMaxHeight = 4096;
        const auto root = std::filesystem::current_path();
        const auto catalog = root / "test-output" / "bgm-panel.json";
        std::filesystem::create_directories(catalog.parent_path());
        std::ofstream(catalog) << R"({"tracks":[{"name":"Startup","asset":"assets/audio/startup.wav"}]})";
        LamaPon::AudioSystem audio;
        LamaPon::AssetManager assets(nullptr, nullptr);
        bool failed{};
        int notifications{};
        auto status = [&](std::string, const bool error)
        {
            failed = failed || error;
            ++notifications;
        };
        {
            LamaPon::BgmLoopPanel panel(audio, assets, root, catalog, status);
            Require(panel.Matches(catalog), "Panel must retain its catalog identity.");
            bool open = true;
            // EditorLayerを作らずに、カタログ読み込みから波形描画まで
            // 実行できることを確認します。音声の再生操作は行いません。
            for (int frame = 0; frame < 2; ++frame)
            {
                ImGui::NewFrame();
                panel.Draw("BGM regression", open, {});
                ImGui::Render();
                for (auto* texture : ImGui::GetPlatformIO().Textures)
                {
                    if (texture->Status == ImTextureStatus_WantCreate
                        || texture->Status == ImTextureStatus_WantUpdates)
                    {
                        texture->SetTexID(1);
                        texture->SetStatus(ImTextureStatus_OK);
                    }
                }
            }
            Require(!failed && notifications > 0,
                "Valid catalog and waveform must load without errors.");
            Require(ImGui::GetDrawData()->TotalVtxCount > 0,
                "BGM panel must emit drawing commands independently.");
            panel.StopPreview();
        }
        std::ofstream(catalog, std::ios::trunc) << R"({"tracks":[{"asset":false}]})";
        {
            LamaPon::BgmLoopPanel panel(audio, assets, root, catalog, status);
            Require(failed, "Malformed catalog must produce an actionable error.");
            bool open = true;
            ImGui::NewFrame();
            panel.Draw("Invalid BGM regression", open, {});
            ImGui::Render();
        }
        std::cout << "BGM panel tests passed.\n";
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        result = 1;
    }
    ImGui::DestroyContext();
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
