#include "LamaPon/Editor/EditorLayer.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Graphics/GraphicsDevice.h"

#include <imgui.h>
#include <shellapi.h>

#include <stdexcept>
#include <utility>

namespace LamaPon
{
    void EditorLayer::RegisterBuiltInEditorExtensions()
    {
        EditorExtensionDefinition workspace;
        workspace.id = "lamapon.workspace";
        workspace.displayName = "標準エディター";
        workspace.panels = {
            EditorPanelDefinition{
                std::string{ ConsolePanelId },
                "コンソール",
                true,
                true,
                [this](bool& open) { DrawConsole(open); }
            },
            EditorPanelDefinition{
                std::string{ PerformancePanelId },
                "パフォーマンス",
                false,
                true,
                [this](bool& open) { DrawPerformancePanel(open); }
            },
            EditorPanelDefinition{
                std::string{ PersistencePanelId },
                "セーブデータ",
                false,
                true,
                [this](bool& open) { DrawPersistencePanel(open); }
            },
            EditorPanelDefinition{
                std::string{ AssetBrowserPanelId },
                "アセット",
                true,
                true,
                [this](bool& open) { DrawAssetBrowser(open); }
            },
            EditorPanelDefinition{
                std::string{ TilePalettePanelId },
                "タイルパレット",
                false,
                true,
                [this](bool& open) { DrawTilePalette(open); }
            }
        };

        std::string error;
        if (!m_editorExtensions.Register(std::move(workspace), &error))
        {
            throw std::logic_error(
                "Failed to register built-in editor panels: " + error);
        }

        EditorExtensionDefinition packages;
        packages.id = "lamapon.packages";
        packages.displayName = "パッケージ管理";
        packages.panels = {
            EditorPanelDefinition{
                std::string{ PackagesPanelId },
                "パッケージ",
                false,
                false,
                [this](bool& open) { DrawPackagesPanel(open); }
            }
        };
        // パネルを閉じていても、完了した取得・インストール処理は
        // UIスレッドで回収します。
        packages.onUpdate = [this]
        {
            ConsumePackageWorkerResult();
        };
        // 既存の「拡張機能」メニュー配置を保ちつつ、機能本体は
        // レジストリ経由で追加します。
        packages.menuInline = true;
        packages.drawMenu = [this]
        {
            auto* const packagePanel =
                m_editorExtensions.FindPanel(PackagesPanelId);
            if (packagePanel == nullptr)
            {
                return;
            }
            if (ImGui::MenuItem(
                    "パッケージを探す...",
                    nullptr,
                    &packagePanel->open))
            {
                if (packagePanel->open)
                {
                    SetStatus(
                        "公式パッケージの一覧を取得しています");
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem(
                    "Zipから読み込む...",
                    nullptr,
                    false,
                    !m_playing))
            {
                ImportPackageFromZipDialog();
            }
            if (ImGui::MenuItem(
                    "パッケージを作成...",
                    nullptr,
                    false,
                    !m_playing))
            {
                OpenPackageBuildDialog();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("インストール先を開く"))
            {
                const auto packagesRoot =
                    m_graphics.Assets().AssetRoot()
                    / L"packages";
                std::error_code createError;
                std::filesystem::create_directories(
                    packagesRoot,
                    createError);
                ShellExecuteW(
                    m_window,
                    L"open",
                    packagesRoot.c_str(),
                    nullptr,
                    nullptr,
                    SW_SHOWNORMAL);
            }
        };
        if (!m_editorExtensions.Register(std::move(packages), &error))
        {
            throw std::logic_error(
                "Failed to register package extension: " + error);
        }
    }

    void EditorLayer::DrawRegisteredPanelMenuItems()
    {
        for (auto& panel : m_editorExtensions.Panels())
        {
            if (panel.showInWindowMenu)
            {
                ImGui::MenuItem(
                    panel.displayName.c_str(),
                    nullptr,
                    &panel.open);
            }
        }
    }

    void EditorLayer::DrawRegisteredExtensionMenuItems()
    {
        bool drewMenu{};
        for (const auto& extension :
            m_editorExtensions.Extensions())
        {
            if (!extension.drawMenu)
            {
                continue;
            }
            if (drewMenu)
            {
                ImGui::Separator();
            }
            if (extension.menuInline)
            {
                extension.drawMenu();
            }
            else if (ImGui::BeginMenu(
                extension.displayName.c_str()))
            {
                extension.drawMenu();
                ImGui::EndMenu();
            }
            drewMenu = true;
        }
    }

    void EditorLayer::DrawRegisteredPanels()
    {
        m_editorExtensions.DrawPanels();
    }
}
