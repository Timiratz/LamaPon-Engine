#include "LamaPon/Editor/GameExportDialog.h"
#include "LamaPon/Editor/GameExporter.h"
#include "LamaPon/Core/PathUtils.h"
#include <Windows.h>
#include <shellapi.h>
#include <imgui.h>
#include <stdexcept>
#include <utility>

namespace
{
    const wchar_t* OutputName(const LamaPon::GameExportTarget target)
    {
        switch (target)
        {
        case LamaPon::GameExportTarget::Windows: return L"LamaPonGame";
        case LamaPon::GameExportTarget::Web: return L"LamaPonWeb";
        case LamaPon::GameExportTarget::LinuxBuildProject: return L"LamaPonLinuxBuild";
        case LamaPon::GameExportTarget::AndroidBuildProject: return L"LamaPonAndroidBuild";
        case LamaPon::GameExportTarget::AndroidApk: return L"LamaPonAndroidApk";
        }
        throw std::invalid_argument("Unknown game export target");
    }
    bool IsNativeBuildProject(const LamaPon::GameExportTarget target)
    {
        return target == LamaPon::GameExportTarget::LinuxBuildProject
            || target == LamaPon::GameExportTarget::AndroidBuildProject;
    }
}

namespace LamaPon
{
    void GameExportDialog::SetPath(const std::filesystem::path& path)
    {
        // 編集バッファに移す出力パス
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
        // 切替前の形式の既定出力先
        const auto oldDefault = m_projectRoot / L"dist"
            / OutputName(m_target);
        m_target = target;
        // 手で選んだ出力先を上書きせず、既定の出力先だけを形式に合わせます。
        if (OutputDirectory() == oldDefault)
            SetPath(m_projectRoot / L"dist"
                / OutputName(target));
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
                / OutputName(m_target));
            m_error.clear();
            m_success.clear();
            m_completedOutput.clear();
            try
            {
                // 読込または入力したWeb出力ツール
                const auto tools = LoadWebExportTools();
                strncpy_s(m_python.data(), m_python.size(), PathToUtf8(tools.python).c_str(), _TRUNCATE);
                strncpy_s(m_emsdk.data(), m_emsdk.size(), PathToUtf8(tools.emsdk).c_str(), _TRUNCATE);
            }
            // 設定読込またはフォルダー参照の失敗理由
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
            // 配布物に開発用HTTP設定を混ぜないようWindows・Webとも配布設定として検証する。
            ValidateProjectSettings(
                context.settings,
                ProjectSettingsFileType::GamePackage);
            context.prepareScene();
            if (m_target == GameExportTarget::Web)
            {
                // 読込または入力したWeb出力ツール
                const WebExportTools tools{PathFromUtf8(m_python.data()), PathFromUtf8(m_emsdk.data())};
                SaveWebExportTools(tools);
                m_web.Start(context.engineRoot, context.projectFile, OutputDirectory(), tools);
                context.setStatus("Web（HTML）のエクスポートを開始しました。", false);
            }
            else if (m_target == GameExportTarget::AndroidApk)
            {
                const WebExportTools tools{PathFromUtf8(m_python.data()), {}};
                const AndroidExportTools android{PathFromUtf8(m_androidSdk.data()), PathFromUtf8(m_javaHome.data()),
                    PathFromUtf8(m_gradleHome.data()), PathFromUtf8(m_sdlSource.data()), m_androidDownloads};
                m_web.StartAndroidApk(context.engineRoot, context.projectFile, OutputDirectory(), tools, android);
                context.setStatus("Android debug APKのビルドを開始しました。", false);
            }
            else if (m_target == GameExportTarget::LinuxBuildProject && m_buildLinuxWithWsl)
            {
                const WebExportTools tools{PathFromUtf8(m_python.data()), {}};
                const LinuxExportTools linux{m_wslDistribution.data(), PathFromUtf8(m_sdlSource.data())};
                m_web.StartLinuxBuild(context.engineRoot, context.projectFile, OutputDirectory(), tools, linux);
                context.setStatus("WSLでLinuxゲームのビルドを開始しました。", false);
            }
            else if (IsNativeBuildProject(m_target))
            {
                const WebExportTools tools{PathFromUtf8(m_python.data()), {}};
                m_web.StartNativeBuildProject(context.engineRoot, context.projectFile,
                    OutputDirectory(), tools,
                    m_target == GameExportTarget::LinuxBuildProject ? "linux" : "android");
                context.setStatus("ネイティブ出力の診断とビルド設定生成を開始しました。", false);
            }
            else
            {
                // Windows配布物の出力と署名設定
                GameExportOptions options{context.runtimeDirectory, context.assetDirectory,
                    OutputDirectory(), context.settings,
                    m_projectRoot / L".lamapon" / L"bin" / L"LamaPonGameModule.dll"};
                options.createZipArchive = m_createZip;
                options.signing.enabled = m_signWindowsBinaries;
                if (m_signWindowsBinaries)
                {
                    options.signing.signToolPath =
                        PathFromUtf8(m_signTool.data());
                    options.signing.certificateSha1 =
                        m_signingCertificate.data();
                    options.signing.timestampUrl =
                        m_timestampUrl.data();
                }
                // Windowsパッケージの出力結果
                const auto result = ExportGamePackage(options);
                m_completedOutput = result.outputDirectory;
                m_success = "Windows（EXE）の出力が完了しました: " + PathToUtf8(result.executablePath);
                if (m_signWindowsBinaries) m_success += " / EXE・DLL署名済み";
                if (!result.zipPath.empty()) m_success += " / ZIP: " + PathToUtf8(result.zipPath);
                context.setStatus(m_success, false);
            }
        }
        // 出力準備または配布物生成の失敗理由
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
                m_completedOutput = !m_web.NativeArtifactPath().empty() ? m_web.NativeArtifactPath().parent_path()
                    : IsNativeBuildProject(m_target)
                    ? m_web.BuildProjectDirectory() : m_web.HtmlPath().parent_path();
                m_success = m_web.Message() + "\n" + PathToUtf8(m_target == GameExportTarget::AndroidApk
                    || !m_web.NativeArtifactPath().empty() ? m_web.NativeArtifactPath() : IsNativeBuildProject(m_target)
                    ? m_web.BuildProjectDirectory() : m_web.HtmlPath());
                context.setStatus(m_success, false);
            }
            else
            {
                m_error = m_web.Message();
                context.setStatus("ゲーム出力に失敗しました: " + m_error, true);
            }
        }
        // 出力ダイアログのImGui ID
        constexpr auto popup = "ゲームをエクスポート##GameExport";
        if (m_requested) { ImGui::OpenPopup(popup); m_requested = false; }
        ImGui::SetNextWindowSize(ImVec2{740.0f, 0.0f}, ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal(popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 710.0f);
        // Web出力を実行中か
        const bool busy = m_web.Running();
        ImGui::BeginDisabled(busy);
        ImGui::TextUnformatted("出力形式");
        if (ImGui::RadioButton("Windows（EXE）", m_target == GameExportTarget::Windows))
            SelectTarget(GameExportTarget::Windows);
        ImGui::SameLine();
        if (ImGui::RadioButton("Web（HTML）", m_target == GameExportTarget::Web))
            SelectTarget(GameExportTarget::Web);
        if (ImGui::RadioButton("Linux／Steam Deck（ビルド設定）", m_target == GameExportTarget::LinuxBuildProject))
            SelectTarget(GameExportTarget::LinuxBuildProject);
        ImGui::SameLine();
        if (ImGui::RadioButton("Android（ビルド設定）", m_target == GameExportTarget::AndroidBuildProject))
            SelectTarget(GameExportTarget::AndroidBuildProject);
        if (ImGui::RadioButton("Android（debug APK）", m_target == GameExportTarget::AndroidApk))
            SelectTarget(GameExportTarget::AndroidApk);
        ImGui::Spacing();
        ImGui::SetNextItemWidth(550.0f);
        ImGui::InputText("出力先", m_path.data(), m_path.size());
        ImGui::SameLine();
        if (ImGui::Button("参照..."))
        {
            try
            {
                // フォルダー参照の初期表示先
                auto initial = OutputDirectory();
                if (!std::filesystem::is_directory(initial)) initial = initial.parent_path();
                // 選択した出力先またはSDKパス
                if (const auto selected = context.browse(initial)) SetPath(*selected);
            }
            // 設定読込またはフォルダー参照の失敗理由
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
                        // 選択した出力先またはSDKパス
                        if (const auto selected = context.browse(PathFromUtf8(m_emsdk.data())))
                            strncpy_s(m_emsdk.data(), m_emsdk.size(), PathToUtf8(*selected).c_str(), _TRUNCATE);
                    }
                    // SDKフォルダー参照の失敗理由
                    catch (const std::exception& error) { m_error = error.what(); }
                }
                ImGui::SetNextItemWidth(510.0f);
                ImGui::InputText("Python実行ファイル", m_python.data(), m_python.size());
                ImGui::TextWrapped("SDKを指定すると、同梱Pythonも検索します。設定変更後はエクスポートで再確認できます。");
            }
        }
        else if (m_target == GameExportTarget::AndroidApk)
        {
            ImGui::TextWrapped("既存のAndroidビルド環境を使い、端末で確認するためのdebug APKを作成します。ストア配布用の署名は別途必要です。");
            ImGui::TextWrapped("SDKは自動取得しません。新しいフォルダーまたは空のフォルダーを指定してください。ビルド生成物・キャッシュは出力先のbuildに置きます。");
            ImGui::TextWrapped("SDK Platform・NDK・CMake・Build Toolsは事前に用意してください。不足パッケージ名は出力ログに表示します。");
            for (const auto& item : {std::pair{"Android SDK", &m_androidSdk}, {"JDK", &m_javaHome},
                {"Gradle", &m_gradleHome}, {"SDL3ソース", &m_sdlSource}, {"Python実行ファイル", &m_python}})
            {
                ImGui::SetNextItemWidth(510.0f);
                ImGui::InputText(item.first, item.second->data(), item.second->size());
            }
            ImGui::Checkbox("Gradleのビルド依存を取得する（キャッシュに保存）", &m_androidDownloads);
            ImGui::TextWrapped("オフの場合は取得済みの依存だけを使います。出力後に端末で動作を確認してください。");
        }
        else if (IsNativeBuildProject(m_target))
        {
            ImGui::TextWrapped("Portable対応を検査し、CMake／Gradleのビルド設定を出力します。エンジンとゲームの既存ソースを参照します。");
            ImGui::TextWrapped("LinuxはLinux環境とSDL3、AndroidはSDK・NDK・JDK・GradleとSDL3が必要です。");
            ImGui::TextWrapped("新しいフォルダーまたは空のフォルダーを指定してください。詳細設定はプロジェクトのexport.nativeに保存します。");
            ImGui::SetNextItemWidth(510.0f);
            ImGui::InputText("Python実行ファイル", m_python.data(), m_python.size());
            if (m_target == GameExportTarget::LinuxBuildProject)
            {
                ImGui::Spacing();
                ImGui::Checkbox("既存のWSL内でLinuxゲームまでビルドする", &m_buildLinuxWithWsl);
                if (m_buildLinuxWithWsl)
                {
                    ImGui::TextWrapped("WSLディストリビューションにPython 3.11以降、CMake 3.25以降、C++20コンパイラーとLinux用SDL3依存が必要です。インストールやダウンロードは行いません。");
                    ImGui::SetNextItemWidth(510.0f);
                    ImGui::InputText("WSLディストリビューション（空欄は既定）", m_wslDistribution.data(), m_wslDistribution.size());
                    ImGui::SetNextItemWidth(510.0f);
                    ImGui::InputText("SDL3ソース（Windowsから参照可能なパス）", m_sdlSource.data(), m_sdlSource.size());
                    if (ImGui::Button("SDL3ソースを選択..."))
                    {
                        try
                        {
                            if (const auto selected = context.browse(PathFromUtf8(m_sdlSource.data())))
                                strncpy_s(m_sdlSource.data(), m_sdlSource.size(), PathToUtf8(*selected).c_str(), _TRUNCATE);
                        }
                        catch (const std::exception& error) { m_error = error.what(); }
                    }
                    ImGui::TextWrapped("WSLからWindowsドライブへアクセスできる必要があります。ビルド後のゲームはLinux／Steam Deck上で別途確認してください。");
                }
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
            ImGui::Checkbox("自分のコード署名証明書でEXE/DLLに署名", &m_signWindowsBinaries);
            if (m_signWindowsBinaries)
            {
                ImGui::TextWrapped("Windows SDKのSignToolと、現在のユーザーの証明書ストアにあるコード署名証明書を使います。設定はプロジェクトに保存しません。");
                ImGui::SetNextItemWidth(510.0f);
                ImGui::InputText("signtool.exeの絶対パス", m_signTool.data(), m_signTool.size());
                ImGui::SetNextItemWidth(510.0f);
                ImGui::InputText("証明書のSHA-1拇印（40桁）", m_signingCertificate.data(), m_signingCertificate.size());
                ImGui::SetNextItemWidth(510.0f);
                ImGui::InputText("RFC 3161タイムスタンプURL（HTTPS）", m_timestampUrl.data(), m_timestampUrl.size());
            }
        }
        if (!IsNativeBuildProject(m_target) && m_target != GameExportTarget::AndroidApk)
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
        if (m_target != GameExportTarget::Windows && !m_web.LogPath().empty()
            && ImGui::Button("ビルドログを開く"))
            ShellExecuteW(nullptr, L"open", m_web.LogPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (!m_completedOutput.empty() && ImGui::Button("出力フォルダーを開く"))
            ShellExecuteW(nullptr, L"open", m_completedOutput.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::Spacing();
        ImGui::BeginDisabled(busy);
        if (ImGui::Button(m_target == GameExportTarget::AndroidApk ? "debug APKをビルド"
            : m_target == GameExportTarget::LinuxBuildProject && m_buildLinuxWithWsl ? "Linuxゲームをビルド"
            : IsNativeBuildProject(m_target) ? "ビルド設定を生成" : "エクスポート", ImVec2{160.0f, 0.0f})) Start(context);
        ImGui::EndDisabled();
        ImGui::SameLine();
        // 閉じてもジョブはDialogが所有し、毎フレーム結果を回収します。
        if (ImGui::Button(busy ? "閉じて続行" : "閉じる")) ImGui::CloseCurrentPopup();
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
}
