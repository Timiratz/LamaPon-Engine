// EditorLayerのScene/Gameビューポート描画、ギズモ、シーンカメラ、タイル描画をまとめた翻訳単位です。
#include "LamaPon/Editor/EditorLayer.h"

#include "LamaPon/Editor/EditorLayerShared.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Components/BoxCollider2DComponent.h"
#include "LamaPon/Components/BoxCollider3DComponent.h"
#include "LamaPon/Components/CameraComponent.h"
#include "LamaPon/Components/CapsuleCollider3DComponent.h"
#include "LamaPon/Components/CharacterControllerComponent.h"
#include "LamaPon/Components/CircleCollider2DComponent.h"
#include "LamaPon/Components/PolygonCollider2DComponent.h"
#include "LamaPon/Components/SpriteMaskComponent.h"
#include "LamaPon/Components/ConvexHullCollider3DComponent.h"
#include "LamaPon/Components/DirectionalLightComponent.h"
#include "LamaPon/Components/MeshCollider3DComponent.h"
#include "LamaPon/Components/MeshRendererComponent.h"
#include "LamaPon/Components/ModelRendererComponent.h"
#include "LamaPon/Components/NavMeshComponent.h"
#include "LamaPon/Components/ParticleSystemComponent.h"
#include "LamaPon/Components/PointLightComponent.h"
#include "LamaPon/Components/SphereCollider3DComponent.h"
#include "LamaPon/Components/SpotLightComponent.h"
#include "LamaPon/Components/TilemapComponent.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Input/InputSystem.h"
#include "LamaPon/Physics/CollisionTypes.h"
#include "LamaPon/Physics/Raycast.h"
#include "LamaPon/Scene/Scene.h"

#include <imgui.h>
// ImGuizmo.hはimgui.hを自前でincludeしないため、必ずこの順で。
#include <ImGuizmo.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

using namespace LamaPon::EditorDetail;

namespace
{
    bool IsOrbitViewportNavigationMouseInput(
        const LamaPon::ViewportNavigationPreset preset)
    {
        if (preset != LamaPon::ViewportNavigationPreset::Orbit)
        {
            return false;
        }

        const bool altDown =
            ImGui::IsKeyDown(ImGuiKey_LeftAlt)
            || ImGui::IsKeyDown(ImGuiKey_RightAlt);
        return ImGui::IsMouseDown(ImGuiMouseButton_Middle)
            || (altDown
                && (ImGui::IsMouseDown(ImGuiMouseButton_Left)
                    || ImGui::IsMouseDown(ImGuiMouseButton_Right)));
    }

    std::optional<LamaPon::Bounds3D> SelectionBounds(
        const LamaPon::GameObject& gameObject)
    {
        if (const auto* collider =
            gameObject.GetComponent<LamaPon::BoxCollider2DComponent>())
        {
            const auto bounds = collider->WorldBounds();
            DirectX::XMFLOAT4X4 world{};
            DirectX::XMStoreFloat4x4(
                &world,
                gameObject.WorldMatrix());
            return LamaPon::Bounds3D{
                { bounds.minimum.x, bounds.minimum.y, world._43 - 0.05f },
                { bounds.maximum.x, bounds.maximum.y, world._43 + 0.05f }
            };
        }
        if (const auto* collider =
            gameObject.GetComponent<
                LamaPon::CircleCollider2DComponent>())
        {
            const auto bounds = collider->WorldBounds();
            DirectX::XMFLOAT4X4 world{};
            DirectX::XMStoreFloat4x4(
                &world,
                gameObject.WorldMatrix());
            return LamaPon::Bounds3D{
                { bounds.minimum.x, bounds.minimum.y, world._43 - 0.05f },
                { bounds.maximum.x, bounds.maximum.y, world._43 + 0.05f }
            };
        }
        if (const auto* collider =
            gameObject.GetComponent<
                LamaPon::PolygonCollider2DComponent>())
        {
            const auto bounds = collider->WorldBounds();
            DirectX::XMFLOAT4X4 world{};
            DirectX::XMStoreFloat4x4(
                &world,
                gameObject.WorldMatrix());
            return LamaPon::Bounds3D{
                { bounds.minimum.x, bounds.minimum.y, world._43 - 0.05f },
                { bounds.maximum.x, bounds.maximum.y, world._43 + 0.05f }
            };
        }
        if (const auto* spriteMask =
            gameObject.GetComponent<
                LamaPon::SpriteMaskComponent>())
        {
            const auto center = spriteMask->WorldPosition();
            const auto& size = spriteMask->Size();
            const float halfWidth = size.x * 0.5f;
            const float halfHeight =
                spriteMask->Shape()
                        == LamaPon::SpriteMaskShape::Circle
                    ? halfWidth
                    : size.y * 0.5f;
            DirectX::XMFLOAT4X4 world{};
            DirectX::XMStoreFloat4x4(
                &world,
                gameObject.WorldMatrix());
            return LamaPon::Bounds3D{
                {
                    center.x - halfWidth,
                    center.y - halfHeight,
                    world._43 - 0.05f
                },
                {
                    center.x + halfWidth,
                    center.y + halfHeight,
                    world._43 + 0.05f
                }
            };
        }
        if (const auto* controller =
            gameObject.GetComponent<
                LamaPon::CharacterControllerComponent>())
        {
            return controller->WorldBounds();
        }
        if (const auto* collider =
            gameObject.GetComponent<LamaPon::BoxCollider3DComponent>())
        {
            return collider->WorldBounds();
        }
        if (const auto* capsule =
            gameObject.GetComponent<
                LamaPon::CapsuleCollider3DComponent>())
        {
            return capsule->WorldBounds();
        }
        if (const auto* sphere =
            gameObject.GetComponent<
                LamaPon::SphereCollider3DComponent>())
        {
            return sphere->WorldBounds();
        }
        if (const auto* hull =
            gameObject.GetComponent<
                LamaPon::ConvexHullCollider3DComponent>())
        {
            return hull->WorldBounds();
        }
        if (const auto* meshCollider =
            gameObject.GetComponent<
                LamaPon::MeshCollider3DComponent>())
        {
            return meshCollider->WorldBounds();
        }
        if (const auto* navMesh =
            gameObject.GetComponent<
                LamaPon::NavMeshComponent>())
        {
            DirectX::XMFLOAT4X4 world{};
            DirectX::XMStoreFloat4x4(
                &world,
                gameObject.WorldMatrix());
            const auto size =
                navMesh->SurfaceSize();
            return LamaPon::Bounds3D{
                {
                    world._41 - size.x * 0.5f,
                    world._42 - 0.05f,
                    world._43 - size.y * 0.5f
                },
                {
                    world._41 + size.x * 0.5f,
                    world._42 + 0.05f,
                    world._43 + size.y * 0.5f
                }
            };
        }

        LamaPon::Bounds3D localBounds{
            { -0.5f, -0.5f, -0.5f },
            { 0.5f, 0.5f, 0.5f }
        };

        if (const auto* mesh =
            gameObject.GetComponent<LamaPon::MeshRendererComponent>())
        {
            if (mesh->Shape() == LamaPon::PrimitiveShape::Plane)
            {
                localBounds.minimum.y = -0.025f;
                localBounds.maximum.y = 0.025f;
            }
        }
        else if (gameObject.GetComponent<LamaPon::CameraComponent>()
            != nullptr)
        {
            localBounds.minimum = { -0.25f, -0.18f, -0.35f };
            localBounds.maximum = { 0.25f, 0.18f, 0.15f };
        }
        else if (gameObject.GetComponent<
            LamaPon::DirectionalLightComponent>() != nullptr)
        {
            localBounds.minimum = { -0.25f, -0.25f, -0.25f };
            localBounds.maximum = { 0.25f, 0.25f, 0.25f };
        }
        else if (gameObject.GetComponent<
                LamaPon::PointLightComponent>() != nullptr
            || gameObject.GetComponent<
                LamaPon::SpotLightComponent>() != nullptr)
        {
            localBounds.minimum = { -0.3f, -0.3f, -0.3f };
            localBounds.maximum = { 0.3f, 0.3f, 0.3f };
        }
        else if (const auto* particles =
            gameObject.GetComponent<
                LamaPon::ParticleSystemComponent>())
        {
            const auto size =
                particles->EmitterSize();
            localBounds.minimum = {
                -std::max(size.x, 0.2f) * 0.5f,
                -std::max(size.y, 0.2f) * 0.5f,
                -std::max(size.z, 0.2f) * 0.5f
            };
            localBounds.maximum = {
                std::max(size.x, 0.2f) * 0.5f,
                std::max(size.y, 0.2f) * 0.5f,
                std::max(size.z, 0.2f) * 0.5f
            };
        }
        else if (gameObject.GetComponent<LamaPon::ModelRendererComponent>()
            == nullptr)
        {
            return std::nullopt;
        }

        return LamaPon::TransformBounds(localBounds, gameObject.WorldMatrix());
    }
}

namespace LamaPon
{
    void EditorLayer::DrawViewport()
    {
        ImGui::SetNextWindowSize(
            ImVec2{ 720.0f, 640.0f },
            ImGuiCond_FirstUseEver);

        constexpr ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoCollapse
            | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse;

        ImGui::Begin("ビューポート", nullptr, flags);
        m_activeViewport = ViewportMode::None;
        m_sceneViewportHovered = false;

        bool drawSceneViewport = false;
        bool drawGameViewport = false;

        ImGui::PushStyleColor(
            ImGuiCol_Tab,
            ImVec4{ 0.10f, 0.22f, 0.36f, 1.0f });
        ImGui::PushStyleColor(
            ImGuiCol_TabHovered,
            ImVec4{ 0.18f, 0.42f, 0.68f, 1.0f });
        ImGui::PushStyleColor(
            ImGuiCol_TabSelected,
            ImVec4{ 0.16f, 0.38f, 0.64f, 1.0f });
        ImGui::PushStyleColor(
            ImGuiCol_TabDimmed,
            ImVec4{ 0.10f, 0.22f, 0.36f, 1.0f });
        ImGui::PushStyleColor(
            ImGuiCol_TabDimmedSelected,
            ImVec4{ 0.16f, 0.38f, 0.64f, 1.0f });
        ImGui::PushStyleColor(
            ImGuiCol_Text,
            ImVec4{ 0.92f, 0.95f, 1.0f, 1.0f });

        if (ImGui::BeginTabBar("ViewportTabs"))
        {
            if (ImGui::BeginTabItem("シーン"))
            {
                drawSceneViewport = true;
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("ゲーム"))
            {
                drawGameViewport = true;
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
        ImGui::PopStyleColor(6);

        // Submit both tab headers before the viewport content.  The viewport
        // uses the same draw list for its image and overlays, so drawing it
        // between tab items can hide a later inactive tab on some frames.
        if (drawSceneViewport)
        {
            m_activeViewport = ViewportMode::Scene;
            DrawSceneViewport();
        }
        else if (drawGameViewport)
        {
            m_activeViewport = ViewportMode::Game;
            DrawGameViewport();
        }

        if (m_activeViewport != ViewportMode::Scene && m_gizmoWasUsing)
        {
            RecordHistory();
            m_gizmoWasUsing = false;
        }

        ImGui::End();
    }

    void EditorLayer::DrawSceneViewport()
    {
        m_viewCubeHovered = false;

        ImVec2 size = ImGui::GetContentRegionAvail();
        size.x = std::max(size.x, 16.0f);
        size.y = std::max(size.y, 16.0f);

        m_sceneRenderTarget.Resize(
            m_graphics.Device(),
            std::max(
                static_cast<std::uint32_t>(
                    std::lround(
                        size.x
                        * m_graphics.Settings().
                            renderScale)),
                1u),
            std::max(
                static_cast<std::uint32_t>(
                    std::lround(
                        size.y
                        * m_graphics.Settings().
                            renderScale)),
                1u));

        const ImVec2 position = ImGui::GetCursorScreenPos();
        m_viewportPosition = { position.x, position.y };
        m_viewportSize = { size.x, size.y };

        // ポスト処理でテクスチャが入れ替わっても表示が最終結果を
        // 指すよう、表示専用SRVを使います。
        ImGui::Image(
            MakeTextureReference(
                m_sceneRenderTarget.DisplayShaderResourceView()),
            size);
        const bool viewportImageHovered = ImGui::IsItemHovered();
        // Asset BrowserからScene Viewへのドロップは、落とした場所へ
        // 配置します（画像の直後で受けるとImGui::Imageが対象になる）。
        HandleSceneViewAssetDrop();
        const ImVec2 cursorAfterViewport = ImGui::GetCursorScreenPos();
        bool cameraPreviewHovered = false;

        ImGui::SetCursorScreenPos(
            ImVec2{ position.x + 8.0f, position.y + 8.0f });
        const auto drawModeButton = [this](
            const char* label,
            const bool active,
            const bool mode2D)
        {
            if (active)
            {
                ImGui::PushStyleColor(
                    ImGuiCol_Button,
                    ImVec4{ 0.16f, 0.43f, 0.75f, 1.0f });
                ImGui::PushStyleColor(
                    ImGuiCol_ButtonHovered,
                    ImVec4{ 0.20f, 0.50f, 0.86f, 1.0f });
            }
            const bool clicked = ImGui::SmallButton(label);
            if (active)
            {
                ImGui::PopStyleColor(2);
            }
            if (clicked && !active)
            {
                SetScene2DMode(mode2D);
            }
        };

        drawModeButton("2D##SceneViewMode", m_scene2DMode, true);
        bool viewportToolbarHovered = ImGui::IsItemHovered();
        ImGui::SameLine(0.0f, 2.0f);
        drawModeButton("3D##SceneViewMode", !m_scene2DMode, false);
        viewportToolbarHovered =
            viewportToolbarHovered || ImGui::IsItemHovered();
        if (viewportToolbarHovered)
        {
            ImGui::SetTooltip(
                "2Dは正面のXY平面、3Dは自由視点で表示します。");
        }
        ImGui::SameLine(0.0f, 6.0f);
        if (ImGui::SmallButton("設定"))
        {
            ImGui::OpenPopup("SceneCameraSettings");
        }
        viewportToolbarHovered =
            viewportToolbarHovered || ImGui::IsItemHovered();
        DrawSceneCameraSettings();

        const auto* selected =
            m_scene.FindGameObject(m_selectedObjectId);
        const auto* selectedCamera = selected != nullptr
            ? selected->GetComponent<CameraComponent>()
            : nullptr;
        if (selectedCamera != nullptr)
        {
            float previewWidth = std::clamp(
                size.x * 0.28f,
                160.0f,
                320.0f);
            previewWidth = std::min(
                previewWidth,
                std::max(size.x - 24.0f, 16.0f));
            float previewHeight = previewWidth * 9.0f / 16.0f;
            const float maximumPreviewHeight = std::max(
                size.y * 0.36f,
                54.0f);
            if (previewHeight > maximumPreviewHeight)
            {
                previewHeight = maximumPreviewHeight;
                previewWidth = previewHeight * 16.0f / 9.0f;
            }

            if (previewWidth >= 96.0f && previewHeight >= 54.0f)
            {
                const float renderScale =
                    m_graphics.Settings().renderScale;
                m_cameraPreviewRenderTarget.Resize(
                    m_graphics.Device(),
                    std::max(
                        static_cast<std::uint32_t>(
                            std::lround(
                                previewWidth * renderScale)),
                        1u),
                    std::max(
                        static_cast<std::uint32_t>(
                            std::lround(
                                previewHeight * renderScale)),
                        1u));

                constexpr float previewMargin = 12.0f;
                constexpr float previewBorder = 3.0f;
                const ImVec2 previewPosition{
                    position.x + size.x - previewWidth - previewMargin,
                    position.y + size.y - previewHeight - previewMargin
                };
                const ImVec2 previewMaximum{
                    previewPosition.x + previewWidth,
                    previewPosition.y + previewHeight
                };
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                drawList->AddRectFilled(
                    ImVec2{
                        previewPosition.x - previewBorder,
                        previewPosition.y - previewBorder
                    },
                    ImVec2{
                        previewMaximum.x + previewBorder,
                        previewMaximum.y + previewBorder
                    },
                    IM_COL32(12, 18, 28, 245),
                    4.0f);

                ImGui::SetCursorScreenPos(previewPosition);
                ImGui::Image(
                    MakeTextureReference(
                        m_cameraPreviewRenderTarget.
                            DisplayShaderResourceView()),
                    ImVec2{ previewWidth, previewHeight });
                cameraPreviewHovered = ImGui::IsItemHovered();
                if (cameraPreviewHovered)
                {
                    ImGui::SetTooltip(
                        "カメラプレビュー: %s",
                        selected->Name().c_str());
                }

                drawList->AddRect(
                    previewPosition,
                    previewMaximum,
                    IM_COL32(82, 158, 235, 255),
                    2.0f,
                    0,
                    2.0f);
                const float labelHeight =
                    ImGui::GetTextLineHeight() + 6.0f;
                drawList->AddRectFilled(
                    previewPosition,
                    ImVec2{
                        previewMaximum.x,
                        previewPosition.y + labelHeight
                    },
                    IM_COL32(10, 18, 30, 205));
                drawList->AddText(
                    ImVec2{
                        previewPosition.x + 6.0f,
                        previewPosition.y + 3.0f
                    },
                    IM_COL32(225, 240, 255, 255),
                    "カメラプレビュー");
            }
        }

        ImGui::SetCursorScreenPos(cursorAfterViewport);
        ImGui::Dummy(ImVec2{});

        m_sceneViewportHovered =
            viewportImageHovered
            && !viewportToolbarHovered
            && !cameraPreviewHovered
            && !ImGui::IsPopupOpen("SceneCameraSettings");

        if (m_sceneViewportHovered
            && !ImGui::IsMouseDown(ImGuiMouseButton_Right)
            && !ImGui::GetIO().WantTextInput)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_F, false))
            {
                FocusSelection();
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_W, false))
            {
                m_gizmoOperation = GizmoOperation::Translate;
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_E, false))
            {
                m_gizmoOperation = GizmoOperation::Rotate;
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_R, false))
            {
                m_gizmoOperation = GizmoOperation::Scale;
            }
        }

        UpdateSceneCamera();
        DrawTransformGizmo();
        if (m_viewCubeVisible)
        {
            DrawViewCube();
        }
        else
        {
            m_viewCubeHovered = false;
        }
        HandleTilemapPainting();

        const auto* selectedObject =
            m_scene.FindGameObject(
                m_selectedObjectId);
        const bool tilemapEditing =
            !m_playing
            && selectedObject != nullptr
            && selectedObject->GetComponent<
                TilemapComponent>() != nullptr;
        if (!m_playing
            && m_sceneViewportHovered
            && !m_viewCubeHovered
            && !tilemapEditing
            && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && !m_transformGizmoHovered
            && !m_transformGizmoUsing
            && !m_transformGizmoMouseCaptured
            && !ImGuizmo::IsUsingViewManipulate()
            && !IsOrbitViewportNavigationMouseInput(
                m_projectSettings.viewport.navigationPreset))
        {
            PickSceneObject();
        }
    }

    void EditorLayer::HandleTilemapPainting()
    {
        using namespace DirectX;

        if (ImGui::IsMouseReleased(
                ImGuiMouseButton_Left)
            && m_tilemapStrokeChanged)
        {
            RecordHistory();
            m_tilemapStrokeChanged = false;
        }

        auto* selected =
            m_scene.FindGameObject(
                m_selectedObjectId);
        auto* tilemap = selected != nullptr
            ? selected->GetComponent<
                TilemapComponent>()
            : nullptr;
        if (m_playing
            || tilemap == nullptr
            || !m_sceneViewportHovered
            || m_viewCubeHovered
            || m_transformGizmoHovered
            || m_transformGizmoUsing
            || m_transformGizmoMouseCaptured
            || ImGuizmo::IsUsingViewManipulate()
            || IsOrbitViewportNavigationMouseInput(
                m_projectSettings.viewport.navigationPreset)
            || m_viewportSize.x <= 0.0f
            || m_viewportSize.y <= 0.0f)
        {
            return;
        }

        const ImVec2 mouse =
            ImGui::GetMousePos();
        const float screenX =
            mouse.x - m_viewportPosition.x;
        const float screenY =
            mouse.y - m_viewportPosition.y;

        XMVECTOR determinant{};
        const XMMATRIX world =
            selected->WorldMatrix();
        const XMMATRIX inverseWorld =
            XMMatrixInverse(
                &determinant,
                world);
        XMFLOAT3 local{};
        XMStoreFloat3(
            &local,
            XMVector3TransformCoord(
                XMVectorSet(
                    screenX,
                    screenY,
                    0.0f,
                    1.0f),
                inverseWorld));

        const auto tileSize =
            tilemap->TileSize();
        const int cellX =
            static_cast<int>(
                std::floor(
                    local.x / tileSize.x));
        const int cellY =
            static_cast<int>(
                std::floor(
                    local.y / tileSize.y));

        std::array<ImVec2, 5> outline{};
        const std::array<XMFLOAT3, 4>
            localCorners{
                XMFLOAT3{
                    cellX * tileSize.x,
                    cellY * tileSize.y,
                    0.0f },
                XMFLOAT3{
                    (cellX + 1) * tileSize.x,
                    cellY * tileSize.y,
                    0.0f },
                XMFLOAT3{
                    (cellX + 1) * tileSize.x,
                    (cellY + 1) * tileSize.y,
                    0.0f },
                XMFLOAT3{
                    cellX * tileSize.x,
                    (cellY + 1) * tileSize.y,
                    0.0f }
            };
        for (std::size_t index = 0;
            index < localCorners.size();
            ++index)
        {
            XMFLOAT3 corner{};
            XMStoreFloat3(
                &corner,
                XMVector3TransformCoord(
                    XMLoadFloat3(
                        &localCorners[index]),
                    world));
            outline[index] = {
                m_viewportPosition.x
                    + corner.x,
                m_viewportPosition.y
                    + corner.y
            };
        }
        outline.back() = outline.front();
        ImGui::GetWindowDrawList()->AddPolyline(
            outline.data(),
            static_cast<int>(outline.size()),
            IM_COL32(40, 220, 255, 255),
            0,
            2.0f);

        if (!ImGui::IsMouseDown(
                ImGuiMouseButton_Left))
        {
            return;
        }
        if (ImGui::IsMouseClicked(
                ImGuiMouseButton_Left))
        {
            m_tilemapStrokeChanged = false;
        }

        bool changed{};
        if (m_tilemapTool
            == TilemapTool::Erase)
        {
            changed =
                tilemap->EraseCell(
                    cellX,
                    cellY);
        }
        else
        {
            m_tilePaletteSelectedTile =
                std::min(
                    m_tilePaletteSelectedTile,
                    tilemap->TileCount() - 1);
            changed = tilemap->SetCell(
                cellX,
                cellY,
                m_tilePaletteSelectedTile);
        }
        m_tilemapStrokeChanged =
            m_tilemapStrokeChanged || changed;
    }

    void EditorLayer::DrawSceneCameraSettings()
    {
        if (!ImGui::BeginPopup("SceneCameraSettings"))
        {
            return;
        }

        ImGui::TextUnformatted("エディター設定");
        ImGui::Separator();

        if (ImGui::CollapsingHeader("操作・表示",
                ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::BeginDisabled(m_playing);
            if (ImGui::RadioButton("移動 (W)",
                    m_gizmoOperation == GizmoOperation::Translate))
            {
                m_gizmoOperation = GizmoOperation::Translate;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("回転 (E)",
                    m_gizmoOperation == GizmoOperation::Rotate))
            {
                m_gizmoOperation = GizmoOperation::Rotate;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("拡縮 (R)",
                    m_gizmoOperation == GizmoOperation::Scale))
            {
                m_gizmoOperation = GizmoOperation::Scale;
            }
            ImGui::Checkbox("ローカル座標", &m_gizmoLocal);
            ImGui::EndDisabled();

            ImGui::BeginDisabled(
                m_scene.FindGameObject(m_selectedObjectId) == nullptr);
            if (ImGui::Button("選択オブジェクトへフォーカス (F)"))
            {
                FocusSelection();
            }
            ImGui::EndDisabled();

            const bool wasOrthographic = m_sceneOrthographic;
            ImGui::Checkbox("正投影", &m_sceneOrthographic);
            if (wasOrthographic && !m_sceneOrthographic)
            {
                m_scene2DMode = false;
                m_scene3DViewStored = false;
            }
            if (m_sceneOrthographic)
            {
                ImGui::SetNextItemWidth(130.0f);
                if (ImGui::DragFloat("表示サイズ",
                        &m_sceneOrthographicSize,
                        0.1f,
                        0.1f,
                        1000.0f,
                        "%.1f"))
                {
                    m_sceneOrthographicSize =
                        std::clamp(m_sceneOrthographicSize, 0.1f, 1000.0f);
                }
            }

            ImGui::Checkbox("グリッドを表示", &m_gridVisible);
            ImGui::Checkbox("ビューキューブを表示", &m_viewCubeVisible);

            // デバッグ線は種類ごとに出し入れできます。作業中の対象
            // だけ残せるよう、まとめてではなく個別にしています。
            ImGui::Checkbox(
                "当たり判定を表示",
                &m_colliderDebugVisible);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "コライダー・NavMesh・Sprite Mask・Light 2Dの"
                    "範囲を線で表示します");
            }
            ImGui::Checkbox(
                "ライトのギズモを表示",
                &m_lightGizmosVisible);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "ポイントライトの範囲、平行光源の向き、"
                    "スポットライトの円錐を表示します");
            }
            ImGui::Checkbox(
                "カメラのギズモを表示",
                &m_cameraGizmosVisible);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Cameraコンポーネントの視錐台を表示します");
            }
            ImGui::SetNextItemWidth(130.0f);
            if (ImGui::DragFloat("グリッド間隔",
                    &m_gridSpacing,
                    0.05f,
                    0.1f,
                    100.0f,
                    "%.2f"))
            {
                m_gridSpacing = std::clamp(m_gridSpacing, 0.1f, 100.0f);
            }
            ImGui::SetNextItemWidth(130.0f);
            if (ImGui::DragFloat("グリッド範囲",
                    &m_gridExtent,
                    1.0f,
                    1.0f,
                    1000.0f,
                    "%.0f"))
            {
                m_gridExtent = std::clamp(m_gridExtent, 1.0f, 1000.0f);
            }

            ImGui::BeginDisabled(m_playing);
            ImGui::Checkbox("スナップ", &m_snapEnabled);
            ImGui::BeginDisabled(!m_snapEnabled);
            ImGui::SetNextItemWidth(130.0f);
            switch (m_gizmoOperation)
            {
            case GizmoOperation::Translate:
                if (ImGui::DragFloat("移動単位",
                        &m_translationSnap,
                        0.05f,
                        0.01f,
                        100.0f,
                        "%.2f"))
                {
                    m_translationSnap =
                        std::clamp(m_translationSnap, 0.01f, 100.0f);
                }
                break;
            case GizmoOperation::Rotate:
                if (ImGui::DragFloat("回転角",
                        &m_rotationSnap,
                        1.0f,
                        1.0f,
                        180.0f,
                        "%.0f°"))
                {
                    m_rotationSnap = std::clamp(m_rotationSnap, 1.0f, 180.0f);
                }
                break;
            case GizmoOperation::Scale:
                if (ImGui::DragFloat("拡縮単位",
                        &m_scaleSnap,
                        0.01f,
                        0.01f,
                        10.0f,
                        "%.2f"))
                {
                    m_scaleSnap = std::clamp(m_scaleSnap, 0.01f, 10.0f);
                }
                break;
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
        }

        if (ImGui::CollapsingHeader("名前付きプリセット"))
        {
            if (!m_editorPresets.empty())
            {
                m_selectedEditorPreset = std::min(m_selectedEditorPreset,
                    m_editorPresets.size() - 1);
                const auto& selectedPreset =
                    m_editorPresets[m_selectedEditorPreset];
                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::BeginCombo("プリセット",
                        selectedPreset.name.c_str()))
                {
                    for (std::size_t index = 0; index < m_editorPresets.size();
                        ++index)
                    {
                        const bool selected = index == m_selectedEditorPreset;
                        if (ImGui::Selectable(
                                m_editorPresets[index].name.c_str(),
                                selected))
                        {
                            try
                            {
                                ApplyEditorPreset(index);
                            }
                            catch (const std::exception& exception)
                            {
                                SetStatus(exception.what(), true);
                            }
                        }
                        if (selected)
                        {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }

                if (ImGui::Button("再適用"))
                {
                    try
                    {
                        ApplyEditorPreset(m_selectedEditorPreset);
                    }
                    catch (const std::exception& exception)
                    {
                        SetStatus(exception.what(), true);
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("現在値で上書き"))
                {
                    try
                    {
                        UpdateSelectedEditorPreset();
                    }
                    catch (const std::exception& exception)
                    {
                        SetStatus(exception.what(), true);
                    }
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(m_editorPresets.size() <= 1);
                if (ImGui::Button("削除"))
                {
                    try
                    {
                        DeleteSelectedEditorPreset();
                    }
                    catch (const std::exception& exception)
                    {
                        SetStatus(exception.what(), true);
                    }
                }
                ImGui::EndDisabled();
            }

            ImGui::SetNextItemWidth(220.0f);
            const bool createPreset = ImGui::InputText("新規名",
                m_editorPresetNameBuffer.data(),
                m_editorPresetNameBuffer.size(),
                ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::Button("現在設定から新規保存") || createPreset)
            {
                try
                {
                    SaveCurrentEditorPreset(m_editorPresetNameBuffer.data());
                }
                catch (const std::exception& exception)
                {
                    SetStatus(exception.what(), true);
                }
            }
            if (!m_editorPresetError.empty())
            {
                ImGui::TextColored(ImVec4{1.0f, 0.35f, 0.30f, 1.0f},
                    "%s",
                    m_editorPresetError.c_str());
            }
            ImGui::TextDisabled(
                "プリセットは視点位置とAsset Browserを変更しません。");
        }

        if (ImGui::CollapsingHeader("シーンカメラ"))
        {

            ImGui::SetNextItemWidth(130.0f);
            if (ImGui::DragFloat("移動速度",
                    &m_sceneCameraSpeed,
                    0.1f,
                    0.1f,
                    100.0f,
                    "%.1f"))
            {
                m_sceneCameraSpeed =
                    std::clamp(m_sceneCameraSpeed, 0.1f, 100.0f);
            }

            ImGui::SetNextItemWidth(130.0f);
            if (ImGui::DragFloat("Shift倍率",
                    &m_sceneCameraBoostMultiplier,
                    0.1f,
                    1.0f,
                    10.0f,
                    "%.1fx"))
            {
                m_sceneCameraBoostMultiplier =
                    std::clamp(m_sceneCameraBoostMultiplier, 1.0f, 10.0f);
            }

            ImGui::SetNextItemWidth(130.0f);
            if (ImGui::SliderFloat("回転感度",
                    &m_sceneCameraLookSensitivity,
                    0.1f,
                    3.0f,
                    "%.2fx"))
            {
                m_sceneCameraLookSensitivity =
                    std::clamp(m_sceneCameraLookSensitivity, 0.1f, 3.0f);
            }

            ImGui::SetNextItemWidth(130.0f);
            if (ImGui::SliderFloat("ズーム感度",
                    &m_sceneCameraZoomSensitivity,
                    0.1f,
                    3.0f,
                    "%.2fx"))
            {
                m_sceneCameraZoomSensitivity =
                    std::clamp(m_sceneCameraZoomSensitivity, 0.1f, 3.0f);
            }

            ImGui::TextDisabled(
                "正投影では表示サイズに応じて移動速度を補正します。");
            if (ImGui::Button("カメラ設定を既定値に戻す"))
            {
                m_sceneCameraSpeed = 5.0f;
                m_sceneCameraBoostMultiplier = 3.0f;
                m_sceneCameraLookSensitivity = 1.0f;
                m_sceneCameraZoomSensitivity = 1.0f;
                SetStatus("シーンカメラ設定を既定値に戻しました");
            }
        }

        if (ImGui::CollapsingHeader("保存とリセット"))
        {
            ImGui::TextWrapped(
                "保存先: %s",
                PathToUtf8(EditorSettingsPath()).c_str()
            );
            ImGui::TextDisabled("終了時にも自動保存されます。");
            if (ImGui::Button("設定を保存"))
            {
                try
                {
                    SaveEditorSettings();
                    SetStatus("エディター設定を保存しました");
                }
                catch (const std::exception& exception)
                {
                    SetStatus(exception.what(), true);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("全設定を既定値に戻す"))
            {
                try
                {
                    ResetEditorSettings();
                    SaveEditorSettings();
                    SetStatus("エディター設定を既定値に戻しました");
                }
                catch (const std::exception& exception)
                {
                    SetStatus(exception.what(), true);
                }
            }
        }

        ImGui::EndPopup();
    }

    void EditorLayer::DrawGameViewport()
    {
        ImVec2 size = ImGui::GetContentRegionAvail();
        size.x = std::max(size.x, 16.0f);
        size.y = std::max(size.y, 16.0f);
        const ImVec2 position = ImGui::GetCursorScreenPos();

        // 描画スケールはアスペクトを変えずに画素数だけ減らします。
        // UIのレイアウトも縮んだ解像度で解決されるので、割合
        // ベースのUIは同じ見た目、ピクセル指定のUIは原寸確認の
        // ときだけ100%へ戻してください。
        const float resolutionScale = std::clamp(
            m_gameViewResolutionScale, 0.25f, 1.0f);

        std::uint32_t targetWidth{};
        std::uint32_t targetHeight{};
        if (m_gameViewFixedResolution)
        {
            targetWidth = static_cast<std::uint32_t>(
                std::max(
                    static_cast<int>(std::lround(
                        m_gameViewResolutionWidth
                        * resolutionScale)),
                    16));
            targetHeight = static_cast<std::uint32_t>(
                std::max(
                    static_cast<int>(std::lround(
                        m_gameViewResolutionHeight
                        * resolutionScale)),
                    16));
        }
        else
        {
            // 自由（パネル追従）でも描画スケールを効かせます。
            // こちらが既定なので、ここで効かないと「重いときに
            // 軽くする手段が無い」状態になります。パネルの大きさは
            // 変わらないので、絵はそのまま拡大表示されます。
            const float panelScale =
                m_graphics.Settings().renderScale
                * resolutionScale;
            targetWidth = std::max(
                static_cast<std::uint32_t>(
                    std::lround(size.x * panelScale)),
                1u);
            targetHeight = std::max(
                static_cast<std::uint32_t>(
                    std::lround(size.y * panelScale)),
                1u);
        }

        m_gameRenderTarget.Resize(
            m_graphics.Device(),
            targetWidth,
            targetHeight);
        m_graphics.SetUIViewportSize(
            m_gameRenderTarget.Width(),
            m_gameRenderTarget.Height());

        // When a fixed resolution is chosen, letterbox/pillarbox
        // the render inside the available panel space instead of
        // stretching it, so the aspect ratio matches the target
        // resolution exactly so the result matches the game view.
        ImVec2 displaySize = size;
        ImVec2 displayOffset{};
        if (m_gameViewFixedResolution)
        {
            const float targetAspect =
                static_cast<float>(targetWidth)
                / static_cast<float>(targetHeight);
            const float availableAspect = size.x / size.y;
            if (availableAspect > targetAspect)
            {
                displaySize.y = size.y;
                displaySize.x = size.y * targetAspect;
            }
            else
            {
                displaySize.x = size.x;
                displaySize.y = size.x / targetAspect;
            }
            displayOffset.x = (size.x - displaySize.x) * 0.5f;
            displayOffset.y = (size.y - displaySize.y) * 0.5f;

            ImGui::GetWindowDrawList()->AddRectFilled(
                position,
                ImVec2{
                    position.x + size.x,
                    position.y + size.y
                },
                IM_COL32(0, 0, 0, 255));
        }

        ImGui::SetCursorScreenPos(
            ImVec2{
                position.x + displayOffset.x,
                position.y + displayOffset.y
            });
        ImGui::Image(
            MakeTextureReference(
                m_gameRenderTarget.DisplayShaderResourceView()),
            displaySize);
        const ImVec2 imageMinimum =
            ImGui::GetItemRectMin();
        const ImVec2 mouse =
            ImGui::GetMousePos();
        const bool valid =
            ImGui::IsItemHovered();
        {
            const float scaleX =
                static_cast<float>(
                    m_gameRenderTarget.Width())
                / displaySize.x;
            const float scaleY =
                static_cast<float>(
                    m_gameRenderTarget.Height())
                / displaySize.y;
            const auto& inputOutput = ImGui::GetIO();
            InputPointerState pointer;
            pointer.position = {
                (mouse.x - imageMinimum.x) * scaleX,
                (mouse.y - imageMinimum.y) * scaleY
            };
            pointer.valid = valid;
            pointer.delta = {
                inputOutput.MouseDelta.x * scaleX,
                inputOutput.MouseDelta.y * scaleY
            };
            if (valid)
            {
                pointer.wheel = inputOutput.MouseWheel;
                pointer.wheelHorizontal =
                    inputOutput.MouseWheelH;
            }
            // ImGuiのボタン0～4はPointerButtonの並びと一致します。
            for (std::size_t button = 0;
                button < pointer.buttons.size();
                ++button)
            {
                pointer.buttons[button].down =
                    valid
                    && ImGui::IsMouseDown(
                        static_cast<int>(button));
            }
            pointer.down =
                pointer
                    .Button(PointerButton::Left)
                    .down;
            m_graphics.Input().SetPointerOverride(pointer);
        }

        ImGui::SetCursorScreenPos(
            ImVec2{ position.x + 8.0f, position.y + 8.0f });
        if (ImGui::SmallButton("解像度"))
        {
            ImGui::OpenPopup("GameViewResolution");
        }
        DrawGameViewResolutionSettings();
    }

    void EditorLayer::DrawGameViewResolutionSettings()
    {
        if (!ImGui::BeginPopup("GameViewResolution"))
        {
            return;
        }

        ImGui::TextUnformatted("ゲームビューの解像度");
        ImGui::Separator();

        if (ImGui::RadioButton(
                "自由（パネルサイズに合わせる）",
                !m_gameViewFixedResolution))
        {
            m_gameViewFixedResolution = false;
        }
        if (ImGui::RadioButton(
                "固定解像度",
                m_gameViewFixedResolution))
        {
            m_gameViewFixedResolution = true;
        }

        ImGui::BeginDisabled(!m_gameViewFixedResolution);
        ImGui::SetNextItemWidth(90.0f);
        ImGui::InputInt(
            "##GameViewWidth",
            &m_gameViewResolutionWidth);
        ImGui::SameLine();
        ImGui::TextUnformatted("x");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::InputInt(
            "##GameViewHeight",
            &m_gameViewResolutionHeight);
        m_gameViewResolutionWidth = std::clamp(
            m_gameViewResolutionWidth,
            16,
            8192);
        m_gameViewResolutionHeight = std::clamp(
            m_gameViewResolutionHeight,
            16,
            8192);

        ImGui::Separator();
        ImGui::TextDisabled("プリセット");
        const auto presetButton = [this](
            const char* label,
            const int width,
            const int height)
        {
            if (ImGui::Button(label))
            {
                m_gameViewResolutionWidth = width;
                m_gameViewResolutionHeight = height;
            }
        };
        presetButton("1920 x 1080 (16:9)", 1920, 1080);
        ImGui::SameLine();
        presetButton("1280 x 720 (16:9)", 1280, 720);
        presetButton("1080 x 1920 (縦 9:16)", 1080, 1920);
        ImGui::SameLine();
        presetButton("750 x 1334 (縦 iPhone)", 750, 1334);
        if (ImGui::Button("プロジェクト設定のウィンドウサイズに合わせる"))
        {
            m_gameViewResolutionWidth =
                static_cast<int>(m_projectSettings.windowWidth);
            m_gameViewResolutionHeight =
                static_cast<int>(m_projectSettings.windowHeight);
        }
        ImGui::EndDisabled();

        // 描画スケールは固定解像度の外に置きます。自由（パネル
        // 追従）が既定なので、そちらで触れないと「重いときに
        // 軽くする手段が無い」状態になります。
        ImGui::Separator();
        ImGui::TextDisabled("描画スケール");
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        // ツールチップは必ず出る項目へ付けます。条件付きの行の
        // 後ろに置くと、その行が出ないフレームでは係り先が別物へ
        // ずれ、既定の100%では一度も表示されませんでした。
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "塗る画素だけを減らします（アスペクトと割合ベースの"
                "UIは同じ見た目のまま軽くなります）。FPSはおおむね"
                "画素数なりなので、50%%で約4分の1の負荷です。"
                "ピクセル指定UIの原寸確認だけ100%%で行ってください");
        }
        const auto scaleButton = [this](
            const char* label,
            const float value)
        {
            if (ImGui::RadioButton(
                    label,
                    std::abs(
                        m_gameViewResolutionScale - value)
                        < 0.01f))
            {
                m_gameViewResolutionScale = value;
            }
            ImGui::SameLine();
        };
        scaleButton("100%", 1.0f);
        scaleButton("75%", 0.75f);
        scaleButton("50%", 0.5f);
        ImGui::NewLine();

        // 実際の描画解像度は常に出します。自由だと設定値のどこにも
        // 数字が出ないため、FPSが落ちた原因が解像度なのか中身なのか
        // を切り分けられませんでした。
        ImGui::TextDisabled(
            "実際の描画: %u x %u",
            m_gameRenderTarget.Width(),
            m_gameRenderTarget.Height());

        ImGui::EndPopup();
    }

    void EditorLayer::DrawTransformGizmo()
    {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            m_transformGizmoMouseCaptured = false;
        }
        m_transformGizmoHovered = false;
        m_transformGizmoUsing = false;

        auto* selected = m_scene.FindGameObject(m_selectedObjectId);
        if (selected == nullptr || m_playing)
        {
            const bool cancelGizmo =
                m_gizmoWasUsing
                || m_transformGizmoUsing
                || m_transformGizmoMouseCaptured;
            if (m_gizmoWasUsing)
            {
                RecordHistory();
                m_gizmoWasUsing = false;
            }
            if (cancelGizmo)
            {
                ImGuizmo::Enable(false);
                ImGuizmo::Enable(true);
                m_transformGizmoMouseCaptured = false;
            }
            m_gizmoObjectId = {};
            return;
        }

        if (m_gizmoWasUsing
            && m_gizmoObjectId != selected->Id())
        {
            RecordHistory();
            ImGuizmo::Enable(false);
            ImGuizmo::Enable(true);
            m_gizmoWasUsing = false;
            m_transformGizmoMouseCaptured = false;
        }
        m_gizmoObjectId = selected->Id();

        ImGuizmo::SetOrthographic(m_sceneOrthographic);
        ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
        ImGuizmo::SetRect(
            m_viewportPosition.x,
            m_viewportPosition.y,
            m_viewportSize.x,
            m_viewportSize.y);
        ImGuizmo::SetGizmoSizeClipSpace(0.13f);

        auto& gizmoStyle = ImGuizmo::GetStyle();
        gizmoStyle.TranslationLineThickness = 4.0f;
        gizmoStyle.TranslationLineArrowSize = 8.0f;
        gizmoStyle.RotationLineThickness = 3.0f;
        gizmoStyle.ScaleLineThickness = 4.0f;
        gizmoStyle.ScaleLineCircleSize = 7.0f;
        gizmoStyle.CenterCircleSize = 7.0f;
        gizmoStyle.Colors[ImGuizmo::DIRECTION_X] =
            ImVec4{ 0.95f, 0.18f, 0.18f, 1.0f };
        gizmoStyle.Colors[ImGuizmo::DIRECTION_Y] =
            ImVec4{ 0.25f, 0.85f, 0.30f, 1.0f };
        gizmoStyle.Colors[ImGuizmo::DIRECTION_Z] =
            ImVec4{ 0.18f, 0.48f, 1.0f, 1.0f };

        DirectX::XMFLOAT4X4 viewMatrix{};
        DirectX::XMFLOAT4X4 projectionMatrix{};
        DirectX::XMFLOAT4X4 worldMatrix{};
        DirectX::XMStoreFloat4x4(
            &viewMatrix,
            SceneViewMatrix());
        DirectX::XMStoreFloat4x4(
            &projectionMatrix,
            SceneProjectionMatrix());
        DirectX::XMStoreFloat4x4(
            &worldMatrix,
            selected->WorldMatrix());

        ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
        switch (m_gizmoOperation)
        {
        case GizmoOperation::Translate:
            operation = ImGuizmo::TRANSLATE;
            break;
        case GizmoOperation::Rotate:
            operation = ImGuizmo::ROTATE;
            break;
        case GizmoOperation::Scale:
            operation = ImGuizmo::SCALE;
            break;
        }

        const ImGuizmo::MODE mode = m_gizmoLocal
            ? ImGuizmo::LOCAL
            : ImGuizmo::WORLD;

        std::array snapValues{
            m_translationSnap,
            m_translationSnap,
            m_translationSnap
        };
        switch (m_gizmoOperation)
        {
        case GizmoOperation::Translate:
            break;
        case GizmoOperation::Rotate:
            snapValues.fill(m_rotationSnap);
            break;
        case GizmoOperation::Scale:
            snapValues.fill(m_scaleSnap);
            break;
        }
        const bool useSnap = m_snapEnabled || ImGui::GetIO().KeyCtrl;

        ImGuizmo::PushID(selected);
        const bool changed = ImGuizmo::Manipulate(
            &viewMatrix._11,
            &projectionMatrix._11,
            operation,
            mode,
            &worldMatrix._11,
            nullptr,
            useSnap ? snapValues.data() : nullptr);
        const bool isUsing = ImGuizmo::IsUsing();
        const bool isHovered = ImGuizmo::IsOver(operation);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && (isHovered || isUsing))
        {
            m_transformGizmoMouseCaptured = true;
        }
        m_transformGizmoHovered = isHovered;
        m_transformGizmoUsing = isUsing;
        ImGuizmo::PopID();

        if (changed)
        {
            DirectX::XMMATRIX newWorld =
                DirectX::XMLoadFloat4x4(&worldMatrix);
            DirectX::XMMATRIX newLocal = newWorld;

            if (selected->Parent() != nullptr)
            {
                DirectX::XMVECTOR determinant{};
                const DirectX::XMMATRIX inverseParent =
                    DirectX::XMMatrixInverse(
                        &determinant,
                        selected->Parent()->WorldMatrix());
                if (std::abs(
                        DirectX::XMVectorGetX(determinant))
                    <= 1.0e-8f)
                {
                    return;
                }
                newLocal = newWorld * inverseParent;
            }

            auto& transform = selected->GetTransform();
            DirectX::XMVECTOR localScale{};
            DirectX::XMVECTOR localRotation{};
            DirectX::XMVECTOR localTranslation{};
            if (!DirectX::XMMatrixDecompose(
                    &localScale,
                    &localRotation,
                    &localTranslation,
                    newLocal))
            {
                return;
            }

            DirectX::XMFLOAT3 translation{};
            DirectX::XMStoreFloat3(
                &translation,
                localTranslation);
            DirectX::XMFLOAT3 scale{};
            DirectX::XMStoreFloat3(
                &scale,
                localScale);

            // 複数選択中は、主選択の変化量を他へも同じだけ適用します。
            const DirectX::XMFLOAT3 deltaPosition{
                translation.x - transform.position.x,
                translation.y - transform.position.y,
                translation.z - transform.position.z
            };
            // 回転差分はEuler角で求めると±180度やジンバルロックの
            // 近くで不連続になるため、クォータニオンの差分にします。
            const auto deltaRotation =
                DirectX::XMQuaternionMultiply(
                    DirectX::XMQuaternionInverse(
                        transform.RotationVector()),
                    localRotation);
            const DirectX::XMFLOAT3 scaleRatio{
                transform.scale.x != 0.0f
                    ? scale.x / transform.scale.x
                    : 1.0f,
                transform.scale.y != 0.0f
                    ? scale.y / transform.scale.y
                    : 1.0f,
                transform.scale.z != 0.0f
                    ? scale.z / transform.scale.z
                    : 1.0f
            };

            transform.position = {
                translation.x,
                translation.y,
                translation.z
            };
            transform.SetRotationVector(localRotation);
            transform.scale = {
                scale.x,
                scale.y,
                scale.z
            };

            for (auto* other : SelectedObjects())
            {
                if (other == selected)
                {
                    continue;
                }
                // 主選択の子は親の移動で一緒に動くため除きます。
                bool underPrimary = false;
                for (const auto* parent = other->Parent();
                    parent != nullptr;
                    parent = parent->Parent())
                {
                    if (parent == selected)
                    {
                        underPrimary = true;
                        break;
                    }
                }
                if (underPrimary)
                {
                    continue;
                }

                auto& otherTransform =
                    other->GetTransform();
                otherTransform.position.x +=
                    deltaPosition.x;
                otherTransform.position.y +=
                    deltaPosition.y;
                otherTransform.position.z +=
                    deltaPosition.z;
                otherTransform.SetRotationVector(
                    DirectX::XMQuaternionMultiply(
                        otherTransform.RotationVector(),
                        deltaRotation));
                otherTransform.scale.x *= scaleRatio.x;
                otherTransform.scale.y *= scaleRatio.y;
                otherTransform.scale.z *= scaleRatio.z;
            }
        }

        if (m_gizmoWasUsing && !isUsing)
        {
            RecordHistory();
        }
        m_gizmoWasUsing = isUsing;
    }

    void EditorLayer::DrawViewCube()
    {
        using namespace DirectX;

        constexpr float cubeSize = 108.0f;
        const ImVec2 cubePosition{
            m_viewportPosition.x + m_viewportSize.x - cubeSize - 8.0f,
            m_viewportPosition.y + 8.0f
        };
        const ImVec2 mouse = ImGui::GetMousePos();
        m_viewCubeHovered =
            mouse.x >= cubePosition.x
            && mouse.y >= cubePosition.y
            && mouse.x <= cubePosition.x + cubeSize
            && mouse.y <= cubePosition.y + cubeSize;

        XMFLOAT4X4 viewMatrix{};
        XMStoreFloat4x4(
            &viewMatrix,
            SceneViewMatrix());
        const XMFLOAT4X4 previousView = viewMatrix;

        ImGuizmo::SetOrthographic(m_sceneOrthographic);
        ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
        ImGuizmo::ViewManipulate(
            &viewMatrix._11,
            m_sceneCameraFocusDistance,
            cubePosition,
            ImVec2{ cubeSize, cubeSize },
            IM_COL32(20, 24, 32, 190));

        const float* previous = &previousView._11;
        const float* current = &viewMatrix._11;
        bool changed = false;
        for (std::size_t index = 0; index < 16; ++index)
        {
            if (std::abs(previous[index] - current[index]) > 0.00001f)
            {
                changed = true;
                break;
            }
        }

        if (changed)
        {
            if (m_scene2DMode)
            {
                m_scene2DMode = false;
                m_sceneOrthographic = false;
                m_scene3DViewStored = false;
            }
            XMVECTOR determinant{};
            const XMMATRIX view =
                XMLoadFloat4x4(&viewMatrix);
            const XMMATRIX cameraWorld =
                XMMatrixInverse(&determinant, view);
            const XMVECTOR forward = XMVector3Normalize(
                XMVectorNegate(cameraWorld.r[2]));

            XMStoreFloat3(
                &m_sceneCameraPosition,
                cameraWorld.r[3]);
            const float forwardX = XMVectorGetX(forward);
            const float forwardY = std::clamp(
                XMVectorGetY(forward),
                -1.0f,
                1.0f);
            const float forwardZ = XMVectorGetZ(forward);
            m_sceneCameraRotation.x = std::asin(forwardY);
            m_sceneCameraRotation.y =
                std::atan2(-forwardX, -forwardZ);
            m_sceneCameraRotation.z = 0.0f;
        }

        if (m_viewCubeHovered)
        {
            ImGui::SetTooltip(
                "ビューキューブ\n"
                "面をクリック: 正面・側面・上面へ整列\n"
                "ドラッグ: 視点を回転");
        }
    }

    void EditorLayer::FocusSelection()
    {
        using namespace DirectX;

        const auto* selected = m_scene.FindGameObject(m_selectedObjectId);
        if (selected == nullptr)
        {
            return;
        }

        const auto bounds = SelectionBounds(*selected);
        if (!bounds)
        {
            return;
        }

        const XMFLOAT3 center{
            (bounds->minimum.x + bounds->maximum.x) * 0.5f,
            (bounds->minimum.y + bounds->maximum.y) * 0.5f,
            (bounds->minimum.z + bounds->maximum.z) * 0.5f
        };
        const float halfExtent = std::max({
            (bounds->maximum.x - bounds->minimum.x) * 0.5f,
            (bounds->maximum.y - bounds->minimum.y) * 0.5f,
            (bounds->maximum.z - bounds->minimum.z) * 0.5f
        });
        m_sceneCameraFocusDistance = std::max(halfExtent * 3.0f, 2.0f);
        if (m_sceneOrthographic)
        {
            m_sceneOrthographicSize = std::max(
                halfExtent * 3.0f,
                1.0f);
        }

        const XMMATRIX rotation = XMMatrixRotationRollPitchYaw(
            m_sceneCameraRotation.x,
            m_sceneCameraRotation.y,
            0.0f);
        const XMVECTOR forward = XMVector3Normalize(
            XMVector3TransformNormal(
                g_XMNegIdentityR2,
                rotation));
        const XMVECTOR position = XMVectorSubtract(
            XMLoadFloat3(&center),
            XMVectorScale(forward, m_sceneCameraFocusDistance));
        XMStoreFloat3(&m_sceneCameraPosition, position);
    }

    void EditorLayer::SetScene2DMode(const bool enabled)
    {
        using namespace DirectX;

        if (enabled == m_scene2DMode)
        {
            return;
        }

        if (enabled)
        {
            m_scene3DCameraPosition = m_sceneCameraPosition;
            m_scene3DCameraRotation = m_sceneCameraRotation;
            m_scene3DCameraFocusDistance =
                m_sceneCameraFocusDistance;
            m_scene3DViewStored = true;

            const XMMATRIX rotation = XMMatrixRotationRollPitchYaw(
                m_sceneCameraRotation.x,
                m_sceneCameraRotation.y,
                0.0f);
            const XMVECTOR forward = XMVector3Normalize(
                XMVector3TransformNormal(
                    g_XMNegIdentityR2,
                    rotation));
            const XMVECTOR focus = XMVectorMultiplyAdd(
                XMVectorReplicate(m_sceneCameraFocusDistance),
                forward,
                XMLoadFloat3(&m_sceneCameraPosition));
            XMFLOAT3 focusPosition{};
            XMStoreFloat3(&focusPosition, focus);

            m_sceneCameraRotation = { 0.0f, 0.0f, 0.0f };
            m_sceneCameraPosition = {
                focusPosition.x,
                focusPosition.y,
                focusPosition.z + m_sceneCameraFocusDistance
            };
            m_sceneOrthographic = true;
            m_scene2DMode = true;
            return;
        }

        m_scene2DMode = false;
        m_sceneOrthographic = false;
        if (m_scene3DViewStored)
        {
            m_sceneCameraPosition = m_scene3DCameraPosition;
            m_sceneCameraRotation = m_scene3DCameraRotation;
            m_sceneCameraFocusDistance =
                m_scene3DCameraFocusDistance;
            m_scene3DViewStored = false;
            return;
        }

        const XMVECTOR focus = XMVectorAdd(
            XMLoadFloat3(&m_sceneCameraPosition),
            XMVectorScale(
                g_XMNegIdentityR2,
                m_sceneCameraFocusDistance));
        m_sceneCameraRotation = { -0.12f, 0.0f, 0.0f };
        const XMMATRIX rotation = XMMatrixRotationRollPitchYaw(
            m_sceneCameraRotation.x,
            m_sceneCameraRotation.y,
            0.0f);
        const XMVECTOR forward = XMVector3Normalize(
            XMVector3TransformNormal(
                g_XMNegIdentityR2,
                rotation));
        XMStoreFloat3(
            &m_sceneCameraPosition,
            XMVectorMultiplyAdd(
                XMVectorReplicate(-m_sceneCameraFocusDistance),
                forward,
                focus));
    }

    DirectX::XMFLOAT3 EditorLayer::SceneViewDropPosition() const
    {
        using namespace DirectX;

        if (m_viewportSize.x <= 0.0f
            || m_viewportSize.y <= 0.0f)
        {
            return {};
        }

        const ImVec2 mousePosition = ImGui::GetMousePos();
        const float localX =
            mousePosition.x - m_viewportPosition.x;
        const float localY =
            mousePosition.y - m_viewportPosition.y;

        const XMMATRIX view = SceneViewMatrix();
        const XMMATRIX projection = SceneProjectionMatrix();
        const XMMATRIX world = XMMatrixIdentity();
        const XMVECTOR nearPoint = XMVector3Unproject(
            XMVectorSet(localX, localY, 0.0f, 1.0f),
            0.0f,
            0.0f,
            m_viewportSize.x,
            m_viewportSize.y,
            0.0f,
            1.0f,
            projection,
            view,
            world);
        const XMVECTOR farPoint = XMVector3Unproject(
            XMVectorSet(localX, localY, 1.0f, 1.0f),
            0.0f,
            0.0f,
            m_viewportSize.x,
            m_viewportSize.y,
            0.0f,
            1.0f,
            projection,
            view,
            world);

        Ray ray{};
        XMStoreFloat3(&ray.origin, nearPoint);
        XMStoreFloat3(
            &ray.direction,
            XMVector3Normalize(
                XMVectorSubtract(farPoint, nearPoint)));

        // 既存オブジェクトの上へ落とした場合は、その手前へ置きます。
        float nearestDistance =
            std::numeric_limits<float>::max();
        for (const auto& gameObject : m_scene.GameObjects())
        {
            if (!gameObject->IsEnabled())
            {
                continue;
            }
            const auto bounds = SelectionBounds(*gameObject);
            if (!bounds)
            {
                continue;
            }
            float distance{};
            if (RayIntersectsBounds(ray, *bounds, distance)
                && distance > 0.0f
                && distance < nearestDistance)
            {
                nearestDistance = distance;
            }
        }

        if (nearestDistance
            == std::numeric_limits<float>::max())
        {
            // 何にも当たらなければグリッド平面との交点へ。
            // 2DはXY平面(z=0)、3DはXZ平面(y=0)です。
            const float origin = m_scene2DMode
                ? ray.origin.z
                : ray.origin.y;
            const float direction = m_scene2DMode
                ? ray.direction.z
                : ray.direction.y;
            if (std::abs(direction) > 1.0e-4f)
            {
                const float hit = -origin / direction;
                if (hit > 0.0f)
                {
                    nearestDistance = hit;
                }
            }
            if (nearestDistance
                == std::numeric_limits<float>::max())
            {
                // 平面と平行な視点では、カメラ前方の一定距離へ。
                nearestDistance = std::max(
                    m_sceneCameraFocusDistance,
                    1.0f);
            }
        }

        XMFLOAT3 position{};
        XMStoreFloat3(
            &position,
            XMVectorMultiplyAdd(
                XMVectorReplicate(nearestDistance),
                XMLoadFloat3(&ray.direction),
                XMLoadFloat3(&ray.origin)));
        return position;
    }

    void EditorLayer::HandleSceneViewAssetDrop()
    {
        if (m_playing || !ImGui::BeginDragDropTarget())
        {
            return;
        }

        if (const ImGuiPayload* payload =
            ImGui::AcceptDragDropPayload(AssetPayload))
        {
            const auto asset = PathFromUtf8(
                static_cast<const char*>(payload->Data));
            const auto position = SceneViewDropPosition();
            try
            {
                if (IsPrefabAsset(asset))
                {
                    auto& instance = m_scene.InstantiatePrefab(
                        m_graphics.Assets().ResolvePath(asset));
                    instance.GetTransform().position = position;
                    m_selectedObjectId = instance.Id();
                    RecordHistory();
                    SetStatus(
                        "Prefabを配置しました: "
                        + PathToUtf8(asset));
                }
                else if (IsCppScriptAsset(asset)
                    || IsSceneAsset(asset))
                {
                    // シーンやスクリプトは置き場所の概念が無いため
                    // Scene Viewへのドロップでは何もしません。
                }
                else
                {
                    auto& created = m_scene.CreateGameObject(
                        PathToUtf8(asset.stem()));
                    created.GetTransform().position = position;
                    if (ApplyDroppedAsset(created, asset))
                    {
                        m_selectedObjectId = created.Id();
                    }
                    else
                    {
                        static_cast<void>(
                            m_scene.DestroyGameObject(
                                created));
                    }
                }
            }
            catch (const std::exception& exception)
            {
                SetStatus(exception.what(), true);
            }
        }
        ImGui::EndDragDropTarget();
    }

    void EditorLayer::PickSceneObject()
    {
        using namespace DirectX;

        if (m_viewportSize.x <= 0.0f || m_viewportSize.y <= 0.0f)
        {
            return;
        }

        const ImVec2 mousePosition = ImGui::GetMousePos();
        const float localX = mousePosition.x - m_viewportPosition.x;
        const float localY = mousePosition.y - m_viewportPosition.y;
        if (localX < 0.0f
            || localY < 0.0f
            || localX > m_viewportSize.x
            || localY > m_viewportSize.y)
        {
            return;
        }

        const XMMATRIX view = SceneViewMatrix();
        const XMMATRIX projection = SceneProjectionMatrix();
        const XMMATRIX world = XMMatrixIdentity();
        const XMVECTOR nearPoint = XMVector3Unproject(
            XMVectorSet(localX, localY, 0.0f, 1.0f),
            0.0f,
            0.0f,
            m_viewportSize.x,
            m_viewportSize.y,
            0.0f,
            1.0f,
            projection,
            view,
            world);
        const XMVECTOR farPoint = XMVector3Unproject(
            XMVectorSet(localX, localY, 1.0f, 1.0f),
            0.0f,
            0.0f,
            m_viewportSize.x,
            m_viewportSize.y,
            0.0f,
            1.0f,
            projection,
            view,
            world);

        Ray ray{};
        XMStoreFloat3(&ray.origin, nearPoint);
        XMStoreFloat3(
            &ray.direction,
            XMVector3Normalize(XMVectorSubtract(farPoint, nearPoint)));

        GameObjectId nearestId{};
        float nearestDistance = std::numeric_limits<float>::max();
        for (const auto& gameObject : m_scene.GameObjects())
        {
            if (!gameObject->IsEnabled())
            {
                continue;
            }

            const auto bounds = SelectionBounds(*gameObject);
            if (!bounds)
            {
                continue;
            }

            float distance{};
            if (RayIntersectsBounds(ray, *bounds, distance)
                && distance < nearestDistance)
            {
                nearestDistance = distance;
                nearestId = gameObject->Id();
            }
        }

        // Ctrl+クリックはScene Viewでも選択の追加／解除にします。
        SelectObject(nearestId, ImGui::GetIO().KeyCtrl);
    }

    void EditorLayer::DrawSelectionHighlight()
    {
        // 主選択は明るい水色、追加選択は少し暗い色で描きます。
        const auto selection = SelectedObjects();
        for (const auto* object : selection)
        {
            if (object == nullptr || !object->IsEnabled())
            {
                continue;
            }
            const auto bounds = SelectionBounds(*object);
            if (!bounds)
            {
                continue;
            }
            const bool primary =
                object->Id() == m_selectedObjectId;
            m_graphics.Debug().DrawBounds(
                *bounds,
                primary
                    ? DirectX::XMVectorSet(
                        0.10f, 0.85f, 1.0f, 1.0f)
                    : DirectX::XMVectorSet(
                        0.10f, 0.55f, 0.75f, 1.0f),
                SceneViewMatrix(),
                SceneProjectionMatrix());
        }
    }

    void EditorLayer::DrawCameraGizmos()
    {
        const float aspectRatio = m_gameRenderTarget.IsValid()
            ? m_gameRenderTarget.AspectRatio()
            : 16.0f / 9.0f;

        for (const auto& gameObject : m_scene.GameObjects())
        {
            const auto* camera =
                gameObject->GetComponent<CameraComponent>();
            if (!gameObject->IsEnabled()
                || camera == nullptr
                || !camera->IsEnabled())
            {
                continue;
            }

            const bool isMainCamera = camera == m_scene.MainCamera();
            const auto color = isMainCamera
                ? DirectX::XMVectorSet(1.0f, 0.78f, 0.16f, 1.0f)
                : DirectX::XMVectorSet(0.72f, 0.36f, 1.0f, 1.0f);
            const float debugFarDistance = std::min(
                camera->FarPlane(),
                5.0f);

            m_graphics.Debug().DrawFrustum(
                gameObject->WorldMatrix(),
                camera->VerticalFieldOfView(),
                aspectRatio,
                camera->NearPlane(),
                debugFarDistance,
                color,
                SceneViewMatrix(),
                SceneProjectionMatrix());
        }
    }

    void EditorLayer::DrawLightGizmos()
    {
        for (const auto& gameObject : m_scene.GameObjects())
        {
            if (!gameObject->IsEnabled())
            {
                continue;
            }

            if (const auto* light =
                gameObject->GetComponent<
                    DirectionalLightComponent>();
                light != nullptr && light->IsEnabled())
            {
                const auto& color = light->Color();
                m_graphics.Debug().DrawDirectionalLight(
                    gameObject->WorldMatrix(),
                    DirectX::XMVectorSet(
                        color.x,
                        color.y,
                        color.z,
                        1.0f),
                    SceneViewMatrix(),
                    SceneProjectionMatrix());
            }
            if (const auto* light =
                gameObject->GetComponent<PointLightComponent>();
                light != nullptr && light->IsEnabled())
            {
                const auto& color = light->Color();
                m_graphics.Debug().DrawPointLight(
                    gameObject->WorldMatrix(),
                    light->Range(),
                    DirectX::XMVectorSet(
                        color.x,
                        color.y,
                        color.z,
                        1.0f),
                    SceneViewMatrix(),
                    SceneProjectionMatrix());
            }
            if (const auto* light =
                gameObject->GetComponent<SpotLightComponent>();
                light != nullptr && light->IsEnabled())
            {
                const auto& color = light->Color();
                m_graphics.Debug().DrawSpotLight(
                    gameObject->WorldMatrix(),
                    light->Range(),
                    light->OuterConeAngle(),
                    DirectX::XMVectorSet(
                        color.x,
                        color.y,
                        color.z,
                        1.0f),
                    SceneViewMatrix(),
                    SceneProjectionMatrix());
            }
        }
    }

    void EditorLayer::UpdateSceneCamera()
    {
        if (!m_sceneViewportHovered
            || m_transformGizmoHovered
            || m_transformGizmoUsing
            || m_transformGizmoMouseCaptured
            || ImGuizmo::IsUsingViewManipulate())
        {
            return;
        }

        const auto& inputOutput = ImGui::GetIO();
        const auto& viewportSettings = m_projectSettings.viewport;
        const bool orbitNavigation =
            viewportSettings.navigationPreset
            == ViewportNavigationPreset::Orbit;
        const bool altDown =
            ImGui::IsKeyDown(ImGuiKey_LeftAlt)
            || ImGui::IsKeyDown(ImGuiKey_RightAlt);
        const bool leftMouseDown =
            ImGui::IsMouseDown(ImGuiMouseButton_Left);
        const bool middleMouseDown =
            ImGui::IsMouseDown(ImGuiMouseButton_Middle);
        const bool rightMouseDown =
            ImGui::IsMouseDown(ImGuiMouseButton_Right);
        const float orbitSensitivity =
            m_sceneCameraLookSensitivity
            * viewportSettings.orbitSensitivity;
        const float zoomSensitivity =
            m_sceneCameraZoomSensitivity
            * viewportSettings.zoomSensitivity;
        const DirectX::XMMATRIX rotation =
            DirectX::XMMatrixRotationRollPitchYaw(
                m_sceneCameraRotation.x,
                m_sceneCameraRotation.y,
                0.0f);
        const DirectX::XMVECTOR forward =
            DirectX::XMVector3Normalize(
                DirectX::XMVector3TransformNormal(
                    DirectX::g_XMNegIdentityR2,
                    rotation));
        const DirectX::XMVECTOR up = DirectX::g_XMIdentityR1;

        DirectX::XMVECTOR position =
            DirectX::XMLoadFloat3(&m_sceneCameraPosition);

        if (inputOutput.MouseWheel != 0.0f)
        {
            if (m_sceneOrthographic)
            {
                m_sceneOrthographicSize *= std::exp(
                    -0.16f
                    * zoomSensitivity
                    * inputOutput.MouseWheel);
                m_sceneOrthographicSize = std::clamp(
                    m_sceneOrthographicSize,
                    0.1f,
                    1000.0f);
            }
            else
            {
                const DirectX::XMVECTOR focus = DirectX::XMVectorMultiplyAdd(
                    DirectX::XMVectorReplicate(m_sceneCameraFocusDistance),
                    forward,
                    position);
                m_sceneCameraFocusDistance *= std::exp(
                    -0.16f
                    * zoomSensitivity
                    * inputOutput.MouseWheel);
                m_sceneCameraFocusDistance = std::clamp(
                    m_sceneCameraFocusDistance,
                    0.2f,
                    1000.0f);
                position = DirectX::XMVectorMultiplyAdd(
                    DirectX::XMVectorReplicate(
                        -m_sceneCameraFocusDistance),
                    forward,
                    focus);
            }
        }

        if (orbitNavigation
            && altDown
            && rightMouseDown
            && std::abs(inputOutput.MouseDelta.y) > 0.0f)
        {
            const DirectX::XMVECTOR focus =
                DirectX::XMVectorMultiplyAdd(
                    DirectX::XMVectorReplicate(
                        m_sceneCameraFocusDistance),
                    forward,
                    position);
            m_sceneCameraFocusDistance *= std::exp(
                0.01f
                * inputOutput.MouseDelta.y
                * viewportSettings.zoomSensitivity);
            m_sceneCameraFocusDistance = std::clamp(
                m_sceneCameraFocusDistance,
                0.2f,
                1000.0f);
            position = DirectX::XMVectorMultiplyAdd(
                DirectX::XMVectorReplicate(
                    -m_sceneCameraFocusDistance),
                forward,
                focus);
        }

        const bool orbit =
            orbitNavigation
            && altDown
            && leftMouseDown
            && !m_scene2DMode;
        const bool pan = orbitNavigation && middleMouseDown;
        if (orbit)
        {
            const float lookScale =
                0.004f * orbitSensitivity;
            const float invertY =
                viewportSettings.invertY ? -1.0f : 1.0f;
            m_sceneCameraRotation.y +=
                inputOutput.MouseDelta.x * lookScale;
            m_sceneCameraRotation.x +=
                inputOutput.MouseDelta.y * lookScale * invertY;
            m_sceneCameraRotation.x = std::clamp(
                m_sceneCameraRotation.x,
                -DirectX::XM_PIDIV2 + 0.01f,
                DirectX::XM_PIDIV2 - 0.01f);

            const DirectX::XMMATRIX orbitRotation =
                DirectX::XMMatrixRotationRollPitchYaw(
                    m_sceneCameraRotation.x,
                    m_sceneCameraRotation.y,
                    0.0f);
            const DirectX::XMVECTOR orbitForward =
                DirectX::XMVector3Normalize(
                    DirectX::XMVector3TransformNormal(
                        DirectX::g_XMNegIdentityR2,
                        orbitRotation));
            const DirectX::XMVECTOR focus =
                DirectX::XMVectorMultiplyAdd(
                    DirectX::XMVectorReplicate(
                        m_sceneCameraFocusDistance),
                    forward,
                    position);
            position = DirectX::XMVectorMultiplyAdd(
                DirectX::XMVectorReplicate(
                    -m_sceneCameraFocusDistance),
                orbitForward,
                focus);
        }
        else if (pan)
        {
            const DirectX::XMMATRIX panRotation =
                DirectX::XMMatrixRotationRollPitchYaw(
                    m_sceneCameraRotation.x,
                    m_sceneCameraRotation.y,
                    0.0f);
            const DirectX::XMVECTOR right =
                DirectX::XMVector3Normalize(
                    DirectX::XMVector3TransformNormal(
                        DirectX::g_XMIdentityR0,
                        panRotation));
            const DirectX::XMVECTOR panUp =
                DirectX::XMVector3Normalize(
                    DirectX::XMVector3TransformNormal(
                        DirectX::g_XMIdentityR1,
                        panRotation));
            const float distance = m_sceneOrthographic
                ? m_sceneOrthographicSize
                : m_sceneCameraFocusDistance;
            const float panScale =
                0.0025f
                * std::max(distance, 0.1f)
                * viewportSettings.panSensitivity;
            position = DirectX::XMVectorMultiplyAdd(
                DirectX::XMVectorReplicate(
                    -inputOutput.MouseDelta.x * panScale),
                right,
                position);
            position = DirectX::XMVectorMultiplyAdd(
                DirectX::XMVectorReplicate(
                    inputOutput.MouseDelta.y * panScale),
                panUp,
                position);
        }

        if (orbit || pan || (orbitNavigation && altDown && rightMouseDown))
        {
            DirectX::XMStoreFloat3(&m_sceneCameraPosition, position);
            return;
        }

        if (!rightMouseDown)
        {
            DirectX::XMStoreFloat3(&m_sceneCameraPosition, position);
            return;
        }

        if (!m_scene2DMode)
        {
            const float lookScale =
                0.004f * orbitSensitivity;
            const float invertY =
                viewportSettings.invertY ? -1.0f : 1.0f;
            m_sceneCameraRotation.y += inputOutput.MouseDelta.x * lookScale;
            m_sceneCameraRotation.x +=
                inputOutput.MouseDelta.y * lookScale * invertY;
            m_sceneCameraRotation.x = std::clamp(
                m_sceneCameraRotation.x,
                -DirectX::XM_PIDIV2 + 0.01f,
                DirectX::XM_PIDIV2 - 0.01f);
        }

        const DirectX::XMMATRIX updatedRotation =
            DirectX::XMMatrixRotationRollPitchYaw(
                m_sceneCameraRotation.x,
                m_sceneCameraRotation.y,
                0.0f);
        const DirectX::XMVECTOR updatedForward =
            DirectX::XMVector3Normalize(
                DirectX::XMVector3TransformNormal(
                    DirectX::g_XMNegIdentityR2,
                    updatedRotation));
        const DirectX::XMVECTOR updatedRight =
            DirectX::XMVector3Normalize(
                DirectX::XMVector3TransformNormal(
                    DirectX::g_XMIdentityR0,
                    updatedRotation));
        const DirectX::XMVECTOR updatedUp =
            DirectX::XMVector3Normalize(
                DirectX::XMVector3TransformNormal(
                    DirectX::g_XMIdentityR1,
                    updatedRotation));

        const float projectionScale = m_sceneOrthographic
            ? std::max(m_sceneOrthographicSize / 10.0f, 0.1f)
            : 1.0f;
        float speed =
            m_sceneCameraSpeed
            * projectionScale
            * inputOutput.DeltaTime;
        if (ImGui::IsKeyDown(ImGuiKey_LeftShift)
            || ImGui::IsKeyDown(ImGuiKey_RightShift))
        {
            speed *= m_sceneCameraBoostMultiplier;
        }

        if (ImGui::IsKeyDown(ImGuiKey_W))
        {
            position = DirectX::XMVectorMultiplyAdd(
                DirectX::XMVectorReplicate(speed),
                m_sceneOrthographic ? updatedUp : updatedForward,
                position);
        }
        if (ImGui::IsKeyDown(ImGuiKey_S))
        {
            position = DirectX::XMVectorMultiplyAdd(
                DirectX::XMVectorReplicate(-speed),
                m_sceneOrthographic ? updatedUp : updatedForward,
                position);
        }
        if (ImGui::IsKeyDown(ImGuiKey_D))
        {
            position = DirectX::XMVectorMultiplyAdd(
                DirectX::XMVectorReplicate(speed),
                updatedRight,
                position);
        }
        if (ImGui::IsKeyDown(ImGuiKey_A))
        {
            position = DirectX::XMVectorMultiplyAdd(
                DirectX::XMVectorReplicate(-speed),
                updatedRight,
                position);
        }
        if (ImGui::IsKeyDown(ImGuiKey_E))
        {
            position = DirectX::XMVectorMultiplyAdd(
                DirectX::XMVectorReplicate(speed),
                m_sceneOrthographic ? updatedForward : up,
                position);
        }
        if (ImGui::IsKeyDown(ImGuiKey_Q))
        {
            position = DirectX::XMVectorMultiplyAdd(
                DirectX::XMVectorReplicate(-speed),
                m_sceneOrthographic ? updatedForward : up,
                position);
        }

        DirectX::XMStoreFloat3(&m_sceneCameraPosition, position);
    }

    DirectX::XMMATRIX EditorLayer::SceneViewMatrix() const noexcept
    {
        const DirectX::XMMATRIX rotation =
            DirectX::XMMatrixRotationRollPitchYaw(
                m_sceneCameraRotation.x,
                m_sceneCameraRotation.y,
                0.0f);
        const DirectX::XMVECTOR position =
            DirectX::XMLoadFloat3(&m_sceneCameraPosition);
        const DirectX::XMVECTOR forward =
            DirectX::XMVector3TransformNormal(
                DirectX::g_XMNegIdentityR2,
                rotation);
        const DirectX::XMVECTOR up =
            DirectX::XMVector3TransformNormal(
                DirectX::g_XMIdentityR1,
                rotation);

        return DirectX::XMMatrixLookToRH(position, forward, up);
    }

    DirectX::XMMATRIX EditorLayer::SceneProjectionMatrix() const noexcept
    {
        if (m_sceneOrthographic)
        {
            const float height = std::max(
                m_sceneOrthographicSize,
                0.1f);
            return DirectX::XMMatrixOrthographicRH(
                height * m_sceneRenderTarget.AspectRatio(),
                height,
                0.1f,
                1000.0f);
        }

        return DirectX::XMMatrixPerspectiveFovRH(
            DirectX::XMConvertToRadians(60.0f),
            m_sceneRenderTarget.AspectRatio(),
            0.1f,
            1000.0f);
    }

    void EditorLayer::DrawTilePalette()
    {
        if (!m_tilePalettePanelOpen)
        {
            return;
        }

        ImGui::SetNextWindowSize(
            ImVec2{ 360.0f, 430.0f },
            ImGuiCond_FirstUseEver);
        constexpr ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoCollapse;
        if (!ImGui::Begin(
                "タイルパレット",
                &m_tilePalettePanelOpen,
                flags))
        {
            ImGui::End();
            return;
        }

        auto* selected =
            m_scene.FindGameObject(
                m_selectedObjectId);
        auto* tilemap = selected != nullptr
            ? selected->GetComponent<
                TilemapComponent>()
            : nullptr;
        if (tilemap == nullptr)
        {
            ImGui::TextWrapped(
                "ヒエラルキーからTilemapコンポーネントを持つGameObjectを選択してください。");
            ImGui::TextDisabled(
                "タブはドラッグして好きな場所へ移動・分離・サイズ変更できます。");
            ImGui::End();
            return;
        }

        ImGui::Text(
            "%s  (%zuセル)",
            selected->Name().c_str(),
            tilemap->Cells().size());
        ImGui::Separator();

        ImGui::BeginDisabled(m_playing);
        const bool paintSelected =
            m_tilemapTool
                == TilemapTool::Paint;
        if (paintSelected)
        {
            ImGui::PushStyleColor(
                ImGuiCol_Button,
                ImVec4{
                    0.10f,
                    0.42f,
                    0.66f,
                    1.0f });
        }
        if (ImGui::Button(
                "ペイント (B)",
                ImVec2{ 126.0f, 0.0f }))
        {
            m_tilemapTool =
                TilemapTool::Paint;
        }
        if (paintSelected)
        {
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();

        const bool eraseSelected =
            m_tilemapTool
                == TilemapTool::Erase;
        if (eraseSelected)
        {
            ImGui::PushStyleColor(
                ImGuiCol_Button,
                ImVec4{
                    0.58f,
                    0.20f,
                    0.22f,
                    1.0f });
        }
        if (ImGui::Button(
                "消去 (E)",
                ImVec2{ 110.0f, 0.0f }))
        {
            m_tilemapTool =
                TilemapTool::Erase;
        }
        if (eraseSelected)
        {
            ImGui::PopStyleColor();
        }

        if (m_sceneViewportHovered
            && !ImGui::GetIO().WantTextInput)
        {
            if (ImGui::IsKeyPressed(
                    ImGuiKey_B,
                    false))
            {
                m_tilemapTool =
                    TilemapTool::Paint;
            }
            if (ImGui::IsKeyPressed(
                    ImGuiKey_E,
                    false))
            {
                m_tilemapTool =
                    TilemapTool::Erase;
            }
        }

        ImGui::TextDisabled(
            "Sceneタブ上を左ドラッグして編集");
        ImGui::SeparatorText("タイルシート");

        const auto texturePath =
            PathToUtf8(
                tilemap->TexturePath());
        ImGui::TextWrapped(
            "%s",
            texturePath.empty()
                ? "画像未設定（白タイル）"
                : texturePath.c_str());
        ImGui::Button(
            "画像をここへドロップ",
            ImVec2{ -1.0f, 0.0f });
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload(
                    AssetPayload))
            {
                const auto droppedPath =
                    PathFromUtf8(
                        static_cast<
                            const char*>(
                                payload->Data));
                if (IsTextureAsset(
                        droppedPath))
                {
                    try
                    {
                        tilemap->SetTexturePath(
                            droppedPath);
                        RecordHistory();
                        SetStatus(
                            "Tilemapへタイルシートを設定しました");
                    }
                    catch (
                        const std::exception&
                            exception)
                    {
                        SetStatus(
                            exception.what(),
                            true);
                    }
                }
                else
                {
                    SetStatus(
                        "画像ファイルをドロップしてください",
                        true);
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::BeginDisabled(
            !IsTextureAsset(
                m_selectedAsset));
        if (ImGui::Button(
                "アセットで選択中の画像を使用",
                ImVec2{ -1.0f, 0.0f }))
        {
            try
            {
                tilemap->SetTexturePath(
                    m_selectedAsset);
                RecordHistory();
                SetStatus(
                    "Tilemapへタイルシートを設定しました");
            }
            catch (
                const std::exception&
                    exception)
            {
                SetStatus(
                    exception.what(),
                    true);
            }
        }
        ImGui::EndDisabled();

        int atlasGrid[]{
            static_cast<int>(
                tilemap->AtlasColumns()),
            static_cast<int>(
                tilemap->AtlasRows())
        };
        if (ImGui::InputInt2(
                "列 / 行",
                atlasGrid))
        {
            tilemap->SetAtlasGrid(
                static_cast<std::uint32_t>(
                    std::max(
                        atlasGrid[0],
                        1)),
                static_cast<std::uint32_t>(
                    std::max(
                        atlasGrid[1],
                        1)));
            m_tilePaletteSelectedTile =
                std::min(
                    m_tilePaletteSelectedTile,
                    tilemap->TileCount() - 1);
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            RecordHistory();
        }

        auto tileSize =
            tilemap->TileSize();
        if (ImGui::DragFloat2(
                "セルサイズ",
                &tileSize.x,
                1.0f,
                1.0f,
                4096.0f,
                "%.0f"))
        {
            tilemap->SetTileSize(tileSize);
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            RecordHistory();
        }

        ImGui::BeginDisabled(
            tilemap->Cells().empty());
        if (ImGui::Button("全セルを消去"))
        {
            tilemap->Clear();
            RecordHistory();
            SetStatus(
                "Tilemapの全セルを消去しました");
        }
        ImGui::EndDisabled();

        ImGui::SeparatorText("パレット");
        ImGui::Text(
            "選択中: Tile %u",
            m_tilePaletteSelectedTile);

        std::shared_ptr<
            const TextureAsset> texture;
        if (!tilemap->TexturePath().empty())
        {
            try
            {
                texture =
                    m_graphics.Assets().
                        LoadTexture(
                            tilemap->
                                TexturePath());
            }
            catch (
                const std::exception&
                    exception)
            {
                ImGui::TextWrapped(
                    "読込エラー: %s",
                    exception.what());
            }
        }

        ImGui::BeginChild(
            "TilePaletteGrid",
            ImVec2{ 0.0f, 0.0f },
            ImGuiChildFlags_Borders);
        const std::uint32_t visibleCount =
            std::min(
                tilemap->TileCount(),
                1024u);
        const int displayColumns =
            std::max(
                static_cast<int>(
                    ImGui::GetContentRegionAvail().x
                    / 58.0f),
                1);
        for (std::uint32_t tileIndex{};
            tileIndex < visibleCount;
            ++tileIndex)
        {
            ImGui::PushID(
                static_cast<int>(
                    tileIndex));
            const bool selectedTile =
                tileIndex
                    == m_tilePaletteSelectedTile;
            ImGui::PushStyleVar(
                ImGuiStyleVar_FrameBorderSize,
                selectedTile ? 3.0f : 0.0f);
            ImGui::PushStyleColor(
                ImGuiCol_Border,
                ImVec4{
                    0.10f,
                    0.82f,
                    1.0f,
                    1.0f });

            bool clicked{};
            if (texture)
            {
                const std::uint32_t column =
                    tileIndex
                    % tilemap->AtlasColumns();
                const std::uint32_t row =
                    tileIndex
                    / tilemap->AtlasColumns();
                const ImVec2 uv0{
                    static_cast<float>(column)
                        / tilemap->
                            AtlasColumns(),
                    static_cast<float>(row)
                        / tilemap->
                            AtlasRows()
                };
                const ImVec2 uv1{
                    static_cast<float>(
                        column + 1)
                        / tilemap->
                            AtlasColumns(),
                    static_cast<float>(
                        row + 1)
                        / tilemap->
                            AtlasRows()
                };
                clicked =
                    ImGui::ImageButton(
                        "##Tile",
                        MakeTextureReference(
                            texture->view.Get()),
                        ImVec2{
                            48.0f,
                            48.0f },
                        uv0,
                        uv1);
            }
            else
            {
                clicked = ImGui::Button(
                    std::to_string(
                        tileIndex).c_str(),
                    ImVec2{
                        48.0f,
                        48.0f });
            }

            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            if (clicked)
            {
                m_tilePaletteSelectedTile =
                    tileIndex;
                m_tilemapTool =
                    TilemapTool::Paint;
            }
            ImGui::PopID();

            if ((static_cast<int>(
                    tileIndex) + 1)
                % displayColumns != 0)
            {
                ImGui::SameLine();
            }
        }
        if (tilemap->TileCount()
            > visibleCount)
        {
            ImGui::TextDisabled(
                "先頭1024タイルを表示しています");
        }
        ImGui::EndChild();
        ImGui::EndDisabled();
        ImGui::End();
    }
}
