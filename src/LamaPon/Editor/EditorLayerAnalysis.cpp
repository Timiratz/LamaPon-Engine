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


#include <commdlg.h>

#include <array>
#include <stdexcept>
#include <string>
#include <utility>

namespace LamaPon
{
    namespace
    {
        // 解析パネルのメニューグループ
        constexpr const char* AnalysisMenuGroup = "解析";
    }

    // プロジェクト内の解析記録先を返し、資産の基準パスが無ければ空を返します(name: 記録用途のサブフォルダー名)。
    std::filesystem::path EditorLayer::DebugCaptureDirectory(
        const std::wstring_view name) const
    {
        // 現在の資産管理への借用参照
        const auto* assets = m_graphics.TryAssets();
        if (assets == nullptr || assets->AssetRoot().empty())
        {
            return {};
        }
        // エディターログやprofile.jsonと同じく、Gitの追跡対象外の.lamapon配下に置きます。
        return assets->AssetRoot().parent_path()
            / L".lamapon"
            / std::filesystem::path{ std::wstring{ name } };
    }

    // JSON記録を選ぶダイアログを開き、取消や失敗ならnulloptを返します(initialDirectory: 初期表示するフォルダー)。
    std::optional<std::filesystem::path>
        EditorLayer::OpenDebugCaptureDialog(
            const std::filesystem::path& initialDirectory)
    {
        // 選択したファイル名の出力領域
        std::array<wchar_t, 32768> filename{};
        // ダイアログの初期パス文字列
        const std::wstring initial = initialDirectory.wstring();
        // JSON記録のファイル種別指定
        constexpr wchar_t filter[] =
            L"解析の記録 (*.json)\0*.json\0\0";

        // Windowsのファイル選択情報
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

    // 解析パネルを生成し、描画・記録同期・終了処理を登録します。
    void EditorLayer::RegisterAnalysisExtension()
    {
        // 解析パネルの状態通知をエディターへ渡します(message: 通知内容, error: エラー通知か)。
        auto status = [this](std::string message, const bool error)
        {
            SetStatus(std::move(message), error);
        };
        // 解析記録の選択をダイアログへ委譲します(directory: 初期表示するフォルダー)。
        auto openFile = [this](const std::filesystem::path& directory)
        {
            return OpenDebugCaptureDialog(directory);
        };
        // シーン内に存在する対象だけを選択します(id: 選択するオブジェクトID)。
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
        // 描画系からメモリ内訳を取得する解析パネルを生成します。
        m_memoryProfilerPanel = std::make_unique<MemoryProfilerPanel>(
            [this]
            {
                return CaptureMemorySnapshot(m_graphics);
            },
            status,
            openFile);
        // 選択通知と、描画を続けたまま再生更新を停止する処理を渡します。
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

        // 解析パネルの拡張登録情報
        EditorExtensionDefinition analysis;
        analysis.id = "lamapon.analysis";
        analysis.displayName = "解析";
        analysis.panels = {
            // フレーム時間を表示します(open: パネルの表示状態)。
            EditorPanelDefinition{
                std::string{ ProfilerPanelId },
                "プロファイラー",
                false,
                true,
                [this](bool& open)
                {
                    // 描画系のプロジェクト設定
                    const auto& settings = m_graphics.Settings();
                    // 目標フレーム時間・ms
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
            // 時間の集計と比較を表示します(open: パネルの表示状態)。
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
            // メモリ量を表示します(open: パネルの表示状態)。
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
            // 描画イベントを表示します(open: パネルの表示状態)。
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
            // 物理状態を表示します(open: パネルの表示状態)。
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
            // Dear ImGuiの内部状態を表示します(open: パネルの表示状態)。
            EditorPanelDefinition{
                std::string{ ImGuiDebuggerPanelId },
                "ImGuiデバッガー",
                false,
                true,
                [](bool& open)
                {

                    if (open)
                    {
                        ImGui::ShowMetricsWindow(&open);
                    }
                },
                AnalysisMenuGroup
            }
        };

        // パネルの開閉を描画前に毎フレーム同期し、閉じている間の記録を止めます。
        analysis.onUpdate = [this]
        {
            m_frameDebuggerPanel->SynchronizeEnabled(
                m_editorExtensions.IsPanelOpen(FrameDebuggerPanelId));
            m_physicsDebuggerPanel->SynchronizeCapture(
                m_scene,
                m_editorExtensions.IsPanelOpen(PhysicsDebuggerPanelId));
        };

        // EditorLayerの終了時に生存するパネルの記録を止め、登録途中の失敗も考慮します。
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

        // 解析拡張の登録失敗理由
        std::string error;
        if (!m_editorExtensions.Register(std::move(analysis), &error))
        {
            throw std::logic_error(
                "Failed to register analysis extension: " + error);
        }
    }

    // 物理デバッガーがある場合にScene Viewへ補助線を描画します。
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
