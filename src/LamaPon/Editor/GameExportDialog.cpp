#include "LamaPon/Editor/GameExportDialog.h"
#include "LamaPon/Editor/GameExporter.h"
#include "LamaPon/Core/PathUtils.h"
#include <Windows.h>
#include <shellapi.h>
#include <imgui.h>
#include <stdexcept>

namespace LamaPon
{
    void GameExportDialog::SetPath(const std::filesystem::path& path)
    {
        const auto utf8 = PathToUtf8(path);
        strncpy_s(m_path.data(), m_path.size(), utf8.c_str(), _TRUNCATE);
    }

    std::filesystem::path GameExportDialog::OutputDirectory() const
    {
        return PathFromUtf8(m_path.data());
    }

    void GameExportDialog::SelectTarget(const GameExportTarget target)
    {
        if (m_web.Running() || target == m_target) return;
        const auto oldDefault = m_projectRoot / L"dist"
            / (m_target == GameExportTarget::Windows ? L"LamaPonGame" : L"LamaPonWeb");
        m_target = target;
        // 手で選んだ出力先を上書きせず、既定の出力先だけを形式に合わせます。
        if (OutputDirectory() == oldDefault)
            SetPath(m_projectRoot / L"dist"
                / (target == GameExportTarget::Windows ? L"LamaPonGame" : L"LamaPonWeb"));
        m_error.clear();
        m_success.clear();
        m_completedOutput.clear();
    }

    void GameExportDialog::Open(const std::filesystem::path& projectRoot)
    {
        if (!m_web.Running())
        {
            m_projectRoot = projectRoot;
            SetPath(projectRoot / L"dist"
                / (m_target == GameExportTarget::Windows ? L"LamaPonGame" : L"LamaPonWeb"));
            m_error.clear();
            m_success.clear();
            m_completedOutput.clear();
            try
            {
                const auto tools = LoadWebExportTools();
                strncpy_s(m_python.data(), m_python.size(), PathToUtf8(tools.python).c_str(), _TRUNCATE);
                strncpy_s(m_emsdk.data(), m_emsdk.size(), PathToUtf8(tools.emsdk).c_str(), _TRUNCATE);
            }
            catch (const std::exception& error) { m_error = error.what(); }
        }
        m_requested = true;
    }

    void GameExportDialog::Start(const GameExportDialogContext& context)
    {
        try
        {
            m_error.clear();
            m_success.clear();
            m_completedOutput.clear();
            if (m_path[0] == '\0') throw std::runtime_error("出力先フォルダーを指定してください。");
            context.prepareScene();
            if (m_target == GameExportTarget::Web)
            {
                const WebExportTools tools{PathFromUtf8(m_python.data()), PathFromUtf8(m_emsdk.data())};
                SaveWebExportTools(tools);
                m_web.Start(context.engineRoot, context.projectFile, OutputDirectory(), tools);
                context.setStatus("Web（HTML）のエクスポートを開始しました。", false);
            }
            else
            {
                GameExportOptions options{context.runtimeDirectory, context.assetDirectory,
                    OutputDirectory(), context.settings,
                    m_projectRoot / L".lamapon" / L"bin" / L"LamaPonGameModule.dll"};
                options.createZipArchive = m_createZip;
                const auto result = ExportGamePackage(options);
                m_completedOutput = result.outputDirectory;
                m_success = "Windows（EXE）の出力が完了しました: " + PathToUtf8(result.executablePath);
                if (!result.zipPath.empty()) m_success += " / ZIP: " + PathToUtf8(result.zipPath);
                context.setStatus(m_success, false);
            }
        }
        catch (const std::exception& error)
        {
            m_error = error.what();
            context.setStatus("ゲームのエクスポートに失敗しました: " + m_error, true);
        }
    }

    void GameExportDialog::Draw(const GameExportDialogContext& context)
    {
        if (m_web.Poll())
        {
            if (m_web.Succeeded())
            {
                m_completedOutput = m_web.HtmlPath().parent_path();
                m_success = m_web.Message() + "\n" + PathToUtf8(m_web.HtmlPath());
                context.setStatus(m_success, false);
            }
            else
            {
                m_error = m_web.Message();
                context.setStatus("Web出力に失敗しました: " + m_error, true);
            }
        }
        constexpr auto popup = "ゲームをエクスポート##GameExport";
        if (m_requested) { ImGui::OpenPopup(popup); m_requested = false; }
        ImGui::SetNextWindowSize(ImVec2{740.0f, 0.0f}, ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal(popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 710.0f);
        const bool busy = m_web.Running();
        ImGui::BeginDisabled(busy);
        ImGui::TextUnformatted("出力形式");
        if (ImGui::RadioButton("Windows（EXE）", m_target == GameExportTarget::Windows))
            SelectTarget(GameExportTarget::Windows);
        ImGui::SameLine();
        if (ImGui::RadioButton("Web（HTML）", m_target == GameExportTarget::Web))
            SelectTarget(GameExportTarget::Web);
        ImGui::Spacing();
        ImGui::SetNextItemWidth(550.0f);
        ImGui::InputText("出力先", m_path.data(), m_path.size());
        ImGui::SameLine();
        if (ImGui::Button("参照..."))
        {
            try
            {
                auto initial = OutputDirectory();
                if (!std::filesystem::is_directory(initial)) initial = initial.parent_path();
                if (const auto selected = context.browse(initial)) SetPath(*selected);
            }
            catch (const std::exception& error) { m_error = error.what(); }
        }
        ImGui::Text("ゲーム名: %s", context.settings.gameName.c_str());
        ImGui::Text("初期解像度: %u x %u", context.settings.windowWidth, context.settings.windowHeight);
        ImGui::Text("起動シーン: %s", PathToUtf8(context.settings.startupScene).c_str());
        if (m_target == GameExportTarget::Web)
        {
            ImGui::TextWrapped("ブラウザーで遊べるHTMLを出力します。通常のプロジェクトはゲーム本体とアセットをHTMLにまとめます。");
            ImGui::TextWrapped("Web未対応の機能は出力時に理由を表示します。描画・音声・入力は出力後にブラウザーで確認してください。");
            if (ImGui::CollapsingHeader("Webビルド環境"))
            {
                ImGui::TextWrapped("Emscripten SDK、Python 3.11以降、CMakeが必要です。空欄は環境から自動検出します。この設定はPC内に保存します。");
                ImGui::SetNextItemWidth(510.0f);
                ImGui::InputText("Emscripten SDK", m_emsdk.data(), m_emsdk.size());
                if (ImGui::Button("SDKフォルダーを選択..."))
                {
                    try
                    {
                        if (const auto selected = context.browse(PathFromUtf8(m_emsdk.data())))
                            strncpy_s(m_emsdk.data(), m_emsdk.size(), PathToUtf8(*selected).c_str(), _TRUNCATE);
                    }
                    catch (const std::exception& error) { m_error = error.what(); }
                }
                ImGui::SetNextItemWidth(510.0f);
                ImGui::InputText("Python実行ファイル", m_python.data(), m_python.size());
                ImGui::TextWrapped("SDKを指定すると、同梱Pythonも検索します。設定変更後はエクスポートで再確認できます。");
            }
        }
        else
        {
            ImGui::Text("ゲームアイコン: %s", context.settings.gameIcon.empty()
                ? "（LamaPon標準）" : PathToUtf8(context.settings.gameIcon).c_str());
            ImGui::Text("グラフィック品質: %s / 描画スケール %.2f",
                GraphicsQualityPresetName(context.settings.graphics.preset).data(), context.settings.graphics.renderScale);
            ImGui::TextWrapped("出力物: EXE / LamaPonRuntime.dll / 音声DLL / assets.tpak / 設定ファイル");
            ImGui::Checkbox("配布用ZIPも作成（出力フォルダーの隣に置きます）", &m_createZip);
        }
        ImGui::TextWrapped("既存のパッケージは、出力が成功してから置き換えます。");
        ImGui::EndDisabled();
        if (busy) ImGui::TextWrapped("%s", m_web.Message().c_str());
        if (!m_error.empty())
        {
            ImGui::BeginChild("ExportError", ImVec2{710.0f, 130.0f}, ImGuiChildFlags_Borders);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1.0f, 0.35f, 0.30f, 1.0f});
            ImGui::TextWrapped("%s", m_error.c_str());
            ImGui::PopStyleColor();
            ImGui::EndChild();
        }
        if (!m_success.empty()) ImGui::TextWrapped("%s", m_success.c_str());
        if (m_target == GameExportTarget::Web && !m_web.LogPath().empty()
            && ImGui::Button("ビルドログを開く"))
            ShellExecuteW(nullptr, L"open", m_web.LogPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (!m_completedOutput.empty() && ImGui::Button("出力フォルダーを開く"))
            ShellExecuteW(nullptr, L"open", m_completedOutput.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::Spacing();
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("エクスポート", ImVec2{140.0f, 0.0f})) Start(context);
        ImGui::EndDisabled();
        ImGui::SameLine();
        // 閉じてもジョブはDialogが所有し、毎フレーム結果を回収します。
        if (ImGui::Button(busy ? "閉じて続行" : "閉じる")) ImGui::CloseCurrentPopup();
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
}
