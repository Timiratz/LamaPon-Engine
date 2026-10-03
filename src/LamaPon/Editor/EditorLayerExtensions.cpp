#include "LamaPon/Editor/EditorLayer.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Graphics/GraphicsDevice.h"

#include <imgui.h>
#include <shellapi.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace LamaPon
{
    // 標準パネルとパッケージ管理を登録して解析拡張を追加します。
    void EditorLayer::RegisterBuiltInEditorExtensions()
    {
        // 標準パネルの拡張登録情報
        EditorExtensionDefinition workspace;
        workspace.id = "lamapon.workspace";
        workspace.displayName = "標準エディター";
        workspace.panels = {
            // 登録したコンソールパネルを描画します(open: パネルの表示状態)。
            EditorPanelDefinition{
                std::string{ ConsolePanelId },
                "コンソール",
                true,
                true,
                [this](bool& open) { DrawConsole(open); }
            },
            // 登録した性能表示パネルを描画します(open: パネルの表示状態)。
            EditorPanelDefinition{
                std::string{ PerformancePanelId },
                "パフォーマンス",
                false,
                true,
                [this](bool& open) { DrawPerformancePanel(open); }
            },
            // 登録した保存データパネルを描画します(open: パネルの表示状態)。
            EditorPanelDefinition{
                std::string{ PersistencePanelId },
                "セーブデータ",
                false,
                true,
                [this](bool& open) { DrawPersistencePanel(open); }
            },
            // 登録したオンライン診断パネルを描画します(open: パネルの表示状態)。
            EditorPanelDefinition{
                std::string{ OnlineDiagnosticsPanelId },
                "オンライン診断",
                false,
                true,
                [this](bool& open) { DrawOnlineDiagnosticsPanel(open); }
            },
            // 登録したサービス診断パネルを描画します(open: パネルの表示状態)。
            EditorPanelDefinition{
                std::string{ ServiceDiagnosticsPanelId },
                "サービス連携の診断",
                false,
                true,
                [this](bool& open) { DrawServiceDiagnosticsPanel(open); }
            },
            // 登録した資産一覧パネルを描画します(open: パネルの表示状態)。
            EditorPanelDefinition{
                std::string{ AssetBrowserPanelId },
                "アセット",
                true,
                true,
                [this](bool& open) { DrawAssetBrowser(open); }
            },
            // 登録したタイル一覧パネルを描画します(open: パネルの表示状態)。
            EditorPanelDefinition{
                std::string{ TilePalettePanelId },
                "タイルパレット",
                false,
                true,
                [this](bool& open) { DrawTilePalette(open); }
            }
        };


        // パネルを閉じた時や再生停止時にオンライン検索を終了します。
        workspace.onUpdate = [this]
        {
            // 登録済みパネルへの借用参照
            const auto* panel = m_editorExtensions.FindPanel(OnlineDiagnosticsPanelId);
            if (!m_playing || panel == nullptr || !panel->open)
                m_onlineDiagnosticsPanel.StopSearch();
        };

        // 拡張登録の失敗理由
        std::string error;
        if (!m_editorExtensions.Register(std::move(workspace), &error))
        {
            throw std::logic_error(
                "Failed to register built-in editor panels: " + error);
        }

        // パッケージ管理の拡張登録情報
        EditorExtensionDefinition packages;
        packages.id = "lamapon.packages";
        packages.displayName = "パッケージ管理";
        packages.panels = {
            // 登録したパッケージパネルを描画します(open: パネルの表示状態)。
            EditorPanelDefinition{
                std::string{ PackagesPanelId },
                "パッケージ",
                false,
                false,
                [this](bool& open) { DrawPackagesPanel(open); }
            }
        };

        // パネルを閉じていてもUIスレッドでパッケージ処理の完了を回収します。
        packages.onUpdate = [this]
        {
            ConsumePackageWorkerResult();
        };
        // 既存の「拡張機能」メニュー配置を保ちつつ、機能本体はレジストリ経由で追加します。
        packages.menuInline = true;
        // 登録パネルの開閉とパッケージ操作のメニューを描画します。
        packages.drawMenu = [this]
        {
            // パッケージパネルの借用参照
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
                // パッケージのインストール先
                const auto packagesRoot =
                    m_graphics.Assets().AssetRoot()
                    / L"packages";
                // インストール先作成の失敗状態
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

        RegisterAnalysisExtension();
    }

    // グループ無しのパネルを先に、グループ付きは登録順のサブメニューに並べます。
    void EditorLayer::DrawRegisteredPanelMenuItems()
    {

        // 登録順のメニューグループ名
        std::vector<std::string_view> groups;
        // 登録済みパネルへの借用参照
        for (auto& panel : m_editorExtensions.Panels())
        {
            if (!panel.showInWindowMenu)
            {
                continue;
            }
            if (!panel.windowMenuGroup.empty())
            {
                if (std::ranges::find(groups, panel.windowMenuGroup)
                    == groups.end())
                {
                    groups.push_back(panel.windowMenuGroup);
                }
                continue;
            }
            ImGui::MenuItem(
                panel.displayName.c_str(),
                nullptr,
                &panel.open);
        }
        // 描画中のメニューグループ
        for (const auto group : groups)
        {
            // サブメニューの表示名
            const std::string label{ group };
            if (!ImGui::BeginMenu(label.c_str()))
            {
                continue;
            }
            // 登録済みパネルへの借用参照
            for (auto& panel : m_editorExtensions.Panels())
            {
                if (panel.showInWindowMenu
                    && panel.windowMenuGroup == group)
                {
                    ImGui::MenuItem(
                        panel.displayName.c_str(),
                        nullptr,
                        &panel.open);
                }
            }
            ImGui::EndMenu();
        }
    }

    // 拡張ごとのインラインまたはサブメニューを登録順に描画します。
    void EditorLayer::DrawRegisteredExtensionMenuItems()
    {
        // 前の拡張メニューを描画済みか
        bool drewMenu{};
        // 登録済み拡張のメニュー情報
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

    // 拡張レジストリに登録した表示中のパネルを描画します。
    void EditorLayer::DrawRegisteredPanels()
    {
        m_editorExtensions.DrawPanels();
    }
}
