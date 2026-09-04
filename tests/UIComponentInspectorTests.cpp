#include "LamaPon/Editor/UIComponentInspectors.h"
#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Components/UICanvasComponent.h"
#include "LamaPon/Components/UIRectTransformComponent.h"
#include "LamaPon/Components/UIButtonComponent.h"
#include "LamaPon/Components/UIImageComponent.h"
#include "LamaPon/Components/UIToggleComponent.h"
#include "LamaPon/Components/UISliderComponent.h"
#include "LamaPon/Components/UIInputFieldComponent.h"
#include "LamaPon/Components/UILayoutGroupComponent.h"
#include "LamaPon/Components/UIScrollViewComponent.h"
#include "LamaPon/Components/RotatorComponent.h"

#include <imgui.h>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{
    void Require(const bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
}

int main()
{
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

        // Scene・GraphicsDevice・EditorLayerを生成せずに全担当を描画します。
        LamaPon::GameObject object(1, "UI inspector regression");
        auto& image = object.AddComponent<LamaPon::UIImageComponent>();
        const std::vector<LamaPon::Component*> components{
            &object.AddComponent<LamaPon::UICanvasComponent>(),
            &object.AddComponent<LamaPon::UIRectTransformComponent>(),
            &object.AddComponent<LamaPon::UIButtonComponent>(), &image,
            &object.AddComponent<LamaPon::UIToggleComponent>(),
            &object.AddComponent<LamaPon::UISliderComponent>(),
            &object.AddComponent<LamaPon::UIInputFieldComponent>(),
            &object.AddComponent<LamaPon::UILayoutGroupComponent>(),
            &object.AddComponent<LamaPon::UIScrollViewComponent>() };
        LamaPon::RotatorComponent unsupported;
        std::filesystem::path selectedAsset;
        int historyCount{}, pickerCalls{}, frame{};
        LamaPon::UIInspectorContext context{
            selectedAsset, 1280, 720,
            [&] { ++historyCount; },
            [](const std::string& message, bool) { throw std::runtime_error(message); },
            [&](const char* id, const std::string& current)
            {
                Require(std::string_view(id) == "UIImageRenderTexture", "Unexpected picker request");
                ++pickerCalls;
                if (frame == 0)
                {
                    Require(current.empty(), "Initial image must have no render texture");
                    return LamaPon::RenderTexturePickerResult{ "Camera preview", false };
                }
                Require(current == "Camera preview", "Edited value must survive until commit");
                return LamaPon::RenderTexturePickerResult{ std::nullopt, frame == 1 };
            } };
        for (; frame < 3; ++frame)
        {
            ImGui::NewFrame();
            ImGui::Begin("UI inspector regression");
            for (auto* component : components)
            {
                ImGui::PushID(component);
                Require(LamaPon::DrawUIComponentInspector(*component, context), "UI component must have an inspector");
                ImGui::PopID();
            }
            Require(!LamaPon::DrawUIComponentInspector(unsupported, context),
                "Unsupported component must be delegated without side effects");
            ImGui::End();
            ImGui::Render();
            for (auto* texture : ImGui::GetPlatformIO().Textures)
            {
                if (texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_WantUpdates)
                {
                    texture->SetTexID(1);
                    texture->SetStatus(ImTextureStatus_OK);
                }
            }
            Require(historyCount == (frame == 0 ? 0 : 1),
                "Edit must be recorded once at commit, not during every frame");
        }
        Require(pickerCalls == 3 && image.RenderTexture() == "Camera preview",
            "Image inspector must apply the narrow picker result");
        Require(ImGui::GetDrawData()->TotalVtxCount > 0, "Independent inspectors must emit draw commands");
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    ImGui::DestroyContext();
    return result;
}
