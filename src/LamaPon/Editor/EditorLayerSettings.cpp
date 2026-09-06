#include "LamaPon/Editor/EditorLayer.h"

#include "LamaPon/Editor/EditorLayerShared.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/Scene.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>

using namespace LamaPon::EditorDetail;

namespace
{
    constexpr std::size_t MaximumHistoryEntries = 64;
}

namespace LamaPon
{    std::filesystem::path EditorLayer::EditorSettingsPath() const
    {
        return m_graphics.Assets().AssetRoot().parent_path()
            / ".lamapon"
            / "editor-settings.json";
    }

    bool EditorLayer::LoadEditorSettings()
    {
        const auto path = EditorSettingsPath();
        if (!std::filesystem::exists(path))
        {
            return false;
        }

        std::ifstream input(path);
        if (!input)
        {
            throw std::runtime_error(
                "Failed to open editor settings: "
                + LamaPon::PathToUtf8(path));
        }

        nlohmann::json settings;
        input >> settings;

        const auto readFloat3 =
            [](const nlohmann::json& value, DirectX::XMFLOAT3& target)
            {
                if (!value.is_array() || value.size() != 3)
                {
                    return;
                }

                const float x = value[0].get<float>();
                const float y = value[1].get<float>();
                const float z = value[2].get<float>();
                if (std::isfinite(x)
                    && std::isfinite(y)
                    && std::isfinite(z))
                {
                    target = { x, y, z };
                }
            };

        if (const auto iterator = settings.find("sceneCamera");
            iterator != settings.end() && iterator->is_object())
        {
            const auto& camera = *iterator;
            if (camera.contains("position"))
            {
                readFloat3(camera["position"], m_sceneCameraPosition);
            }
            if (camera.contains("rotation"))
            {
                readFloat3(camera["rotation"], m_sceneCameraRotation);
            }
            m_sceneCameraRotation.x = std::clamp(
                m_sceneCameraRotation.x,
                -DirectX::XM_PIDIV2 + 0.01f,
                DirectX::XM_PIDIV2 - 0.01f);
            m_sceneCameraRotation.z = 0.0f;
            m_sceneCameraFocusDistance = std::clamp(
                camera.value(
                    "focusDistance",
                    m_sceneCameraFocusDistance),
                0.2f,
                1000.0f);
            m_sceneOrthographic =
                camera.value("orthographic", m_sceneOrthographic);
            m_scene2DMode =
                camera.value("mode2D", false)
                && m_sceneOrthographic;
            m_sceneOrthographicSize = std::clamp(
                camera.value(
                    "orthographicSize",
                    m_sceneOrthographicSize),
                0.1f,
                1000.0f);
            m_sceneCameraSpeed = std::clamp(
                camera.value("speed", m_sceneCameraSpeed),
                0.1f,
                100.0f);
            m_sceneCameraBoostMultiplier = std::clamp(
                camera.value(
                    "boostMultiplier",
                    m_sceneCameraBoostMultiplier),
                1.0f,
                10.0f);
            m_sceneCameraLookSensitivity = std::clamp(
                camera.value(
                    "lookSensitivity",
                    m_sceneCameraLookSensitivity),
                0.1f,
                3.0f);
            m_sceneCameraZoomSensitivity = std::clamp(
                camera.value(
                    "zoomSensitivity",
                    m_sceneCameraZoomSensitivity),
                0.1f,
                3.0f);
        }

        if (const auto iterator = settings.find("grid");
            iterator != settings.end() && iterator->is_object())
        {
            const auto& grid = *iterator;
            m_gridVisible = grid.value("visible", m_gridVisible);
            // デバッグ線の表示。旧設定には無いので、未指定なら
            // 従来どおり表示（true）のままにします。
            m_colliderDebugVisible = grid.value(
                "colliderDebugVisible",
                m_colliderDebugVisible);
            m_lightGizmosVisible = grid.value(
                "lightGizmosVisible",
                m_lightGizmosVisible);
            m_cameraGizmosVisible = grid.value(
                "cameraGizmosVisible",
                m_cameraGizmosVisible);
            m_gridSpacing = std::clamp(
                grid.value("spacing", m_gridSpacing),
                0.1f,
                100.0f);
            m_gridExtent = std::clamp(
                grid.value("extent", m_gridExtent),
                1.0f,
                1000.0f);
        }

        if (const auto iterator = settings.find("viewCube");
            iterator != settings.end() && iterator->is_object())
        {
            m_viewCubeVisible =
                iterator->value("visible", m_viewCubeVisible);
        }

        if (const auto iterator = settings.find("gizmo");
            iterator != settings.end() && iterator->is_object())
        {
            const auto& gizmo = *iterator;
            const int operation = std::clamp(
                gizmo.value(
                    "operation",
                    static_cast<int>(m_gizmoOperation)),
                0,
                2);
            m_gizmoOperation =
                static_cast<GizmoOperation>(operation);
            m_gizmoLocal = gizmo.value("local", m_gizmoLocal);
            m_snapEnabled =
                gizmo.value("snapEnabled", m_snapEnabled);
            m_translationSnap = std::clamp(
                gizmo.value(
                    "translationSnap",
                    m_translationSnap),
                0.01f,
                100.0f);
            m_rotationSnap = std::clamp(
                gizmo.value("rotationSnap", m_rotationSnap),
                1.0f,
                180.0f);
            m_scaleSnap = std::clamp(
                gizmo.value("scaleSnap", m_scaleSnap),
                0.01f,
                10.0f);
        }

        if (const auto iterator = settings.find("assetBrowser");
            iterator != settings.end() && iterator->is_object())
        {
            const auto& assets = *iterator;
            m_assetGridView =
                assets.value("gridView", m_assetGridView);
            m_assetDirectoryTreeVisible = assets.value(
                "directoryTreeVisible",
                m_assetDirectoryTreeVisible);
            const auto directory = PathFromUtf8(
                assets.value("directory", std::string{}));
            if (directory.empty()
                || std::ranges::find(
                    m_assetDirectories,
                    directory) != m_assetDirectories.end())
            {
                m_assetDirectory = directory;
            }
        }

        if (const auto iterator = settings.find("panels");
            iterator != settings.end() && iterator->is_object())
        {
            const auto& panels = *iterator;
            for (const auto& [id, value] : panels.items())
            {
                if (value.is_boolean())
                {
                    m_editorExtensions.RestorePanelVisibility(
                        id,
                        value.get<bool>());
                }
            }
        }

        if (const auto iterator = settings.find("gameView");
            iterator != settings.end() && iterator->is_object())
        {
            const auto& gameView = *iterator;
            m_gameViewFixedResolution = gameView.value(
                "fixedResolution", m_gameViewFixedResolution);
            m_gameViewResolutionWidth = std::clamp(
                gameView.value(
                    "width", m_gameViewResolutionWidth),
                16,
                8192);
            m_gameViewResolutionHeight = std::clamp(
                gameView.value(
                    "height", m_gameViewResolutionHeight),
                16,
                8192);
            m_gameViewResolutionScale = std::clamp(
                gameView.value(
                    "resolutionScale",
                    m_gameViewResolutionScale),
                0.25f,
                1.0f);
        }

        if (const auto iterator = settings.find("presets");
            iterator != settings.end() && iterator->is_array())
        {
            std::vector<EditorSettingsPreset> presets;
            for (const auto& value : *iterator)
            {
                if (!value.is_object())
                {
                    continue;
                }
                const auto name =
                    value.value("name", std::string{});
                if (name.empty() || name.size() > 64)
                {
                    continue;
                }
                if (std::ranges::any_of(
                    presets,
                    [&name](
                        const EditorSettingsPreset& preset)
                    {
                        return Lowercase(preset.name)
                            == Lowercase(name);
                    }))
                {
                    continue;
                }

                EditorSettingsPreset preset;
                preset.name = name;
                preset.sceneOrthographic =
                    value.value(
                        "orthographic",
                        preset.sceneOrthographic);
                preset.sceneOrthographicSize = std::clamp(
                    value.value(
                        "orthographicSize",
                        preset.sceneOrthographicSize),
                    0.1f,
                    1000.0f);
                preset.sceneCameraSpeed = std::clamp(
                    value.value(
                        "cameraSpeed",
                        preset.sceneCameraSpeed),
                    0.1f,
                    100.0f);
                preset.sceneCameraBoostMultiplier =
                    std::clamp(
                        value.value(
                            "cameraBoostMultiplier",
                            preset.
                                sceneCameraBoostMultiplier),
                        1.0f,
                        10.0f);
                preset.sceneCameraLookSensitivity =
                    std::clamp(
                        value.value(
                            "cameraLookSensitivity",
                            preset.
                                sceneCameraLookSensitivity),
                        0.1f,
                        3.0f);
                preset.sceneCameraZoomSensitivity =
                    std::clamp(
                        value.value(
                            "cameraZoomSensitivity",
                            preset.
                                sceneCameraZoomSensitivity),
                        0.1f,
                        3.0f);
                preset.gridVisible = value.value(
                    "gridVisible",
                    preset.gridVisible);
                preset.gridSpacing = std::clamp(
                    value.value(
                        "gridSpacing",
                        preset.gridSpacing),
                    0.1f,
                    100.0f);
                preset.gridExtent = std::clamp(
                    value.value(
                        "gridExtent",
                        preset.gridExtent),
                    1.0f,
                    1000.0f);
                preset.gizmoOperation =
                    static_cast<GizmoOperation>(
                        std::clamp(
                            value.value(
                                "gizmoOperation",
                                static_cast<int>(
                                    preset.gizmoOperation)),
                            0,
                            2));
                preset.gizmoLocal = value.value(
                    "gizmoLocal",
                    preset.gizmoLocal);
                preset.snapEnabled = value.value(
                    "snapEnabled",
                    preset.snapEnabled);
                preset.translationSnap = std::clamp(
                    value.value(
                        "translationSnap",
                        preset.translationSnap),
                    0.01f,
                    100.0f);
                preset.rotationSnap = std::clamp(
                    value.value(
                        "rotationSnap",
                        preset.rotationSnap),
                    1.0f,
                    180.0f);
                preset.scaleSnap = std::clamp(
                    value.value(
                        "scaleSnap",
                        preset.scaleSnap),
                    0.01f,
                    10.0f);
                presets.emplace_back(std::move(preset));
            }

            if (!presets.empty())
            {
                m_editorPresets = std::move(presets);
            }
        }

        m_selectedEditorPreset = 0;
        const auto selectedPreset =
            settings.value(
                "selectedPreset",
                std::string{});
        for (std::size_t index = 0;
            index < m_editorPresets.size();
            ++index)
        {
            if (m_editorPresets[index].name
                == selectedPreset)
            {
                m_selectedEditorPreset = index;
                break;
            }
        }

        return true;
    }

    void EditorLayer::SaveEditorSettings() const
    {
        const auto path = EditorSettingsPath();
        std::filesystem::create_directories(path.parent_path());

        nlohmann::json panelVisibility = nlohmann::json::object();
        for (const auto& panel : m_editorExtensions.Panels())
        {
            panelVisibility[panel.id] = panel.open;
        }

        nlohmann::json settings{
            { "version", 2 },
            {
                "sceneCamera",
                {
                    {
                        "position",
                        {
                            m_sceneCameraPosition.x,
                            m_sceneCameraPosition.y,
                            m_sceneCameraPosition.z
                        }
                    },
                    {
                        "rotation",
                        {
                            m_sceneCameraRotation.x,
                            m_sceneCameraRotation.y,
                            m_sceneCameraRotation.z
                        }
                    },
                    { "focusDistance", m_sceneCameraFocusDistance },
                    { "orthographic", m_sceneOrthographic },
                    { "mode2D", m_scene2DMode },
                    { "orthographicSize", m_sceneOrthographicSize },
                    { "speed", m_sceneCameraSpeed },
                    {
                        "boostMultiplier",
                        m_sceneCameraBoostMultiplier
                    },
                    {
                        "lookSensitivity",
                        m_sceneCameraLookSensitivity
                    },
                    {
                        "zoomSensitivity",
                        m_sceneCameraZoomSensitivity
                    }
                }
            },
            {
                "grid",
                {
                    { "visible", m_gridVisible },
                    { "spacing", m_gridSpacing },
                    { "extent", m_gridExtent },
                    {
                        "colliderDebugVisible",
                        m_colliderDebugVisible
                    },
                    {
                        "lightGizmosVisible",
                        m_lightGizmosVisible
                    },
                    {
                        "cameraGizmosVisible",
                        m_cameraGizmosVisible
                    }
                }
            },
            {
                "viewCube",
                {
                    { "visible", m_viewCubeVisible }
                }
            },
            {
                "gizmo",
                {
                    {
                        "operation",
                        static_cast<int>(m_gizmoOperation)
                    },
                    { "local", m_gizmoLocal },
                    { "snapEnabled", m_snapEnabled },
                    { "translationSnap", m_translationSnap },
                    { "rotationSnap", m_rotationSnap },
                    { "scaleSnap", m_scaleSnap }
                }
            },
            {
                "assetBrowser",
                {
                    { "gridView", m_assetGridView },
                    {
                        "directoryTreeVisible",
                        m_assetDirectoryTreeVisible
                    },
                    {
                        "directory",
                        PathToUtf8(m_assetDirectory)
                    }
                }
            },
            {
                "panels",
                std::move(panelVisibility)
            },
            {
                "gameView",
                {
                    {
                        "fixedResolution",
                        m_gameViewFixedResolution
                    },
                    { "width", m_gameViewResolutionWidth },
                    { "height", m_gameViewResolutionHeight },
                    {
                        "resolutionScale",
                        m_gameViewResolutionScale
                    }
                }
            }
        };

        settings["presets"] = nlohmann::json::array();
        for (const auto& preset : m_editorPresets)
        {
            settings["presets"].push_back({
                { "name", preset.name },
                {
                    "orthographic",
                    preset.sceneOrthographic
                },
                {
                    "orthographicSize",
                    preset.sceneOrthographicSize
                },
                {
                    "cameraSpeed",
                    preset.sceneCameraSpeed
                },
                {
                    "cameraBoostMultiplier",
                    preset.sceneCameraBoostMultiplier
                },
                {
                    "cameraLookSensitivity",
                    preset.sceneCameraLookSensitivity
                },
                {
                    "cameraZoomSensitivity",
                    preset.sceneCameraZoomSensitivity
                },
                { "gridVisible", preset.gridVisible },
                { "gridSpacing", preset.gridSpacing },
                { "gridExtent", preset.gridExtent },
                {
                    "gizmoOperation",
                    static_cast<int>(
                        preset.gizmoOperation)
                },
                { "gizmoLocal", preset.gizmoLocal },
                { "snapEnabled", preset.snapEnabled },
                {
                    "translationSnap",
                    preset.translationSnap
                },
                {
                    "rotationSnap",
                    preset.rotationSnap
                },
                { "scaleSnap", preset.scaleSnap }
            });
        }
        settings["selectedPreset"] =
            m_editorPresets.empty()
                ? std::string{}
                : m_editorPresets[
                    std::min(
                        m_selectedEditorPreset,
                        m_editorPresets.size() - 1)].name;

        std::ofstream output(path, std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "Failed to write editor settings: "
                + LamaPon::PathToUtf8(path));
        }
        output << settings.dump(2) << '\n';
        if (!output)
        {
            throw std::runtime_error(
                "Failed to save editor settings: "
                + LamaPon::PathToUtf8(path));
        }
    }

    void EditorLayer::ResetEditorSettings()
    {
        m_sceneCameraPosition = { 0.0f, 1.8f, 7.0f };
        m_sceneCameraRotation = { -0.12f, 0.0f, 0.0f };
        m_sceneCameraFocusDistance = 7.0f;
        m_sceneOrthographicSize = 10.0f;
        m_sceneCameraSpeed = 5.0f;
        m_sceneCameraBoostMultiplier = 3.0f;
        m_sceneCameraLookSensitivity = 1.0f;
        m_sceneCameraZoomSensitivity = 1.0f;
        m_sceneOrthographic = false;
        m_scene2DMode = false;
        m_scene3DViewStored = false;
        m_gridVisible = true;
        m_colliderDebugVisible = true;
        m_lightGizmosVisible = true;
        m_cameraGizmosVisible = true;
        m_gridSpacing = 1.0f;
        m_gridExtent = 20.0f;
        m_gizmoOperation = GizmoOperation::Translate;
        m_gizmoLocal = false;
        m_snapEnabled = false;
        m_translationSnap = 0.5f;
        m_rotationSnap = 15.0f;
        m_scaleSnap = 0.1f;
        m_assetGridView = true;
        m_assetDirectoryTreeVisible = false;
        m_assetDirectory.clear();
        m_assetFilter.fill('\0');
        m_editorExtensions.ResetPanelVisibility();
        m_resetDockLayout = true;
    }

    void EditorLayer::CreateDefaultEditorPresets()
    {
        m_editorPresets.clear();

        EditorSettingsPreset standard;
        standard.name = "標準";
        m_editorPresets.emplace_back(standard);

        EditorSettingsPreset levelDesign;
        levelDesign.name = "レベルデザイン";
        levelDesign.sceneCameraSpeed = 8.0f;
        levelDesign.sceneCameraBoostMultiplier = 4.0f;
        levelDesign.gridExtent = 50.0f;
        levelDesign.snapEnabled = true;
        levelDesign.translationSnap = 0.5f;
        levelDesign.rotationSnap = 15.0f;
        levelDesign.scaleSnap = 0.1f;
        m_editorPresets.emplace_back(levelDesign);

        EditorSettingsPreset precision;
        precision.name = "精密配置";
        precision.sceneCameraSpeed = 2.0f;
        precision.sceneCameraBoostMultiplier = 2.0f;
        precision.gridSpacing = 0.25f;
        precision.snapEnabled = true;
        precision.translationSnap = 0.1f;
        precision.rotationSnap = 5.0f;
        precision.scaleSnap = 0.05f;
        m_editorPresets.emplace_back(precision);

        m_selectedEditorPreset = 0;
        strncpy_s(
            m_editorPresetNameBuffer.data(),
            m_editorPresetNameBuffer.size(),
            "新しいプリセット",
            _TRUNCATE);
        m_editorPresetError.clear();
    }

    void EditorLayer::ApplyEditorPreset(
        const std::size_t index)
    {
        if (index >= m_editorPresets.size())
        {
            return;
        }

        m_selectedEditorPreset = index;
        const auto& preset =
            m_editorPresets[m_selectedEditorPreset];
        m_sceneOrthographic =
            preset.sceneOrthographic;
        m_sceneOrthographicSize =
            preset.sceneOrthographicSize;
        m_sceneCameraSpeed =
            preset.sceneCameraSpeed;
        m_sceneCameraBoostMultiplier =
            preset.sceneCameraBoostMultiplier;
        m_sceneCameraLookSensitivity =
            preset.sceneCameraLookSensitivity;
        m_sceneCameraZoomSensitivity =
            preset.sceneCameraZoomSensitivity;
        m_gridVisible = preset.gridVisible;
        m_colliderDebugVisible = preset.colliderDebugVisible;
        m_lightGizmosVisible = preset.lightGizmosVisible;
        m_cameraGizmosVisible = preset.cameraGizmosVisible;
        m_gridSpacing = preset.gridSpacing;
        m_gridExtent = preset.gridExtent;
        m_gizmoOperation = preset.gizmoOperation;
        m_gizmoLocal = preset.gizmoLocal;
        m_snapEnabled = preset.snapEnabled;
        m_translationSnap = preset.translationSnap;
        m_rotationSnap = preset.rotationSnap;
        m_scaleSnap = preset.scaleSnap;
        m_editorPresetError.clear();
        SaveEditorSettings();
        SetStatus(
            "エディタープリセットを適用しました: "
            + preset.name);
    }

    void EditorLayer::SaveCurrentEditorPreset(
        std::string name)
    {
        const bool blank = name.empty()
            || std::ranges::all_of(
                name,
                [](const unsigned char character)
                {
                    return std::isspace(character) != 0;
                });
        if (blank)
        {
            m_editorPresetError =
                "プリセット名を入力してください";
            return;
        }
        if (name.size() > 64)
        {
            m_editorPresetError =
                "プリセット名は64バイト以内にしてください";
            return;
        }
        if (std::ranges::any_of(
            m_editorPresets,
            [&name](const EditorSettingsPreset& preset)
            {
                return Lowercase(preset.name)
                    == Lowercase(name);
            }))
        {
            m_editorPresetError =
                "同じ名前のプリセットが存在します";
            return;
        }

        EditorSettingsPreset preset;
        preset.name = std::move(name);
        preset.sceneOrthographic =
            m_sceneOrthographic;
        preset.sceneOrthographicSize =
            m_sceneOrthographicSize;
        preset.sceneCameraSpeed =
            m_sceneCameraSpeed;
        preset.sceneCameraBoostMultiplier =
            m_sceneCameraBoostMultiplier;
        preset.sceneCameraLookSensitivity =
            m_sceneCameraLookSensitivity;
        preset.sceneCameraZoomSensitivity =
            m_sceneCameraZoomSensitivity;
        preset.gridVisible = m_gridVisible;
        preset.colliderDebugVisible = m_colliderDebugVisible;
        preset.lightGizmosVisible = m_lightGizmosVisible;
        preset.cameraGizmosVisible = m_cameraGizmosVisible;
        preset.gridSpacing = m_gridSpacing;
        preset.gridExtent = m_gridExtent;
        preset.gizmoOperation = m_gizmoOperation;
        preset.gizmoLocal = m_gizmoLocal;
        preset.snapEnabled = m_snapEnabled;
        preset.translationSnap = m_translationSnap;
        preset.rotationSnap = m_rotationSnap;
        preset.scaleSnap = m_scaleSnap;
        m_editorPresets.emplace_back(std::move(preset));
        m_selectedEditorPreset =
            m_editorPresets.size() - 1;
        m_editorPresetError.clear();
        SaveEditorSettings();
        SetStatus(
            "エディタープリセットを作成しました: "
            + m_editorPresets[
                m_selectedEditorPreset].name);
    }

    void EditorLayer::UpdateSelectedEditorPreset()
    {
        if (m_selectedEditorPreset
            >= m_editorPresets.size())
        {
            return;
        }

        auto& preset =
            m_editorPresets[m_selectedEditorPreset];
        preset.sceneOrthographic =
            m_sceneOrthographic;
        preset.sceneOrthographicSize =
            m_sceneOrthographicSize;
        preset.sceneCameraSpeed =
            m_sceneCameraSpeed;
        preset.sceneCameraBoostMultiplier =
            m_sceneCameraBoostMultiplier;
        preset.sceneCameraLookSensitivity =
            m_sceneCameraLookSensitivity;
        preset.sceneCameraZoomSensitivity =
            m_sceneCameraZoomSensitivity;
        preset.gridVisible = m_gridVisible;
        preset.colliderDebugVisible = m_colliderDebugVisible;
        preset.lightGizmosVisible = m_lightGizmosVisible;
        preset.cameraGizmosVisible = m_cameraGizmosVisible;
        preset.gridSpacing = m_gridSpacing;
        preset.gridExtent = m_gridExtent;
        preset.gizmoOperation = m_gizmoOperation;
        preset.gizmoLocal = m_gizmoLocal;
        preset.snapEnabled = m_snapEnabled;
        preset.translationSnap = m_translationSnap;
        preset.rotationSnap = m_rotationSnap;
        preset.scaleSnap = m_scaleSnap;
        m_editorPresetError.clear();
        SaveEditorSettings();
        SetStatus(
            "エディタープリセットを上書きしました: "
            + preset.name);
    }

    void EditorLayer::DeleteSelectedEditorPreset()
    {
        if (m_selectedEditorPreset
            >= m_editorPresets.size())
        {
            return;
        }
        if (m_editorPresets.size() <= 1)
        {
            m_editorPresetError =
                "最後のプリセットは削除できません";
            return;
        }

        const auto deletedName =
            m_editorPresets[
                m_selectedEditorPreset].name;
        m_editorPresets.erase(
            m_editorPresets.begin()
                + static_cast<std::ptrdiff_t>(
                    m_selectedEditorPreset));
        m_selectedEditorPreset = std::min(
            m_selectedEditorPreset,
            m_editorPresets.size() - 1);
        m_editorPresetError.clear();
        SaveEditorSettings();
        SetStatus(
            "エディタープリセットを削除しました: "
            + deletedName);
    }

    void EditorLayer::ResetHistory()
    {
        // シーンの入れ替え時に呼ばれるため、消えたGameObjectを
        // 指したままの追加選択もここで捨てます。
        ClearMultiSelection();
        m_history.clear();
        m_history.push_back(m_scene.SerializeToJson());
        m_historyIndex = 0;
    }

    void EditorLayer::RecordHistory()
    {
        if (m_playing)
        {
            return;
        }

        try
        {
            const std::string snapshot = m_scene.SerializeToJson();

            if (!m_history.empty() && snapshot == m_history[m_historyIndex])
            {
                return;
            }

            m_history.erase(
                m_history.begin() + static_cast<std::ptrdiff_t>(m_historyIndex + 1),
                m_history.end());
            m_history.push_back(snapshot);

            if (m_history.size() > MaximumHistoryEntries)
            {
                m_history.erase(m_history.begin());
            }

            m_historyIndex = m_history.size() - 1;
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    void EditorLayer::Undo()
    {
        if (!CanUndo() || m_playing)
        {
            return;
        }

        --m_historyIndex;
        RestoreHistoryState();
        SetStatus("元に戻しました");
    }

    void EditorLayer::Redo()
    {
        if (!CanRedo() || m_playing)
        {
            return;
        }

        ++m_historyIndex;
        RestoreHistoryState();
        SetStatus("やり直しました");
    }

    void EditorLayer::RestoreHistoryState()
    {
        try
        {
            if (m_animationTimelineOpen)
            {
                CloseAnimationTimeline(true);
            }
            // Undo履歴は主シーンだけのスナップショットなので、
            // 復元（LoadFromJson）で追加シーンが消えます。同じ
            // ファイルを読み直して、表示状態を保ちます。
            std::vector<std::filesystem::path>
                additiveScenes;
            additiveScenes.reserve(
                m_scene.AdditiveScenes().size());
            for (const auto& additiveScene :
                m_scene.AdditiveScenes())
            {
                additiveScenes.push_back(
                    additiveScene.path);
            }
            m_scene.LoadFromJson(m_history[m_historyIndex]);
            for (const auto& additiveScene :
                additiveScenes)
            {
                try
                {
                    static_cast<void>(
                        m_scene.MergeFromFile(
                            additiveScene));
                }
                catch (const std::exception&
                    exception)
                {
                    Logger::Instance().Warning(
                        "追加シーンを復元できませんでした: "
                        + PathToUtf8(additiveScene)
                        + " | "
                        + exception.what());
                }
            }
            if (m_scene.FindGameObject(m_selectedObjectId) == nullptr)
            {
                m_selectedObjectId = 0;
            }
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    bool EditorLayer::CanUndo() const noexcept
    {
        return !m_history.empty() && m_historyIndex > 0;
    }

    bool EditorLayer::CanRedo() const noexcept
    {
        return !m_history.empty() && m_historyIndex + 1 < m_history.size();
    }

    void EditorLayer::SetStatus(std::string message, const bool error)
    {
        if (error)
        {
            Logger::Instance().Error(
                message,
                m_selectedObjectId);
        }
        else
        {
            Logger::Instance().Info(
                message,
                m_selectedObjectId);
        }
        m_statusMessage = std::move(message);
        m_statusIsError = error;
    }
}
