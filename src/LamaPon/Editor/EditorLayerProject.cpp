#include "LamaPon/Editor/EditorLayer.h"

#include "LamaPon/Editor/EditorLayerShared.h"
#include "LamaPon/Editor/SceneLoadingScreenEditor.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Audio/AudioSystem.h"
#include "LamaPon/Components/ParticleSystemComponent.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Physics/PhysicsSettings.h"
#include "LamaPon/Core/Time.h"
#include "LamaPon/Editor/GameExportDialog.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Online/OnlineServices.h"
#include "LamaPon/Online/NetworkSession.h"
#include "LamaPon/Online/NetworkSceneBridge.h"
#include "LamaPon/Scene/Scene.h"
#include "LamaPon/Scene/SceneManager.h"

#include <imgui.h>
#include <commdlg.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>

using namespace LamaPon::EditorDetail;

namespace
{
    // 初期化時に指定フォルダーを選択します(dialog: フォルダー選択ダイアログ, message: ダイアログの通知種別, userData: 初期パス文字列の借用ポインター)。
    int CALLBACK BrowseForExportCallback(
        const HWND dialog,
        const UINT message,
        const LPARAM,
        const LPARAM userData)
    {
        if (message == BFFM_INITIALIZED && userData != 0)
        {
            SendMessageW(
                dialog,
                BFFM_SETSELECTIONW,
                TRUE,
                userData);
        }
        return 0;
    }

    // 出力先を選び、取消ならnullopt、パス取得失敗は例外を投げます(owner: ダイアログの親ウィンドウ, initialDirectory: 初期選択するフォルダー)。
    std::optional<std::filesystem::path> BrowseForExportDirectory(
        const HWND owner,
        const std::filesystem::path& initialDirectory)
    {
        // 選択フォルダーの表示名出力
        std::array<wchar_t, MAX_PATH> displayName{};
        // ダイアログの初期選択パス
        const std::wstring initialPath =
            initialDirectory.wstring();

        // Windowsのフォルダー選択情報
        BROWSEINFOW browse{};
        browse.hwndOwner = owner;
        browse.pszDisplayName = displayName.data();
        browse.lpszTitle = L"ゲームの出力先フォルダーを選択";
        browse.ulFlags =
            BIF_RETURNONLYFSDIRS
            | BIF_NEWDIALOGSTYLE;
        browse.lpfn = BrowseForExportCallback;
        browse.lParam = reinterpret_cast<LPARAM>(
            initialPath.c_str());

        // 選択された項目の所有PIDL
        const PIDLIST_ABSOLUTE item =
            SHBrowseForFolderW(&browse);
        if (item == nullptr)
        {
            return std::nullopt;
        }

        // 選択フォルダーのパス出力領域
        std::array<wchar_t, MAX_PATH> selectedPath{};
        // 選択項目をパスへ変換できたか
        const bool pathRead =
            SHGetPathFromIDListW(
                item,
                selectedPath.data()) != FALSE;
        CoTaskMemFree(item);
        if (!pathRead)
        {
            throw std::runtime_error(
                "選択した出力先を読み取れませんでした");
        }
        return std::filesystem::path(
            selectedPath.data());
    }
}

namespace
{

    // 更新時刻に依存せず設定ファイルの内容をハッシュ化します(path: 読込対象の設定パス, readable: 開けたかの出力先)。
    std::uint64_t HashProjectSettingsFile(
        const std::filesystem::path& path,
        bool& readable)
    {
        readable = false;
        // 外部変更検出用の設定ファイル入力
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            return 0;
        }
        // 外部変更検出用の内容ハッシュ
        std::uint64_t hash = 1469598103934665603ull;
        // 設定ファイルの読込バッファ
        char buffer[4096];
        while (stream.read(buffer, sizeof(buffer))
            || stream.gcount() > 0)
        {
            // 読み取った設定のバイト数
            const std::streamsize count = stream.gcount();
            // ハッシュへ加えるバイトの添字
            for (std::streamsize index = 0; index < count; ++index)
            {
                hash ^= static_cast<unsigned char>(buffer[index]);
                hash *= 1099511628211ull;
            }
        }
        readable = true;
        return hash;
    }
}

namespace LamaPon
{
    // プロジェクトの管理ディレクトリ内にある設定JSONの保存パスを返します。
    std::filesystem::path EditorLayer::ProjectSettingsPath() const
    {
        return m_graphics.Assets().AssetRoot().parent_path()
            / L".lamapon"
            / L"project.json";
    }

    // 編集可能な間に二秒ごとに内容変更を検出し、入力・描画・タグへ反映します。
    void EditorLayer::UpdateExternalProjectSettings()
    {
        // 入力状態と編集中の下書きを守るため、ビルド・再生・設定ダイアログ中は基準ハッシュを変えず監視を止めます。
        if (m_gameModuleBuildProcess != nullptr
            || m_playing
            || ImGui::IsPopupOpen(
                "プロジェクト設定とビルド##ProjectSettings"))
        {
            return;
        }
        // 今回の監視時刻・秒
        const double now = ImGui::GetTime();
        if (now - m_lastProjectSettingsScanAt < 2.0)
        {
            return;
        }
        m_lastProjectSettingsScanAt = now;

        // プロジェクト設定JSONのパス
        const auto path = ProjectSettingsPath();
        // 設定ファイルを開けたか
        bool readable = false;
        // 外部変更検出用の内容ハッシュ
        const std::uint64_t hash =
            HashProjectSettingsFile(path, readable);
        if (!readable)
        {
            // 読めない間は基準ハッシュを変えず、次の走査で再試行します。
            return;
        }
        if (!m_projectSettingsHashInitialized)
        {
            m_projectSettingsSeenHash = hash;
            m_projectSettingsHashInitialized = true;
            return;
        }
        if (hash == m_projectSettingsSeenHash)
        {
            return;
        }
        // 読込に失敗しても同じ内容を再試行せず、次の内容変更を待ちます。
        m_projectSettingsSeenHash = hash;

        try
        {
            m_projectSettings = LamaPon::LoadProjectSettings(path);
            // 外部変更では入力・描画・タグを反映し、物理刻みは起動時の状態を保ちます。
            m_graphics.Input().SetActions(
                m_projectSettings.inputActions);
            m_graphics.SetGraphicsSettings(
                m_projectSettings.graphics);
            m_scene.SetRegisteredTags(
                m_projectSettings.tags);
            SetStatus(
                "プロジェクト設定の外部変更を再読み込みしました");
        }
        // 外部変更の適用失敗を通知します(error: 適用の失敗理由)。
        catch (const std::exception& error)
        {
            SetStatus(
                std::string("プロジェクト設定の再読み込みに失敗: ")
                    + error.what(),
                true);
        }
    }

    // プロジェクト設定を物理も含めて反映し、外部変更検出の基準を更新します。
    bool EditorLayer::LoadProjectConfiguration()
    {
        // プロジェクト設定JSONのパス
        const auto path = ProjectSettingsPath();
        if (!std::filesystem::exists(path))
        {
            return false;
        }
        m_projectSettings =
            LamaPon::LoadProjectSettings(path);
        m_graphics.Input().SetActions(
            m_projectSettings.inputActions);
        m_graphics.SetGraphicsSettings(
            m_projectSettings.graphics);
        LamaPon::SetActivePhysicsSettings(
            m_projectSettings.physics);
        m_scene.SetRegisteredTags(
            m_projectSettings.tags);
        // 外部変更検知の基準ハッシュを読み込んだ内容に合わせる
        {
            // 設定ファイルを開けたか
            bool readable = false;
            m_projectSettingsSeenHash =
                HashProjectSettingsFile(path, readable);
            m_projectSettingsHashInitialized = readable;
        }
        return true;
    }

    // 設定を保存し、自身の保存を外部変更として扱わないよう基準を更新します。
    void EditorLayer::SaveProjectConfiguration() const
    {
        LamaPon::SaveProjectSettings(
            ProjectSettingsPath(),
            m_projectSettings,
            ProjectSettingsFileType::Project);
        // 自分の保存を外部変更として誤検知しないよう基準を更新する
        // 設定ファイルを開けたか
        bool readable = false;
        m_projectSettingsSeenHash = HashProjectSettingsFile(
            ProjectSettingsPath(),
            readable);
        m_projectSettingsHashInitialized = readable;
    }

    // タグを正規化して保存し、重複なら成功、保存失敗なら登録を取り消します(tag: 登録する64byte以下のタグ名)。
    bool EditorLayer::AddProjectTag(std::string tag)
    {
        // 前後の空白を除去してから登録します。
        // タグ名の先頭の非空白位置
        const auto first =
            tag.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
        {
            SetStatus("タグ名が空です", true);
            return false;
        }
        // タグ名の末尾の非空白位置
        const auto last =
            tag.find_last_not_of(" \t\r\n");
        tag = tag.substr(first, last - first + 1);
        if (tag.size() > 64)
        {
            SetStatus(
                "タグ名は64バイト以内にしてください",
                true);
            return false;
        }
        if (std::ranges::find(
                m_projectSettings.tags,
                tag)
            != m_projectSettings.tags.end())
        {
            return true;
        }

        m_projectSettings.tags.push_back(tag);
        try
        {
            SaveProjectConfiguration();
        }
        // タグ保存の失敗時に登録を取り消します(exception: 保存の失敗理由)。
        catch (const std::exception& exception)
        {
            m_projectSettings.tags.pop_back();
            SetStatus(
                "タグを保存できませんでした: "
                + std::string{ exception.what() },
                true);
            return false;
        }
        m_scene.SetRegisteredTags(
            m_projectSettings.tags);
        SetStatus("タグ「" + tag + "」を登録しました");
        return true;
    }

    // 保存済み設定を編集用のdraftへ複製して設定画面を開きます。
    void EditorLayer::OpenProjectSettingsDialog()
    {
        strncpy_s(
            m_projectGameNameBuffer.data(),
            m_projectGameNameBuffer.size(),
            m_projectSettings.gameName.c_str(),
            _TRUNCATE);
        // 起動シーンの相対パス
        const std::string startupScene =
            PathToUtf8(m_projectSettings.startupScene);
        strncpy_s(
            m_projectStartupSceneBuffer.data(),
            m_projectStartupSceneBuffer.size(),
            startupScene.c_str(),
            _TRUNCATE);
        // アイコンの相対パス
        const std::string gameIcon =
            PathToUtf8(m_projectSettings.gameIcon);
        strncpy_s(
            m_projectGameIconBuffer.data(),
            m_projectGameIconBuffer.size(),
            gameIcon.c_str(),
            _TRUNCATE);
        m_projectWindowSize = {
            static_cast<int>(m_projectSettings.windowWidth),
            static_cast<int>(m_projectSettings.windowHeight)
        };
        m_projectSplashScreenDraft =
            m_projectSettings.splashScreenEnabled;
        m_projectLoadingScreenDraft =
            m_projectSettings.loadingScreen;
        m_projectGraphicsDraft =
            m_projectSettings.graphics;
        m_projectViewportDraft =
            m_projectSettings.viewport;
        m_projectPhysicsDraft =
            m_projectSettings.physics;
        m_projectInputActionsDraft =
            m_projectSettings.inputActions;
        m_projectTagsDraft = m_projectSettings.tags;
        m_projectNewTagBuffer.fill('\0');
        m_projectScriptEditorDraft =
            m_projectSettings.scriptEditorPath;
        m_projectAutoBuildDraft =
            m_projectSettings.autoBuildGameModuleOnSave;
        m_projectStripShaderSourceDraft =
            m_projectSettings.stripShaderSourceOnExport;
        m_projectInspectorDecimalsDraft =
            static_cast<int>(
                m_projectSettings.inspectorDecimals);
        m_projectNetworkDraft = m_projectSettings.network;
        m_projectOnlineDraft = m_projectSettings.online;
        strncpy_s(
            m_projectOnlineServiceBaseUrlBuffer.data(),
            m_projectOnlineServiceBaseUrlBuffer.size(),
            m_projectSettings.online.serviceBaseUrl.c_str(),
            _TRUNCATE);
        strncpy_s(
            m_projectOnlineGameIdBuffer.data(),
            m_projectOnlineGameIdBuffer.size(),
            m_projectSettings.online.gameId.c_str(),
            _TRUNCATE);
        strncpy_s(
            m_projectOnlineEnvironmentIdBuffer.data(),
            m_projectOnlineEnvironmentIdBuffer.size(),
            m_projectSettings.online.environmentId.c_str(),
            _TRUNCATE);
        strncpy_s(
            m_projectDiscordPresenceApplicationIdBuffer.data(),
            m_projectDiscordPresenceApplicationIdBuffer.size(),
            m_projectSettings.online.discordPresence
                .applicationId.c_str(),
            _TRUNCATE);
        strncpy_s(
            m_projectDiscordPresenceImageKeyBuffer.data(),
            m_projectDiscordPresenceImageKeyBuffer.size(),
            m_projectSettings.online.discordPresence
                .defaultLargeImageKey.c_str(),
            _TRUNCATE);
        strncpy_s(
            m_projectDiscordPresenceImageTextBuffer.data(),
            m_projectDiscordPresenceImageTextBuffer.size(),
            m_projectSettings.online.discordPresence
                .defaultLargeImageText.c_str(),
            _TRUNCATE);
        // 開くたびに外部エディター候補を再検出します。
        m_projectScriptEditorOptions = DetectScriptEditors();
        m_projectSettingsError.clear();
        m_projectSettingsDialogRequested = true;
    }

    // 実行ファイルの選択結果を外部エディターのdraftへ設定し、キャンセル時は維持します。
    void EditorLayer::BrowseForScriptEditor()
    {
        // 選択した実行ファイルのパス
        std::array<wchar_t, 1024> selectedFile{};
        // 実行ファイルの選択フィルター
        constexpr wchar_t filter[] =
            L"実行可能ファイル (*.exe)\0*.exe\0"
            L"すべてのファイル (*.*)\0*.*\0\0";

        // 実行ファイルの選択設定
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = m_window;
        dialog.lpstrFilter = filter;
        dialog.nFilterIndex = 1;
        dialog.lpstrFile = selectedFile.data();
        dialog.nMaxFile =
            static_cast<DWORD>(selectedFile.size());
        dialog.lpstrTitle = L"スクリプトエディターを選択";
        dialog.Flags =
            OFN_EXPLORER
            | OFN_FILEMUSTEXIST
            | OFN_PATHMUSTEXIST
            | OFN_NOCHANGEDIR;

        if (GetOpenFileNameW(&dialog))
        {
            m_projectScriptEditorDraft =
                std::filesystem::path(selectedFile.data());
        }
    }

    // 外部エディター・保存時ビルド・出力設定のdraftを編集します。
    void EditorLayer::DrawProjectSettingsScriptingSection()
    {
        ImGui::TextUnformatted("スクリプト");
        ImGui::Separator();
        ImGui::TextWrapped(
            "アセットブラウザーで.cppをダブルクリックしたときに開く"
            "エディターを選べます。");
        ImGui::Spacing();

        // 既定の関連付けを使うか
        const bool useSystemDefault =
            m_projectScriptEditorDraft.empty();
        // 選択中のエディター名
        std::string preview = useSystemDefault
            ? "システムの既定（ファイルの関連付け）"
            : PathToUtf8(m_projectScriptEditorDraft);
        // 外部エディターの候補
        for (const auto& option : m_projectScriptEditorOptions)
        {
            if (option.executablePath
                == m_projectScriptEditorDraft)
            {
                preview = option.label;
                break;
            }
        }

        ImGui::SetNextItemWidth(480.0f);
        if (ImGui::BeginCombo(
            "エディター",
            preview.c_str()))
        {
            if (ImGui::Selectable(
                "システムの既定（ファイルの関連付け）",
                useSystemDefault))
            {
                m_projectScriptEditorDraft.clear();
            }
            for (const auto& option
                : m_projectScriptEditorOptions)
            {
                // 選択中の候補か
                const bool selected =
                    option.executablePath
                    == m_projectScriptEditorDraft;
                if (ImGui::Selectable(
                    option.label.c_str(),
                    selected))
                {
                    m_projectScriptEditorDraft =
                        option.executablePath;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        if (m_projectScriptEditorOptions.empty())
        {
            ImGui::TextDisabled(
                "Visual Studio / Visual Studio Codeが見つかりませんでした。"
                "「参照」から実行ファイルを直接指定できます。");
        }

        ImGui::SameLine();
        if (ImGui::Button("参照..."))
        {
            BrowseForScriptEditor();
        }

        if (!m_projectScriptEditorDraft.empty())
        {
            ImGui::TextDisabled(
                "%s",
                PathToUtf8(m_projectScriptEditorDraft).c_str());
        }

        ImGui::Spacing();
        ImGui::Checkbox(
            "保存したらGame Moduleを自動ビルド",
            &m_projectAutoBuildDraft);
        ImGui::TextDisabled(
            "assets内の.cpp/.hを保存すると、少し待ってから自動で\n"
            "ビルドします。成功すると変更が反映されます。\n"
            "オフにすると、右クリックの「Game Moduleをビルド」だけに\n"
            "なります。再生中は自動ビルドしません。");

        ImGui::Spacing();
        ImGui::Checkbox(
            "書き出しでHLSLソースを外す",
            &m_projectStripShaderSourceDraft);
        ImGui::TextDisabled(
            "配布物にコンパイル済みのバイトコードだけを入れます\n"
            "HLSLの内容を配布したくないときに使います。\n"
            "入れると全バリアントを焼くので、書き出しは少し\n"
            "時間がかかります。配布先で自作Shaderを差し替える\n"
            "余地は無くなります。");

        ImGui::Spacing();
        ImGui::SetNextItemWidth(130.0f);
        if (ImGui::SliderInt(
            "Inspectorの小数点桁数",
            &m_projectInspectorDecimalsDraft,
            0,
            6))
        {
            m_projectInspectorDecimalsDraft = std::clamp(
                m_projectInspectorDecimalsDraft,
                0,
                6);
        }
        ImGui::TextDisabled(
            "Transformの位置・回転・拡縮を何桁まで表示するかです。\n"
            "既定の1は「0.0」表示で、ざっと確認するのに読みやすい\n"
            "桁数です。表示だけを丸めるので、入力した値はそのまま\n"
            "保持されます。");
    }

    // オンライン設定の編集を通信設定セクションへ委譲します。
    void EditorLayer::DrawProjectSettingsOnlineSection()
    {
        ImGui::TextUnformatted("オンライン");
        ImGui::Separator();
        DrawProjectSettingsNetworkSection();
    }

    // アカウント・クラウド・Presenceの設定draftを編集します。
    void EditorLayer::DrawProjectSettingsServicesSection()
    {
        ImGui::TextUnformatted("サービス連携");
        ImGui::Separator();
        ImGui::TextWrapped("外部サービスのアカウント認証・クラウドセーブ・プレイ状況表示を設定します。");
        ImGui::SeparatorText("Discordアカウント連携 / クラウドセーブ");
        ImGui::Checkbox(
            "Discordアカウント連携を有効にする",
            &m_projectOnlineDraft.enabled);
        ImGui::TextWrapped(
            "Discordログインとクラウドセーブには、ゲームから直接Discordへ秘密情報を送るのではなく、認証と保存を担当するLamaPon用バックエンドが必要です。");
        ImGui::Spacing();

        ImGui::SetNextItemWidth(520.0f);
        ImGui::InputText(
            "サービスURL",
            m_projectOnlineServiceBaseUrlBuffer.data(),
            m_projectOnlineServiceBaseUrlBuffer.size());
        ImGui::TextDisabled(
            "例: https://online.example.com （末尾の / は省略可）");

        ImGui::SetNextItemWidth(360.0f);
        ImGui::InputText(
            "ゲームID",
            m_projectOnlineGameIdBuffer.data(),
            m_projectOnlineGameIdBuffer.size());
        ImGui::TextDisabled(
            "名前を変えても変わらないID。英数字と . _ - を使用できます。\n"
            "例: com.example.my-game");

        ImGui::SetNextItemWidth(240.0f);
        ImGui::InputText(
            "環境ID",
            m_projectOnlineEnvironmentIdBuffer.data(),
            m_projectOnlineEnvironmentIdBuffer.size());
        ImGui::TextDisabled(
            "production / staging など。環境ごとにアカウントとセーブを分離します。");

        ImGui::Checkbox(
            "ログイン時に既定ブラウザーを開く",
            &m_projectOnlineDraft.openAuthorizationBrowser);
        ImGui::Checkbox(
            "ローカル開発用HTTPを許可",
            &m_projectOnlineDraft.allowInsecureLoopback);
        ImGui::PushStyleColor(
            ImGuiCol_Text,
            ImVec4{ 1.0f, 0.65f, 0.25f, 1.0f });
        ImGui::TextWrapped(
            "HTTPは127.0.0.1 / localhost / [::1]だけに制限され、オンラインを有効にした配布ビルドでは拒否されます。");
        ImGui::PopStyleColor();

        ImGui::Spacing();
        DrawProjectSettingsDiscordPresenceSection();

        ImGui::Spacing();
        ImGui::SeparatorText("セキュリティ");
        ImGui::PushStyleColor(
            ImGuiCol_Text,
            ImVec4{ 1.0f, 0.45f, 0.35f, 1.0f });
        ImGui::TextWrapped(
            "Discord client_secret、access token、refresh tokenをここやproject.jsonへ置かないでください。");
        ImGui::PopStyleColor();
        ImGui::TextWrapped(
            "client_secretはバックエンドの環境変数またはシークレット管理へ保存します。ゲームに入れると、配布ファイルから誰でも取り出せます。");
    }

    // ログインやクラウド保存と独立したDiscord Presenceのdraftを編集します。
    void EditorLayer::DrawProjectSettingsDiscordPresenceSection()
    {
        ImGui::SeparatorText("Discord Rich Presence");
        ImGui::Checkbox(
            "Discord Rich Presenceを有効にする",
            &m_projectOnlineDraft.discordPresence.enabled);
        ImGui::TextWrapped(
            "プレイ中の状況をDiscordのプロフィールへ表示します。Discordアカウント連携とは独立していて、ログインしていなくても使えます。");

        ImGui::SetNextItemWidth(360.0f);
        ImGui::InputText(
            "Application ID",
            m_projectDiscordPresenceApplicationIdBuffer.data(),
            m_projectDiscordPresenceApplicationIdBuffer.size(),
            ImGuiInputTextFlags_CharsDecimal);
        ImGui::TextDisabled(
            "Discord Developer Portalでゲームごとに作成したApplicationのIDです。\n"
            "公開情報なのでproject.jsonへ保存します。");

        ImGui::SetNextItemWidth(360.0f);
        ImGui::InputText(
            "既定の大画像キー",
            m_projectDiscordPresenceImageKeyBuffer.data(),
            m_projectDiscordPresenceImageKeyBuffer.size());
        ImGui::TextDisabled(
            "Discord Applicationの Rich Presence > Art Assets へ登録した画像名です。\n"
            "例: game_icon");

        ImGui::SetNextItemWidth(360.0f);
        ImGui::InputText(
            "既定の大画像テキスト",
            m_projectDiscordPresenceImageTextBuffer.data(),
            m_projectDiscordPresenceImageTextBuffer.size());
        ImGui::TextDisabled(
            "画像へカーソルを合わせたときの説明です。例: My Awesome Game");

        // アプリIDが未入力か
        const bool applicationIdMissing =
            m_projectDiscordPresenceApplicationIdBuffer[0]
                == '\0';
        if (m_projectOnlineDraft.discordPresence.enabled
            && applicationIdMissing)
        {
            ImGui::PushStyleColor(
                ImGuiCol_Text,
                ImVec4{ 1.0f, 0.65f, 0.25f, 1.0f });
            ImGui::TextWrapped(
                "Application IDが未設定のため、Rich Presenceは実行時に無効化されます（ゲームは通常どおり動きます）。");
            ImGui::PopStyleColor();
        }

        ImGui::TextDisabled("テスト表示と接続状態は「ウィンドウ > サービス連携の診断」で確認できます。");
    }

    // draftの保存成功後に対象別の出力ダイアログを予約します。
    void EditorLayer::DrawProjectSettingsBuildSection()
    {
        ImGui::TextUnformatted("ビルドプロファイル");
        ImGui::Separator();
        ImGui::TextWrapped(
            "プロジェクト設定を確認して、出力先に合うプロファイルを"
            "選びます。「設定して書き出す」を押すと、この画面の変更を"
            "保存してから書き出し設定を開きます。");
        ImGui::Spacing();

        ImGui::Text("ゲーム名: %s", m_projectGameNameBuffer.data());
        ImGui::Text(
            "初期解像度: %d x %d",
            m_projectWindowSize[0],
            m_projectWindowSize[1]);
        ImGui::Text(
            "起動シーン: %s",
            m_projectStartupSceneBuffer.data());
        ImGui::Text(
            "グラフィック品質: %s",
            GraphicsQualityPresetName(
                m_projectGraphicsDraft.preset).data());
        ImGui::Text(
            "配布時のHLSLソース: %s",
            m_projectStripShaderSourceDraft
                ? "除外"
                : "同梱");
        ImGui::Spacing();

        // プロファイル間の余白
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        // プロファイルの表示幅
        const float profileWidth = std::max(
            250.0f,
            (ImGui::GetContentRegionAvail().x - spacing)
                * 0.5f);
        // シーンの保存先があるか
        const bool hasSavedScene = !m_scenePath.empty();

        ImGui::BeginChild(
            "WindowsBuildProfile",
            ImVec2{ profileWidth, 190.0f },
            ImGuiChildFlags_Borders);
        ImGui::TextUnformatted("Windows");
        ImGui::Separator();
        ImGui::TextWrapped(
            "Windows用のEXE、Runtime DLL、暗号化したアセットを"
            "まとめて出力します。");
        ImGui::Spacing();
        ImGui::TextDisabled("出力: dist/LamaPonGame");
        ImGui::TextDisabled("配布用ZIPも作成できます");
        ImGui::Spacing();
        ImGui::BeginDisabled(!hasSavedScene);
        if (ImGui::Button(
            "設定して書き出す##Windows",
            ImVec2{ -1.0f, 0.0f })
            && SaveProjectSettingsDraft())
        {
            RequestGameExportDialog(
                GameExportTarget::Windows);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild(
            "WebBuildProfile",
            ImVec2{ profileWidth, 190.0f },
            ImGuiChildFlags_Borders);
        ImGui::TextUnformatted("Web");
        ImGui::Separator();
        ImGui::TextWrapped(
            "ブラウザー用のHTMLとWebAssemblyを出力し、"
            "未対応機能を事前に検査します。");
        ImGui::Spacing();
        ImGui::TextDisabled("出力: dist/LamaPonWeb");
        ImGui::TextDisabled("環境: Emscripten / Python / CMake");
        ImGui::Spacing();
        ImGui::BeginDisabled(!hasSavedScene);
        if (ImGui::Button(
            "設定して書き出す##Web",
            ImVec2{ -1.0f, 0.0f })
            && SaveProjectSettingsDraft())
        {
            RequestGameExportDialog(
                GameExportTarget::Web);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::EndChild();

        if (!hasSavedScene)
        {
            ImGui::Spacing();
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.65f, 0.25f, 1.0f },
                "ビルドする前にシーンを保存してください。");
        }
    }

    // ゲーム名・起動シーン・ウィンドウ・アイコンのdraftを編集します。
    void EditorLayer::DrawProjectSettingsGameSection()
    {
        ImGui::TextUnformatted("ゲーム");
        ImGui::Separator();
        ImGui::SetNextItemWidth(360.0f);
        ImGui::InputText(
            "ゲーム名",
            m_projectGameNameBuffer.data(),
            m_projectGameNameBuffer.size());

        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt2(
            "初期解像度",
            m_projectWindowSize.data());
        ImGui::TextDisabled(
            "幅 320～7680、高さ 200～4320");

        ImGui::SetNextItemWidth(360.0f);
        ImGui::InputText(
            "ゲームアイコン",
            m_projectGameIconBuffer.data(),
            m_projectGameIconBuffer.size());
        ImGui::TextDisabled(
            "assets内の画像（.png / .jpg / .ico）。Export時にexeへ埋め込みます。"
            "空欄ならLamaPon標準アイコン");

        ImGui::Checkbox(
            "起動時にLamaPonロゴを表示",
            &m_projectSplashScreenDraft);
        ImGui::TextDisabled(
            "最初のシーンのロード中、ロゴを固定表示します。フェードや追加のロード処理はありません。");

        ImGui::SeparatorText("起動");
        ImGui::SetNextItemWidth(360.0f);
        ImGui::InputText(
            "起動シーン",
            m_projectStartupSceneBuffer.data(),
            m_projectStartupSceneBuffer.size());
        ImGui::SameLine();
        if (ImGui::Button("現在のシーン"))
        {
            // 現在のシーンの相対パス
            const auto relativeScene =
                m_scenePath.lexically_relative(
                    m_graphics.Assets().AssetRoot());
            // 起動シーン欄へ設定するパス
            const std::string scenePath =
                PathToUtf8(relativeScene);
            strncpy_s(
                m_projectStartupSceneBuffer.data(),
                m_projectStartupSceneBuffer.size(),
                scenePath.c_str(),
                _TRUNCATE);
        }

        // 選択中の起動シーン名
        const std::string preview =
            m_projectStartupSceneBuffer.data();
        ImGui::SetNextItemWidth(360.0f);
        if (ImGui::BeginCombo(
            "シーン一覧",
            preview.empty()
                ? "選択してください"
                : preview.c_str()))
        {
            // 起動シーンの候補アセット
            for (const auto& asset : m_assetFiles)
            {
                if (!IsSceneAsset(asset))
                {
                    continue;
                }

                // シーン候補の相対パス
                const std::string assetPath =
                    PathToUtf8(asset);
                // 選択中の起動シーンか
                const bool selected =
                    assetPath == preview;
                if (ImGui::Selectable(
                    assetPath.c_str(),
                    selected))
                {
                    strncpy_s(
                        m_projectStartupSceneBuffer.data(),
                        m_projectStartupSceneBuffer.size(),
                        assetPath.c_str(),
                        _TRUNCATE);
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

    }

    // シーン遷移中の読み込み画面のdraftを編集します。
    void EditorLayer::DrawProjectSettingsLoadingScreenSection()
    {
        ImGui::SeparatorText("読み込み画面");
        ImGui::PushID("ProjectLoadingScreen");
        static_cast<void>(DrawSceneLoadingScreenEditor(
            m_projectLoadingScreenDraft));
        ImGui::PopID();
        ImGui::TextWrapped(
            "シーン遷移の演出（画面を覆う絵）はエンジンに含まれません。"
            "Scriptで Scenes().TransitionCoverage() を読んで自分で描くか、"
            "パッケージの演出を導入します。");
    }

    // 描画設定のdraftを編集し、個別の品質変更をCustomとして扱います。
    void EditorLayer::DrawProjectSettingsGraphicsSection()
    {
        ImGui::SeparatorText("描画API");
        struct RenderingApiOption final
        {
            // 描画APIの値
            RenderingApi api;
            // FPS上限の表示名
            const char* label;
        };
        // 描画APIの選択肢
        static constexpr std::array<
            RenderingApiOption,
            3> renderingApiOptions{ {
            { RenderingApi::Auto, "Auto" },
            { RenderingApi::DirectX11, "DirectX 11" },
            { RenderingApi::DirectX12Experimental,
                "DirectX 12 Experimental" }
        } };
        // 編集前の描画API
        const auto currentRenderingApi =
            m_projectGraphicsDraft.renderingApi;
        // 選択中の描画API名
        const char* currentRenderingApiLabel = "DirectX 11";
        // 描画設定の候補
        for (const auto& option : renderingApiOptions)
        {
            if (option.api == currentRenderingApi)
            {
                currentRenderingApiLabel = option.label;
            }
        }
        if (ImGui::BeginCombo(
                "Rendering API",
                currentRenderingApiLabel))
        {
            // 描画設定の候補
            for (const auto& option : renderingApiOptions)
            {
                // 選択中の候補か
                const bool selected =
                    option.api == currentRenderingApi;
                if (ImGui::Selectable(
                        option.label,
                        selected))
                {
                    m_projectGraphicsDraft.renderingApi =
                        option.api;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        // 描画APIの注意を表示します(message: 注意文)。
        const auto drawRenderingApiWarning =
            [](const char* message)
        {
            ImGui::PushStyleColor(
                ImGuiCol_Text,
                ImVec4{ 1.0f, 0.65f, 0.25f, 1.0f });
            ImGui::TextWrapped("%s", message);
            ImGui::PopStyleColor();
        };
        if (m_projectGraphicsDraft.renderingApi
            != m_graphics.StartupRenderingApi())
        {
            drawRenderingApiWarning(
                "描画APIの変更は再起動後に反映されます。"
                "エディターまたはゲームを再起動してください。");
        }
        if (m_projectGraphicsDraft.renderingApi
            == RenderingApi::DirectX12Experimental)
        {
            drawRenderingApiWarning(
                "DirectX 12は実験的な設定です。"
                "主要機能は対応済みですが、GPUやドライバーによって"
                "表示差または動作しない機能が残る可能性があります。");
            if (m_graphics.StartupRenderingApi()
                    == RenderingApi::DirectX12Experimental
                && m_graphics.ActiveRenderingApi()
                    != RenderingApi::DirectX12Experimental)
            {
                drawRenderingApiWarning(
                    "DirectX 12を初期化できなかったため、このセッションは"
                    "DirectX 11へフォールバックして起動しています。");
            }
        }

        ImGui::SeparatorText("グラフィック品質");
        // 選択できる品質プリセット
        constexpr std::array qualityPresets{
            GraphicsQualityPreset::Low,
            GraphicsQualityPreset::Medium,
            GraphicsQualityPreset::High,
            GraphicsQualityPreset::Ultra,
            GraphicsQualityPreset::Custom
        };
        // 選択中の品質プリセット名
        const auto qualityName =
            GraphicsQualityPresetName(
                m_projectGraphicsDraft.preset);
        if (ImGui::BeginCombo(
            "品質プリセット",
            qualityName.data()))
        {
            // 品質プリセットの候補
            for (const auto preset : qualityPresets)
            {
                // 品質プリセットの表示名
                const auto name =
                    GraphicsQualityPresetName(preset);
                // 選択中の候補か
                const bool selected =
                    preset
                    == m_projectGraphicsDraft.preset;
                if (ImGui::Selectable(
                    name.data(),
                    selected))
                {
                    if (preset
                        == GraphicsQualityPreset::Custom)
                    {
                        m_projectGraphicsDraft.preset =
                            preset;
                    }
                    else
                    {
                        // プリセット適用前のFPS上限
                        const auto targetFrameRate =
                            m_projectGraphicsDraft
                                .targetFrameRate;
                        // プリセット適用前の描画方式
                        const auto renderingPath =
                            m_projectGraphicsDraft
                                .renderingPath;
                        // プリセット適用前の描画API
                        const auto renderingApi =
                            m_projectGraphicsDraft
                                .renderingApi;
                        // FPS上限・描画方式・描画APIはプリセット適用後も維持します。
                        m_projectGraphicsDraft =
                            GraphicsSettingsForPreset(
                                preset);
                        m_projectGraphicsDraft
                            .targetFrameRate =
                                targetFrameRate;
                        m_projectGraphicsDraft
                            .renderingPath =
                                renderingPath;
                        m_projectGraphicsDraft
                            .renderingApi =
                                renderingApi;
                    }
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        // 描画方式は品質プリセットから独立して選択します。
        struct RenderingPathOption final
        {
            // 描画方式の値
            RenderingPath path;
            // FPS上限の表示名
            const char* label;
            // 描画方式の説明
            const char* help;
        };
        // 描画方式の選択肢
        static constexpr std::array<
            RenderingPathOption,
            2> renderingPathOptions{ {
            { RenderingPath::ForwardPlus,
                "Forward+（既定）",
                "視錐台を格子へ切って「そこへ届くライトの番号表」を"
                "毎フレーム作ります。ポイント＋スポットを合計256灯まで"
                "置けます。ライトを多く使うならこちら" },
            { RenderingPath::Forward,
                "Forward",
                "番号表を作らず、下の「Point Light上限」「Spot Light"
                "上限」までのライトだけで計算します。前計算のぶんが"
                "無くなるので、ライトが少ないシーンや非力な環境では"
                "こちらが軽くなります" }
        } };
        // 編集前の描画方式
        const auto currentPath =
            m_projectGraphicsDraft.renderingPath;
        // 選択中の描画方式名
        const char* currentPathLabel =
            renderingPathOptions.front().label;
        // 描画設定の候補
        for (const auto& option : renderingPathOptions)
        {
            if (option.path == currentPath)
            {
                currentPathLabel = option.label;
            }
        }
        if (ImGui::BeginCombo(
                "描画方式",
                currentPathLabel))
        {
            // 描画設定の候補
            for (const auto& option :
                renderingPathOptions)
            {
                // 選択中の候補か
                const bool selected =
                    option.path == currentPath;
                if (ImGui::Selectable(
                        option.label,
                        selected))
                {
                    m_projectGraphicsDraft
                        .renderingPath = option.path;
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s", option.help);
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        if (m_projectGraphicsDraft.renderingPath
            == RenderingPath::Forward)
        {
            ImGui::TextDisabled(
                "Forwardでは下のライト上限がそのまま"
                "1回の描画で使える灯数になります。");
        }

        // 個別変更をCustomの品質設定として記録します。
        const auto markCustom = [this]
        {
            m_projectGraphicsDraft.preset =
                GraphicsQualityPreset::Custom;
        };
        if (ImGui::SliderFloat(
            "描画スケール",
            &m_projectGraphicsDraft.renderScale,
            0.5f,
            2.0f,
            "%.2f"))
        {
            markCustom();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "1.00より小さくすると軽くなり、大きくすると"
                "高解像度で描いて縮小するため輪郭のギザギザが"
                "減ります（スーパーサンプリング）。"
                "2.00はピクセル数が4倍になるので重くなります。"
                "2.00はちょうど2x2の平均になるため一番綺麗です");
        }
        if (ImGui::SliderFloat(
                "自動LOD品質",
                &m_projectGraphicsDraft.automaticLodQuality,
                0.25f,
                2.0f,
                "%.2f"))
        {
            markCustom();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "1.00が基準です。小さくすると遠景モデルを早く"
                "低LODへ切り替えて軽量化し、大きくすると高LODを"
                "長く保ちます。Low=0.55 / Medium=0.80 / High=1.00 / Ultra=1.35");
        }
        if (ImGui::Checkbox(
            "Shadow",
            &m_projectGraphicsDraft.shadowsEnabled))
        {
            markCustom();
        }
        ImGui::BeginDisabled(
            !m_projectGraphicsDraft.shadowsEnabled);
        // シャドウ解像度の編集値
        int shadowResolution =
            static_cast<int>(
                m_projectGraphicsDraft.shadowResolution);
        if (ImGui::SliderInt(
            "Shadow解像度",
            &shadowResolution,
            256,
            8192))
        {
            m_projectGraphicsDraft.shadowResolution =
                static_cast<std::uint32_t>(
                    shadowResolution);
            markCustom();
        }
        // カスケード上限の編集値
        int cascadeLimit =
            static_cast<int>(
                m_projectGraphicsDraft.shadowCascadeLimit);
        if (ImGui::SliderInt(
            "Shadow Cascade上限",
            &cascadeLimit,
            1,
            4))
        {
            m_projectGraphicsDraft.shadowCascadeLimit =
                static_cast<std::uint32_t>(
                    cascadeLimit);
            markCustom();
        }
        ImGui::EndDisabled();
        if (ImGui::Checkbox(
            "Bloom",
            &m_projectGraphicsDraft.bloomEnabled))
        {
            markCustom();
        }
        if (ImGui::Checkbox(
            "Screen Space Lens Flare",
            &m_projectGraphicsDraft
                .screenSpaceLensFlareEnabled))
        {
            markCustom();
        }
        if (ImGui::Checkbox(
            "被写界深度 (DoF)",
            &m_projectGraphicsDraft
                .depthOfFieldEnabled))
        {
            markCustom();
        }
        // 被写界深度のサンプル数
        int depthOfFieldSamples =
            static_cast<int>(
                m_projectGraphicsDraft
                    .depthOfFieldSampleCount);
        if (ImGui::SliderInt(
            "被写界深度のサンプル数",
            &depthOfFieldSamples,
            4,
            64))
        {
            m_projectGraphicsDraft.depthOfFieldSampleCount =
                static_cast<std::uint32_t>(
                    depthOfFieldSamples);
            markCustom();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "ぼけを作るサンプル数です。\n"
                "少ないとぼけが粒状になり、多いほど滑らかですが"
                "重くなります。");
        }
        if (ImGui::Checkbox(
            "モーションブラー",
            &m_projectGraphicsDraft.motionBlurEnabled))
        {
            markCustom();
        }
        // モーションブラーのサンプル数
        int motionBlurSamples =
            static_cast<int>(
                m_projectGraphicsDraft
                    .motionBlurSampleCount);
        if (ImGui::SliderInt(
            "モーションブラーのサンプル数",
            &motionBlurSamples,
            2,
            32))
        {
            m_projectGraphicsDraft.motionBlurSampleCount =
                static_cast<std::uint32_t>(
                    motionBlurSamples);
            markCustom();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "ブレの線に沿って何回サンプルするかです。\n"
                "少ないとブレが縞に分かれて見えます。");
        }
        if (ImGui::Checkbox(
            "自動露出",
            &m_projectGraphicsDraft.autoExposureEnabled))
        {
            markCustom();
        }
        if (ImGui::Checkbox(
            "SSAO",
            &m_projectGraphicsDraft
                .ambientOcclusionEnabled))
        {
            markCustom();
        }
        if (ImGui::Checkbox(
            "FXAAアンチエイリアス",
            &m_projectGraphicsDraft.antiAliasingEnabled))
        {
            markCustom();
        }
        if (ImGui::Checkbox(
            "Fog",
            &m_projectGraphicsDraft.fogEnabled))
        {
            markCustom();
        }
        if (ImGui::Checkbox(
            "VSync",
            &m_projectGraphicsDraft.vSyncEnabled))
        {
            markCustom();
        }
        if (ImGui::Checkbox(
            "テクスチャのランタイム圧縮 (BC1/BC3)",
            &m_projectGraphicsDraft
                .runtimeTextureCompression))
        {
            markCustom();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "PNG/JPGの読み込み時にBCn圧縮して"
                "VRAM使用量を減らします。\n"
                "次に読み込まれるテクスチャから反映されます。");
        }
        // FPS上限の選択肢
        constexpr std::array<std::uint32_t, 7>
            frameRateOptions{
                0,
                30,
                60,
                120,
                144,
                240,
                360
            };
        // 選択中のFPS上限の表示
        const std::string frameRatePreview =
            m_projectGraphicsDraft.targetFrameRate == 0
            ? "無制限"
            : std::to_string(
                m_projectGraphicsDraft
                    .targetFrameRate)
                + " FPS";
        if (ImGui::BeginCombo(
                "FPS上限",
                frameRatePreview.c_str()))
        {
            // FPS上限の候補
            for (const auto frameRate :
                frameRateOptions)
            {
                // FPS上限の表示名
                const std::string label =
                    frameRate == 0
                    ? "無制限"
                    : std::to_string(frameRate)
                        + " FPS";
                // 選択中の候補か
                const bool selected =
                    m_projectGraphicsDraft
                        .targetFrameRate
                    == frameRate;
                if (ImGui::Selectable(
                        label.c_str(),
                        selected))
                {
                    m_projectGraphicsDraft
                        .targetFrameRate =
                            frameRate;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::TextDisabled(
            "リフレッシュレートを超えるにはVSyncを切ってください。"
            "有効なままだと、上限を上げてもモニターの値で頭打ちです。");
        // ポイントライト上限の編集値
        int pointLightLimit =
            static_cast<int>(
                m_projectGraphicsDraft.pointLightLimit);
        if (ImGui::SliderInt(
            "Point Light上限",
            &pointLightLimit,
            0,
            16))
        {
            m_projectGraphicsDraft.pointLightLimit =
                static_cast<std::uint32_t>(
                    pointLightLimit);
            markCustom();
        }
        // スポットライト上限の編集値
        int spotLightLimit =
            static_cast<int>(
                m_projectGraphicsDraft.spotLightLimit);
        if (ImGui::SliderInt(
            "Spot Light上限",
            &spotLightLimit,
            0,
            8))
        {
            m_projectGraphicsDraft.spotLightLimit =
                static_cast<std::uint32_t>(
                    spotLightLimit);
            markCustom();
        }
        ImGui::TextDisabled(
            "個別項目を変更するとCustomになります。Editor表示へ保存直後に反映されます。");

    }

    // ビューポート操作と感度のdraftを編集します。
    void EditorLayer::DrawProjectSettingsViewportSection()
    {
        ImGui::SeparatorText("ビューポート操作");
        ImGui::TextWrapped(
            "Scene Viewのカメラ操作をプロジェクト単位で設定します。"
            "フライ操作は従来の操作、オービット操作は注視点を中心にした操作です。");

        // 選択中の操作方式名
        const char* presetName =
            m_projectViewportDraft.navigationPreset
                == ViewportNavigationPreset::Orbit
            ? "オービット操作"
            : "フライ操作";
        if (ImGui::BeginCombo("操作プリセット", presetName))
        {
            // ビューポート操作の選択肢
            constexpr std::array<std::pair<
                const char*, ViewportNavigationPreset>, 2> presets{
                std::pair{ "フライ操作", ViewportNavigationPreset::Fly },
                std::pair{ "オービット操作", ViewportNavigationPreset::Orbit }
            };
            // name: 操作名、preset: 操作方式
            for (const auto& [name, preset] : presets)
            {
                // 選択中の操作方式か
                const bool selected =
                    m_projectViewportDraft.navigationPreset == preset;
                if (ImGui::Selectable(name, selected))
                {
                    m_projectViewportDraft.navigationPreset = preset;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        if (m_projectViewportDraft.navigationPreset
            == ViewportNavigationPreset::Orbit)
        {
            ImGui::TextDisabled(
                "Alt+左ドラッグ: 回転 / 中ドラッグ: パン / "
                "Alt+右ドラッグ: ズーム / 右ドラッグ: フライ操作");
        }
        else
        {
            ImGui::TextDisabled(
                "右ドラッグ: 回転 / 右ドラッグ+WASD: 移動 / "
                "ホイール: ズーム");
        }

        ImGui::SetNextItemWidth(180.0f);
        ImGui::SliderFloat(
            "回転感度",
            &m_projectViewportDraft.orbitSensitivity,
            0.1f,
            3.0f,
            "%.2fx");
        ImGui::SetNextItemWidth(180.0f);
        ImGui::SliderFloat(
            "パン感度",
            &m_projectViewportDraft.panSensitivity,
            0.1f,
            3.0f,
            "%.2fx");
        ImGui::SetNextItemWidth(180.0f);
        ImGui::SliderFloat(
            "ズーム感度",
            &m_projectViewportDraft.zoomSensitivity,
            0.1f,
            3.0f,
            "%.2fx");
        ImGui::Checkbox(
            "Y軸を反転",
            &m_projectViewportDraft.invertY);
        if (ImGui::Button("ビューポート設定を初期値に戻す"))
        {
            m_projectViewportDraft = ViewportSettings{};
        }
    }

    // 物理設定と衝突レイヤー・対称マトリクスのdraftを編集します。
    void EditorLayer::DrawProjectSettingsPhysicsSection()
    {
        ImGui::SeparatorText("重力");
        ImGui::TextDisabled(
            "Rigidbodyの「重力を使う」がオンのものへ掛かります"
            "（m/s²）。");
        // 重力の編集値
        float gravity[3]{
            m_projectPhysicsDraft.gravity.x,
            m_projectPhysicsDraft.gravity.y,
            m_projectPhysicsDraft.gravity.z
        };
        if (ImGui::DragFloat3(
            "重力##Physics",
            gravity,
            0.05f,
            -100.0f,
            100.0f,
            "%.2f"))
        {
            m_projectPhysicsDraft.gravity = {
                gravity[0], gravity[1], gravity[2]
            };
        }

        ImGui::SeparatorText("進め方");
        // 固定更新間隔の編集値
        float timeStep = m_projectPhysicsDraft.fixedTimeStep;
        if (ImGui::DragFloat(
            "固定タイムステップ（秒）##Physics",
            &timeStep,
            0.0005f,
            1.0f / 1000.0f,
            0.1f,
            "%.4f"))
        {
            m_projectPhysicsDraft.fixedTimeStep =
                std::clamp(timeStep, 1.0f / 1000.0f, 0.1f);
        }

        ImGui::TextDisabled(
            "= %.1f Hz。小さいほど正確ですが重くなります。"
            "FixedUpdateの間隔でもあります。",
            m_projectPhysicsDraft.fixedTimeStep > 0.0f
                ? 1.0f / m_projectPhysicsDraft.fixedTimeStep
                : 0.0f);

        // 追従更新回数の編集値
        int catchUp = static_cast<int>(
            m_projectPhysicsDraft.maximumCatchUpSteps);
        if (ImGui::SliderInt(
            "1フレームの最大回数##Physics",
            &catchUp,
            1,
            32))
        {
            m_projectPhysicsDraft.maximumCatchUpSteps =
                static_cast<std::uint32_t>(catchUp);
        }
        ImGui::TextDisabled(
            "描画が遅れたときに取り戻す上限です。"
            "増やしすぎると処理負荷が増え、遅延がさらに悪化します。");

        ImGui::SeparatorText("当たり判定の解決");
        // 衝突解決の反復回数
        int iterations = static_cast<int>(
            m_projectPhysicsDraft.solverIterations);
        if (ImGui::SliderInt(
            "反復回数##Physics",
            &iterations,
            1,
            64))
        {
            m_projectPhysicsDraft.solverIterations =
                static_cast<std::uint32_t>(iterations);
        }
        ImGui::TextDisabled(
            "多いほどめり込みや揺れが減り、その分重くなります。"
            "積み上げた箱が沈むときに増やしてください。");

        ImGui::SeparatorText("すり抜け対策（DCD）");
        ImGui::TextDisabled(
            "既定の離散判定（DCD）は、1歩で進む距離が当たり判定の"
            "薄さを超えるとすり抜けます。連続判定（CCD）は"
            "オブジェクトごとにRigidbodyで選びます（重いので"
            "必要なものだけに）。");
        ImGui::DragFloat(
            "DCDで安全な速さ（m/s）##Physics",
            &m_projectPhysicsDraft.discreteSafeSpeed,
            0.5f,
            0.01f,
            100000.0f,
            "%.1f");

        ImGui::TextDisabled(
            "今の設定では1歩あたり %.2f m 進みます。"
            "これより薄い当たり判定はすり抜けます。",
            m_projectPhysicsDraft.discreteSafeSpeed
                * m_projectPhysicsDraft.fixedTimeStep);
        ImGui::Checkbox(
            "超えたら頭打ちにする##Physics",
            &m_projectPhysicsDraft.clampDiscreteSpeed);
        ImGui::TextDisabled(
            "オフ（既定）なら、超えた物体をログで知らせるだけで"
            "挙動は変わりません。オンにするとCCD無しでもすり抜け"
            "にくくなりますが、落下速度などの挙動が変わる場合が"
            "あります。CCDを選んだ物体、キネマティック、"
            "眠っている物体はどちらの対象にもなりません。");

        ImGui::SeparatorText("スリープ（止まったものを休ませる）");
        ImGui::DragFloat(
            "速さのしきい値（m/s）##Physics",
            &m_projectPhysicsDraft.sleepLinearVelocity,
            0.01f,
            0.0f,
            10.0f,
            "%.2f");
        ImGui::DragFloat(
            "角速度のしきい値（rad/s）##Physics",
            &m_projectPhysicsDraft.sleepAngularVelocity,
            0.01f,
            0.0f,
            10.0f,
            "%.2f");
        ImGui::DragFloat(
            "眠るまでの秒数##Physics",
            &m_projectPhysicsDraft.sleepDelay,
            0.05f,
            0.0f,
            60.0f,
            "%.2f");
        ImGui::TextDisabled(
            "小さくすると止まりにくく、大きくすると"
            "動いているのに寝てしまいます。0にすると"
            "止まった瞬間に眠ります。");

        ImGui::SeparatorText("衝突レイヤーの名前");
        ImGui::TextDisabled(
            "コライダーのLayer番号（0〜31）に名前を付けます。"
            "空欄は未使用の意味で、下のマトリクス表に出ません。"
            "名前を変えても既存シーンの挙動は変わりません"
            "（判定は番号で行うため）。");
        // 衝突レイヤーの番号
        for (std::size_t layerIndex = 0;
            layerIndex < CollisionLayerCount;
            ++layerIndex)
        {
            ImGui::PushID(
                static_cast<int>(layerIndex) + 91000);
            // 衝突レイヤー名の編集欄
            std::array<char, 64> nameBuffer{};
            strncpy_s(
                nameBuffer.data(),
                nameBuffer.size(),
                m_projectPhysicsDraft
                    .layerNames[layerIndex].c_str(),
                _TRUNCATE);
            // 衝突レイヤー番号の表示
            const std::string label =
                std::to_string(layerIndex);
            ImGui::SetNextItemWidth(240.0f);
            if (ImGui::InputText(
                label.c_str(),
                nameBuffer.data(),
                nameBuffer.size()))
            {
                m_projectPhysicsDraft
                    .layerNames[layerIndex] =
                    nameBuffer.data();
            }
            ImGui::PopID();
        }

        ImGui::SeparatorText("衝突マトリクス");
        ImGui::TextDisabled(
            "チェックを外したペアは当たりません（既定は全部オン）。"
            "コライダーごとのCollision Maskにも別途従います。"
            "RaycastやOverlapなどの問い合わせには掛かりません"
            "（問い合わせは呼び出し側のマスクで絞ります）。");
        {
            // レイヤー0と名前のある番号
            std::vector<std::size_t> usedLayers;
            // 衝突レイヤーの番号
            for (std::size_t layerIndex = 0;
                layerIndex < CollisionLayerCount;
                ++layerIndex)
            {
                if (layerIndex == 0
                    || !m_projectPhysicsDraft
                        .layerNames[layerIndex].empty())
                {
                    usedLayers.push_back(layerIndex);
                }
            }
            // 表の高さを行数から求め、末尾で高さが0になるのを防ぎます。
            // 衝突表の表示高さ
            const float matrixHeight =
                ImGui::GetTextLineHeightWithSpacing()
                * (static_cast<float>(usedLayers.size())
                    + 2.5f);
            if (ImGui::BeginTable(
                "##CollisionMatrix",
                static_cast<int>(usedLayers.size()) + 1,
                ImGuiTableFlags_Borders
                    | ImGuiTableFlags_SizingFixedFit
                    | ImGuiTableFlags_ScrollX,
                ImVec2{ 0.0f, matrixHeight }))
            {
                ImGui::TableSetupColumn("");
                // 衝突相手のレイヤー番号
                for (const auto column : usedLayers)
                {
                    ImGui::TableSetupColumn(
                        std::to_string(column).c_str());
                }
                ImGui::TableHeadersRow();
                // 衝突元のレイヤー番号
                for (const auto row : usedLayers)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    // 衝突元レイヤーの名前
                    const auto& rowName =
                        m_projectPhysicsDraft
                            .layerNames[row];
                    ImGui::Text(
                        "%zu: %s",
                        row,
                        rowName.empty()
                            ? "(無名)"
                            : rowName.c_str());
                    // 衝突表の表示列番号
                    int cellColumn = 0;
                    // 衝突相手のレイヤー番号
                    for (const auto column : usedLayers)
                    {
                        ++cellColumn;
                        // 対称行列の下三角は省略します。
                        if (column < row)
                        {
                            continue;
                        }
                        ImGui::TableSetColumnIndex(
                            cellColumn);
                        ImGui::PushID(
                            static_cast<int>(
                                row * 32 + column)
                            + 92000);
                        // レイヤー間で衝突させるか
                        bool collide =
                            (m_projectPhysicsDraft
                                .collisionMatrix[row]
                                & (1u << column)) != 0;
                        if (ImGui::Checkbox(
                            "##cell",
                            &collide))
                        {
                            if (collide)
                            {
                                m_projectPhysicsDraft
                                    .collisionMatrix[row]
                                    |= (1u << column);
                                m_projectPhysicsDraft
                                    .collisionMatrix[column]
                                    |= (1u << row);
                            }
                            else
                            {
                                m_projectPhysicsDraft
                                    .collisionMatrix[row]
                                    &= ~(1u << column);
                                m_projectPhysicsDraft
                                    .collisionMatrix[column]
                                    &= ~(1u << row);
                            }
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
        }

        ImGui::Spacing();
        if (ImGui::Button("既定へ戻す##Physics"))
        {
            m_projectPhysicsDraft = PhysicsSettings{};
        }
        ImGui::SameLine();
        ImGui::TextDisabled(
            "エンジン標準の物理設定に戻します。");
    }

    // タグ候補のdraftを編集し、列挙後に削除を適用します。
    void EditorLayer::DrawProjectSettingsTagsSection()
    {
        ImGui::SeparatorText("タグ");
        ImGui::TextDisabled(
            "GameObjectのタグ候補です。InspectorのTag欄はこの一覧から選びます。");
        {
            // 列挙後に削除するタグ番号
            std::optional<std::size_t> tagToDelete;
            // 表示中のタグ番号
            for (std::size_t tagIndex = 0;
                tagIndex < m_projectTagsDraft.size();
                ++tagIndex)
            {
                ImGui::PushID(
                    static_cast<int>(tagIndex) + 90000);
                ImGui::BulletText(
                    "%s",
                    m_projectTagsDraft[tagIndex].c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("削除"))
                {
                    tagToDelete = tagIndex;
                }
                ImGui::PopID();
            }
            if (tagToDelete)
            {
                m_projectTagsDraft.erase(
                    m_projectTagsDraft.begin()
                    + static_cast<std::ptrdiff_t>(
                        *tagToDelete));
            }
            ImGui::SetNextItemWidth(220.0f);
            ImGui::InputTextWithHint(
                "##ProjectNewTag",
                "新規タグ名",
                m_projectNewTagBuffer.data(),
                m_projectNewTagBuffer.size());
            ImGui::SameLine();
            if (ImGui::Button("タグを追加")
                && m_projectNewTagBuffer[0] != '\0')
            {
                // 追加するタグ名
                const std::string newTag =
                    m_projectNewTagBuffer.data();
                // 同名のタグが既にあるか
                const bool duplicate =
                    std::ranges::find(
                        m_projectTagsDraft,
                        newTag)
                    != m_projectTagsDraft.end();
                if (!duplicate)
                {
                    m_projectTagsDraft.push_back(newTag);
                }
                m_projectNewTagBuffer.fill('\0');
            }
        }

    }

    // 入力アクションのdraftを編集し、列挙後に削除を適用します。
    void EditorLayer::DrawProjectSettingsInputSection()
    {
        ImGui::SeparatorText("入力アクション");
        ImGui::TextDisabled(
            "複数の入力値を合成し、Action値を -1～1 で取得します。");

        // 列挙後に削除する入力番号
        std::optional<std::size_t> actionToDelete;
        ImGui::BeginChild(
            "InputActionList",
            ImVec2{ -1.0f, 270.0f },
            true);
        // 編集中の入力番号
        for (std::size_t actionIndex = 0;
            actionIndex < m_projectInputActionsDraft.size();
            ++actionIndex)
        {
            // 編集中の入力アクション
            auto& action =
                m_projectInputActionsDraft[actionIndex];
            ImGui::PushID(
                static_cast<int>(actionIndex));

            // 入力アクション名の編集欄
            std::array<char, 96> actionName{};
            strncpy_s(
                actionName.data(),
                actionName.size(),
                action.name.c_str(),
                _TRUNCATE);
            ImGui::SetNextItemWidth(300.0f);
            if (ImGui::InputText(
                "Action名",
                actionName.data(),
                actionName.size()))
            {
                action.name = actionName.data();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(
                m_projectInputActionsDraft.size() <= 1);
            if (ImGui::SmallButton("Action削除"))
            {
                actionToDelete = actionIndex;
            }
            ImGui::EndDisabled();

            // 列挙後に削除する割当番号
            std::optional<std::size_t> bindingToDelete;
            // 編集中の入力割当番号
            for (std::size_t bindingIndex = 0;
                bindingIndex < action.bindings.size();
                ++bindingIndex)
            {
                // 編集中の入力割当
                auto& binding =
                    action.bindings[bindingIndex];
                ImGui::PushID(
                    static_cast<int>(bindingIndex));
                // 割当済み入力の表示名
                const auto controlName =
                    InputControlDisplayName(
                        binding.control);
                ImGui::SetNextItemWidth(285.0f);
                if (ImGui::BeginCombo(
                    "入力",
                    controlName.data()))
                {
                    // 入力デバイス値の候補
                    for (const auto control :
                        AllInputControls())
                    {
                        // 選択中の入力値か
                        const bool selected =
                            control == binding.control;
                        // 入力候補の表示名
                        const auto displayName =
                            InputControlDisplayName(
                                control);
                        if (ImGui::Selectable(
                            displayName.data(),
                            selected))
                        {
                            binding.control = control;
                        }
                        if (selected)
                        {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100.0f);
                ImGui::DragFloat(
                    "倍率",
                    &binding.scale,
                    0.05f,
                    -4.0f,
                    4.0f,
                    "%.2f");
                ImGui::SameLine();
                ImGui::BeginDisabled(
                    action.bindings.size() <= 1);
                if (ImGui::SmallButton("削除"))
                {
                    bindingToDelete = bindingIndex;
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            if (bindingToDelete)
            {
                action.bindings.erase(
                    action.bindings.begin()
                    + static_cast<std::ptrdiff_t>(
                        *bindingToDelete));
            }

            ImGui::BeginDisabled(
                action.bindings.size() >= 16);
            if (ImGui::SmallButton("入力を追加"))
            {
                action.bindings.push_back(
                    InputBinding{
                        InputControl::KeyboardSpace,
                        1.0f
                    });
            }
            ImGui::EndDisabled();
            ImGui::Separator();
            ImGui::PopID();
        }
        ImGui::EndChild();

        if (actionToDelete)
        {
            m_projectInputActionsDraft.erase(
                m_projectInputActionsDraft.begin()
                + static_cast<std::ptrdiff_t>(
                    *actionToDelete));
        }
        ImGui::BeginDisabled(
            m_projectInputActionsDraft.size() >= 64);
        if (ImGui::Button("Actionを追加"))
        {
            // 新規アクションの名前
            std::string name = "NewAction";
            // 重複を避ける名前の連番
            std::size_t suffix = 2;
            // 入力名の重複を確認します(candidate: 名前候補)。
            const auto nameExists =
                [this](const std::string_view candidate)
                {
                    // 候補と同名かを調べます(action: 登録済み入力)。
                    return std::ranges::any_of(
                        m_projectInputActionsDraft,
                        [candidate](
                            const InputActionDefinition& action)
                        {
                            return action.name == candidate;
                        });
                };
            while (nameExists(name))
            {
                name = "NewAction"
                    + std::to_string(suffix++);
            }
            m_projectInputActionsDraft.push_back(
                InputActionDefinition{
                    std::move(name),
                    {
                        {
                            InputControl::KeyboardSpace,
                            1.0f
                        }
                    }
                });
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("既定に戻す"))
        {
            m_projectInputActionsDraft =
                DefaultInputActions();
        }

    }

    // draftを検証して保存・反映し、例外時はエラーを表示してfalseを返します。
    bool EditorLayer::SaveProjectSettingsDraft()
    {
        try
        {
            // 検証・保存するプロジェクト設定
            ProjectSettings settings;
            settings.gameName =
                m_projectGameNameBuffer.data();
            settings.windowWidth =
                static_cast<std::uint32_t>(
                    std::max(m_projectWindowSize[0], 0));
            settings.windowHeight =
                static_cast<std::uint32_t>(
                    std::max(m_projectWindowSize[1], 0));
            settings.startupScene =
                PathFromUtf8(
                    m_projectStartupSceneBuffer.data());
            settings.gameIcon =
                PathFromUtf8(
                    m_projectGameIconBuffer.data());
            settings.splashScreenEnabled =
                m_projectSplashScreenDraft;
            settings.loadingScreen =
                m_projectLoadingScreenDraft;
            settings.graphics =
                ClampGraphicsSettings(
                    m_projectGraphicsDraft);
            settings.viewport = m_projectViewportDraft;
            settings.physics = m_projectPhysicsDraft;
            settings.inputActions =
                m_projectInputActionsDraft;
            settings.tags = m_projectTagsDraft;
            settings.scriptEditorPath =
                m_projectScriptEditorDraft;
            settings.stripShaderSourceOnExport =
                m_projectStripShaderSourceDraft;
            settings.autoBuildGameModuleOnSave =
                m_projectAutoBuildDraft;
            settings.inspectorDecimals =
                static_cast<std::uint32_t>(
                    std::clamp(
                        m_projectInspectorDecimalsDraft,
                        0,
                        6));
            settings.network = m_projectNetworkDraft;
            settings.online = m_projectOnlineDraft;
            settings.online.serviceBaseUrl =
                m_projectOnlineServiceBaseUrlBuffer.data();
            settings.online.gameId =
                m_projectOnlineGameIdBuffer.data();
            settings.online.environmentId =
                m_projectOnlineEnvironmentIdBuffer.data();
            settings.online.discordPresence.applicationId =
                m_projectDiscordPresenceApplicationIdBuffer
                    .data();
            settings.online.discordPresence
                .defaultLargeImageKey =
                m_projectDiscordPresenceImageKeyBuffer.data();
            settings.online.discordPresence
                .defaultLargeImageText =
                m_projectDiscordPresenceImageTextBuffer.data();
            ValidateProjectSettings(settings);

            // 検証する起動シーンのパス
            const auto startupScene =
                m_graphics.Assets().AssetRoot()
                / settings.startupScene;
            if (!std::filesystem::is_regular_file(
                startupScene))
            {
                throw std::runtime_error(
                    "起動シーンが見つかりません: "
                    + PathToUtf8(settings.startupScene));
            }
            if (!settings.gameIcon.empty()
                && !std::filesystem::is_regular_file(
                    m_graphics.Assets().AssetRoot()
                    / settings.gameIcon))
            {
                throw std::runtime_error(
                    "ゲームアイコンが見つかりません: "
                    + PathToUtf8(settings.gameIcon));
            }
            if (!settings.scriptEditorPath.empty()
                && !std::filesystem::is_regular_file(
                    settings.scriptEditorPath))
            {
                throw std::runtime_error(
                    "スクリプトエディターが見つかりません: "
                    + PathToUtf8(settings.scriptEditorPath));
            }

            // メモリを先に更新するため、保存失敗時にも旧設定へは戻りません。
            m_projectSettings = std::move(settings);
            // 編集中の通信セッション
            if (auto* network = ActiveNetworkSession(); network != nullptr && !m_playing)
            {

                static_cast<void>(network->Configure(m_projectSettings.network));
            }
            SaveProjectConfiguration();
            m_graphics.Input().SetActions(
                m_projectSettings.inputActions);
            // 保存時の即時反映は入力・描画・タグで、物理設定はここでは再適用しません。
            m_graphics.SetGraphicsSettings(
                m_projectSettings.graphics);
            m_scene.SetRegisteredTags(
                m_projectSettings.tags);
            SetStatus(
                "プロジェクト設定を保存しました: "
                + m_projectSettings.gameName);
            m_projectSettingsError.clear();
            return true;
        }
        // 処理失敗時の例外
        catch (const std::exception& exception)
        {
            m_projectSettingsError = exception.what();
            SetStatus(
                "プロジェクト設定を保存できませんでした: "
                + m_projectSettingsError,
                true);
            return false;
        }
    }

    // カテゴリー別にdraftを編集し、保存成功またはキャンセルで設定画面を閉じます。
    void EditorLayer::DrawProjectSettingsDialog()
    {
        // プロジェクト設定のPopup名
        constexpr const char* popupName =
            "プロジェクト設定とビルド##ProjectSettings";
        // 内容を先頭へスクロールするか
        bool resetContentScroll = false;
        if (m_projectSettingsDialogRequested)
        {
            ImGui::OpenPopup(popupName);
            m_projectSettingsDialogRequested = false;
            resetContentScroll = true;
        }

        ImGui::SetNextWindowSize(
            ImVec2{ 840.0f, 700.0f },
            ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal(
            popupName,
            nullptr,
            ImGuiWindowFlags_None))
        {
            return;
        }


        // 設定カテゴリーの表示名
        constexpr std::array<const char*, 10> categories{
            "ゲーム",
            "グラフィック",
            "ビューポート設定",
            "物理",
            "タグ",
            "入力",
            "スクリプト",
            "ビルドプロファイル",
            "オンライン",
            "サービス連携"
        };
        ImGui::BeginChild(
            "ProjectSettingsCategories",
            ImVec2{ 175.0f, -96.0f },
            true);
        // 設定カテゴリーの番号
        for (std::size_t index = 0;
            index < categories.size();
            ++index)
        {
            if (ImGui::Selectable(
                categories[index],
                m_projectSettingsCategory
                    == static_cast<int>(index)))
            {
                resetContentScroll =
                    m_projectSettingsCategory
                    != static_cast<int>(index);
                m_projectSettingsCategory =
                    static_cast<int>(index);
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild(
            "ProjectSettingsContent",
            ImVec2{ 0.0f, -96.0f });
        if (resetContentScroll)
        {
            ImGui::SetScrollY(0.0f);
        }
        // 末尾撮影の指定時は毎フレーム最下部へスクロールします。
        if (m_screenshotScrollToBottom
            && !m_screenshotRequest.imagePath.empty())
        {
            ImGui::SetScrollY(ImGui::GetScrollMaxY());
        }
        // 選択中の設定カテゴリーを描画します。
        switch (m_projectSettingsCategory)
        {
        case 1:
            DrawProjectSettingsGraphicsSection();
            break;
        case 2:
            DrawProjectSettingsViewportSection();
            break;
        case 3:
            DrawProjectSettingsPhysicsSection();
            break;
        case 4:
            DrawProjectSettingsTagsSection();
            break;
        case 5:
            DrawProjectSettingsInputSection();
            break;
        case 6:
            DrawProjectSettingsScriptingSection();
            break;
        case 7:
            DrawProjectSettingsBuildSection();
            break;
        case 8:
            DrawProjectSettingsOnlineSection();
            break;
        case 9:
            DrawProjectSettingsServicesSection();
            break;
        default:
            DrawProjectSettingsGameSection();
            ImGui::Spacing();
            DrawProjectSettingsLoadingScreenSection();
            break;
        }
        ImGui::EndChild();

        ImGui::TextDisabled(
            "設定は .lamapon/project.json に保存されます。ゲーム向け設定は次回のExportに反映されます。");
        if (!m_projectSettingsError.empty())
        {
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.35f, 0.30f, 1.0f },
                "%s",
                m_projectSettingsError.c_str());
        }

        ImGui::Spacing();
        if (ImGui::Button("保存", ImVec2{ 100.0f, 0.0f })
            && SaveProjectSettingsDraft())
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("キャンセル"))
        {
            m_projectSettingsError.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    // プロジェクトの親フォルダーを初期位置として出力画面を開きます。
    void EditorLayer::OpenGameExportDialog()
    {
        if (!m_gameExportDialog)
            m_gameExportDialog = std::make_unique<GameExportDialog>();
        m_gameExportDialog->Open(m_graphics.Assets().AssetRoot().parent_path());
    }

    // 設定画面が閉じた後に出力画面を開く対象を予約します(target: 出力対象)。
    void EditorLayer::RequestGameExportDialog(
        const GameExportTarget target)
    {
        m_requestedGameExportTarget = target;
    }

    // 設定画面の終了後に予約を回収し、出力画面を描画します。
    void EditorLayer::DrawGameExportDialog()
    {
        if (m_requestedGameExportTarget.has_value()
            && !ImGui::IsPopupOpen(
                "プロジェクト設定とビルド##ProjectSettings"))
        {
            if (!m_gameExportDialog)
            {
                m_gameExportDialog =
                    std::make_unique<GameExportDialog>();
            }
            m_gameExportDialog->SelectTarget(
                *m_requestedGameExportTarget);
            m_gameExportDialog->Open(
                m_graphics.Assets().AssetRoot().
                    parent_path());
            m_requestedGameExportTarget.reset();
        }
        if (!m_gameExportDialog) return;
        // UIスレッドでシーンを保存し、通知(message: 状態文, failed: 失敗か)と選択(initial: 初期フォルダー)を渡します。
        m_gameExportDialog->Draw(GameExportDialogContext{
            m_engineRoot, ExecutableDirectory(), m_graphics.Assets().AssetRoot(),
            ProjectSettingsPath(), m_projectSettings,
            [this]
            {
                if (m_scenePath.empty())
                    throw std::runtime_error("先にシーンを保存してください");
                m_scene.SaveToFile(m_scenePath);
                MarkSceneSaved();
            },
            [this](std::string message, bool failed)
            {
                SetStatus(std::move(message), failed);
            },
            [this](const std::filesystem::path& initial)
            {
                return BrowseForExportDirectory(m_window, initial);
            }
        });
    }

    // 編集状態を保存し、遷移・通信・粒子・音声・時計を初期化して再生を開始します。
    void EditorLayer::StartPlaying()
    {
        try
        {
            if (m_animationTimelineOpen)
            {
                CloseAnimationTimeline(true);
            }
            // 再生前に初期化する通信連携
            if (auto* bridge = ActiveNetworkSceneBridge()) bridge->Reset();
            // 再生に使う通信セッション
            if (auto* network = ActiveNetworkSession())
                static_cast<void>(network->Configure(m_projectSettings.network));
            m_playSnapshot = m_scene.SerializeToJson();
            if (!m_scenePath.empty())
            {
                m_scene.Scenes().
                    SetCurrentScenePath(
                        m_scenePath);
            }
            // 前回のScriptによる設定を残さないよう、再生開始前に遷移を初期化します。
            m_scene.Scenes().ResetTransition();
            m_scene.Scenes().SetDefaultTransition({});
            m_scene.Scenes().LoadingScreen() =
                m_projectSettings.loadingScreen;
            // 再生開始時のシーン内オブジェクト
            for (const auto& gameObject :
                m_scene.GameObjects())
            {
                // 再生開始時の粒子コンポーネント
                if (auto* particles =
                    gameObject->GetComponent<
                        ParticleSystemComponent>())
                {
                    if (particles->PlayOnStart())
                    {
                        particles->Restart();
                    }
                    else
                    {
                        particles->Stop(true);
                    }
                }
            }
            m_playing = true;
            m_paused = false;
            m_stepRequested = false;
            m_remoteInputSnapshot.reset();
            m_remoteInputFrames = 0;
            m_graphics.Audio().SetSuspended(false);
            // 再生開始時に時計とタイムスケールを初期化します。
            Time::Detail::Reset();
            SetStatus("再生モード");
        }
        // 処理失敗時の例外
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    // 再生中の更新と音声を一緒に停止・再開し、ステップ要求を消去します(paused: 一時停止するか)。
    void EditorLayer::SetPaused(const bool paused)
    {
        if (!m_playing)
        {
            return;
        }
        m_paused = paused;
        // 停止・再開時にステップ要求を持ち越しません。
        m_stepRequested = false;
        // 音声処理も停止し、再開時は元の再生位置から続けます。
        m_graphics.Audio().SetSuspended(m_paused);
        SetStatus(
            m_paused
                ? "一時停止中（描画は続きます）"
                : "再生モード");
    }

    // 一時停止中の再生に限り、次回更新で1フレーム進めるよう予約します。
    void EditorLayer::RequestSimulationStep()
    {
        if (!m_playing || !m_paused)
        {
            return;
        }
        m_stepRequested = true;
        SetStatus("1フレーム進めました");
    }

    // 再生前のシーンと表示状態を復元し、遷移・音声・時計を編集状態へ戻します。
    void EditorLayer::StopPlaying()
    {
        try
        {
            // 停止時に初期化する通信連携
            if (auto* bridge = ActiveNetworkSceneBridge()) bridge->Reset();
            m_scene.LoadFromJson(m_playSnapshot);
            m_scene.Scenes().
                CancelPending();
            // 遷移の途中で停止しても、覆いやBGMの音量を編集モードへ持ち込みません。
            m_scene.Scenes().ResetTransition();
            if (!m_scenePath.empty())
            {
                m_scene.Scenes().
                    SetCurrentScenePath(
                        m_scenePath);
            }
            if (m_scene.FindGameObject(m_selectedObjectId) == nullptr)
            {
                m_selectedObjectId = 0;
            }
            m_playing = false;
            if (m_scriptGameViewSizeChanged)
            {
                m_gameViewFixedResolution =
                    m_savedGameViewFixedResolution;
                m_gameViewResolutionWidth =
                    m_savedGameViewResolutionWidth;
                m_gameViewResolutionHeight =
                    m_savedGameViewResolutionHeight;
                m_gameViewResolutionScale =
                    m_savedGameViewResolutionScale;
                m_scriptGameViewSizeChanged = false;
            }
            m_paused = false;
            m_stepRequested = false;
            m_remoteInputSnapshot.reset();
            m_remoteInputFrames = 0;
            // 一時停止中の終了でも音声処理を再開しておきます。
            m_graphics.Audio().SetSuspended(false);
            m_playSnapshot.clear();
            // ゲームが変更したタイムスケールを編集状態へ持ち込みません。
            Time::Detail::Reset();
            SetStatus("停止しました。編集状態を復元しました");
        }
        // 処理失敗時の例外
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }
}
