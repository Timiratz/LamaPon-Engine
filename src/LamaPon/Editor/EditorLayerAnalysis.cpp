#include "LamaPon/Editor/EditorLayer.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Editor/DebugCaptureFiles.h"
#include "LamaPon/Editor/FrameDebuggerPanel.h"
#include "LamaPon/Editor/MemoryProfilerPanel.h"
#include "LamaPon/Editor/PhysicsDebuggerPanel.h"
#include "LamaPon/Editor/ProfileAnalyzerPanel.h"
#include "LamaPon/Editor/ProfilerPanel.h"
#include "LamaPon/Graphics/FrameDebugger.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/MemorySnapshotCapture.h"
#include "LamaPon/Scene/Scene.h"

#include <imgui.h>

// ファイル選択ダイアログ（GetOpenFileNameW）に必要。
#include <commdlg.h>

#include <array>
#include <stdexcept>
#include <string>
#include <utility>

namespace LamaPon
{
    namespace
    {
        constexpr const char* AnalysisMenuGroup = "解析";
    }

    std::filesystem::path EditorLayer::DebugCaptureDirectory(
        const std::wstring_view name) const
    {
        const auto* assets = m_graphics.TryAssets();
        if (assets == nullptr || assets->AssetRoot().empty())
        {
            return {};
        }
        // エディターログやprofile.jsonと同じく、Gitの追跡対象外の
        // .lamapon配下に置きます。
        return assets->AssetRoot().parent_path()
            / L".lamapon"
            / std::filesystem::path{ std::wstring{ name } };
    }

    std::optional<std::filesystem::path>
        EditorLayer::OpenDebugCaptureDialog(
            const std::filesystem::path& initialDirectory)
    {
        std::array<wchar_t, 32768> filename{};
        const std::wstring initial = initialDirectory.wstring();
        constexpr wchar_t filter[] =
            L"解析の記録 (*.json)\0*.json\0\0";

        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = m_window;
        dialog.lpstrFilter = filter;
        dialog.nFilterIndex = 1;
        dialog.lpstrFile = filename.data();
        dialog.nMaxFile = static_cast<DWORD>(filename.size());
        dialog.lpstrInitialDir =
            initial.empty() ? nullptr : initial.c_str();
        dialog.lpstrTitle = L"解析の記録を開く";
        dialog.Flags =
            OFN_FILEMUSTEXIST
            | OFN_PATHMUSTEXIST
            | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&dialog))
        {
            if (CommDlgExtendedError() != 0)
            {
                SetStatus(
                    "ファイルを開くダイアログを表示できませんでした",
                    true);
            }
            return std::nullopt;
        }
        return std::filesystem::path{ filename.data() };
    }

    void EditorLayer::RegisterAnalysisExtension()
    {
        auto status = [this](std::string message, const bool error)
        {
            SetStatus(std::move(message), error);
        };
        auto openFile = [this](const std::filesystem::path& directory)
        {
            return OpenDebugCaptureDialog(directory);
        };
        auto selectObject = [this](const GameObjectId id)
        {
            if (m_scene.FindGameObject(id) != nullptr)
            {
                SelectObject(id, false);
            }
        };

        m_profilerPanel = std::make_unique<ProfilerPanel>(status);
        m_profileAnalyzerPanel =
            std::make_unique<ProfileAnalyzerPanel>(status, openFile);
        m_memoryProfilerPanel = std::make_unique<MemoryProfilerPanel>(
            [this]
            {
                return CaptureMemorySnapshot(m_graphics);
            },
            status,
            openFile);
        m_frameDebuggerPanel = std::make_unique<FrameDebuggerPanel>(
            m_graphics.FrameDebug(),
            selectObject,
            [this]
            {
                // 描画は続けたまま更新だけを止め、途中の絵を固定します。
                if (m_playing && !m_paused)
                {
                    SetPaused(true);
                }
            });
        m_physicsDebuggerPanel =
            std::make_unique<PhysicsDebuggerPanel>(selectObject);

        EditorExtensionDefinition analysis;
        analysis.id = "lamapon.analysis";
        analysis.displayName = "解析";
        analysis.panels = {
            EditorPanelDefinition{
                std::string{ ProfilerPanelId },
                "プロファイラー",
                false,
                true,
                [this](bool& open)
                {
                    const auto& settings = m_graphics.Settings();
                    const float frameBudget =
                        settings.targetFrameRate > 0
                            ? 1000.0f
                                / static_cast<float>(
                                    settings.targetFrameRate)
                            : 16.6667f;
                    m_profilerPanel->Draw(
                        "プロファイラー",
                        open,
                        DebugCaptureDirectory(L"profiles"),
                        frameBudget);
                },
                AnalysisMenuGroup
            },
            EditorPanelDefinition{
                std::string{ ProfileAnalyzerPanelId },
                "プロファイル分析",
                false,
                true,
                [this](bool& open)
                {
                    m_profileAnalyzerPanel->Draw(
                        "プロファイル分析",
                        open,
                        DebugCaptureDirectory(L"profiles"));
                },
                AnalysisMenuGroup
            },
            EditorPanelDefinition{
                std::string{ MemoryProfilerPanelId },
                "メモリプロファイラー",
                false,
                true,
                [this](bool& open)
                {
                    m_memoryProfilerPanel->Draw(
                        "メモリプロファイラー",
                        open,
                        DebugCaptureDirectory(L"memory"));
                },
                AnalysisMenuGroup
            },
            EditorPanelDefinition{
                std::string{ FrameDebuggerPanelId },
                "フレームデバッガー",
                false,
                true,
                [this](bool& open)
                {
                    m_frameDebuggerPanel->Draw("フレームデバッガー", open);
                },
                AnalysisMenuGroup
            },
            EditorPanelDefinition{
                std::string{ PhysicsDebuggerPanelId },
                "物理デバッガー",
                false,
                true,
                [this](bool& open)
                {
                    m_physicsDebuggerPanel->Draw(
                        "物理デバッガー",
                        open,
                        m_scene,
                        m_selectedObjectId);
                },
                AnalysisMenuGroup
            },
            EditorPanelDefinition{
                std::string{ ImGuiDebuggerPanelId },
                "ImGuiデバッガー",
                false,
                true,
                [](bool& open)
                {
                    // Dear ImGui標準のMetrics/Debuggerです。ウィンドウ、
                    // 描画リスト、ID、入力状態を調べられます。
                    if (open)
                    {
                        ImGui::ShowMetricsWindow(&open);
                    }
                },
                AnalysisMenuGroup
            }
        };
        // パネルを閉じている間は記録を止め、描画や物理の負荷を増やさない
        // ようにします。描画より前（Draw）に毎フレーム同期します。
        analysis.onUpdate = [this]
        {
            m_frameDebuggerPanel->SynchronizeEnabled(
                m_editorExtensions.IsPanelOpen(FrameDebuggerPanelId));
            m_physicsDebuggerPanel->SynchronizeCapture(
                m_scene,
                m_editorExtensions.IsPanelOpen(PhysicsDebuggerPanelId));
        };
        // EditorLayerのデストラクター本体から呼ばれるため、パネルは
        // まだ生存しています。途中で登録に失敗した場合に備えて確認します。
        analysis.onShutdown = [this]
        {
            if (m_frameDebuggerPanel != nullptr)
            {
                m_frameDebuggerPanel->SynchronizeEnabled(false);
            }
            if (m_physicsDebuggerPanel != nullptr)
            {
                m_physicsDebuggerPanel->SynchronizeCapture(
                    m_scene,
                    false);
            }
        };

        std::string error;
        if (!m_editorExtensions.Register(std::move(analysis), &error))
        {
            throw std::logic_error(
                "Failed to register analysis extension: " + error);
        }
    }

    void EditorLayer::DrawAnalysisSceneOverlay()
    {
        if (m_physicsDebuggerPanel != nullptr)
        {
            m_physicsDebuggerPanel->DrawSceneOverlay(
                m_scene,
                m_graphics.Debug(),
                SceneViewMatrix(),
                SceneProjectionMatrix(),
                m_selectedObjectId);
        }
    }
}
