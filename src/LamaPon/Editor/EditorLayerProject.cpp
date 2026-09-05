#include "LamaPon/Editor/EditorLayer.h"

#include "LamaPon/Editor/EditorLayerShared.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Audio/AudioSystem.h"
#include "LamaPon/Components/ParticleSystemComponent.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Physics/PhysicsSettings.h"
#include "LamaPon/Core/Time.h"
#include "LamaPon/Editor/GameExportDialog.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
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

    std::optional<std::filesystem::path> BrowseForExportDirectory(
        const HWND owner,
        const std::filesystem::path& initialDirectory)
    {
        std::array<wchar_t, MAX_PATH> displayName{};
        const std::wstring initialPath =
            initialDirectory.wstring();

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

        const PIDLIST_ABSOLUTE item =
            SHBrowseForFolderW(&browse);
        if (item == nullptr)
        {
            return std::nullopt;
        }

        std::array<wchar_t, MAX_PATH> selectedPath{};
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
    // project.json の外部変更検知用の内容ハッシュ（FNV-1a 64bit）。
    // WebDAV(Z:)ではmtimeがキャッシュで古いままのことがあるため
    // （ビルド側 fa4bdb4 と同じ轍）、更新時刻ではなく内容で比較する。
    // ファイルは数KBなので毎スキャン読んでよい
    std::uint64_t HashProjectSettingsFile(
        const std::filesystem::path& path,
        bool& readable)
    {
        readable = false;
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            return 0;
        }
        std::uint64_t hash = 1469598103934665603ull;
        char buffer[4096];
        while (stream.read(buffer, sizeof(buffer))
            || stream.gcount() > 0)
        {
            const std::streamsize count = stream.gcount();
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
{    std::filesystem::path EditorLayer::ProjectSettingsPath() const
    {
        return m_graphics.Assets().AssetRoot().parent_path()
            / L".lamapon"
            / L"project.json";
    }

    void EditorLayer::UpdateExternalProjectSettings()
    {
        // project.json（入力アクション等）を外部で編集・git pullした
        // 直後に、プロジェクトを開き直さなくても反映されるようにする。
        // 走査は2秒に1回・数KBの読み込みとハッシュだけ。エディター
        // 専用コードで、書き出したゲームには載らない。
        // Play中は走らせない: SetActionsが入力の現在値/前回値を
        // クリアするため押しっぱなしキーのWasPressedが偽発火するのと、
        // WebDAV越しの同期読みがフレームヒッチ源になるため
        // （シーン監視と同じ方針）。ダイアログ表示中も走らせない:
        // 画面上の下書きと競合し、保存でpull内容が無警告で巻き戻る。
        // どちらも基準ハッシュを動かさないので、Play停止・ダイアログを
        // 閉じた後の次スキャンで確実に反映される
        if (m_gameModuleBuildProcess != nullptr
            || m_playing
            || ImGui::IsPopupOpen("プロジェクト設定##ProjectSettings"))
        {
            return;
        }
        const double now = ImGui::GetTime();
        if (now - m_lastProjectSettingsScanAt < 2.0)
        {
            return;
        }
        m_lastProjectSettingsScanAt = now;

        const auto path = ProjectSettingsPath();
        bool readable = false;
        const std::uint64_t hash =
            HashProjectSettingsFile(path, readable);
        if (!readable)
        {
            // 置き換え中などの一時状態。次のスキャンで再試行する
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
        // 半書き込みでパースに失敗しても、内容がさらに変われば
        // ハッシュが動いて再試行される
        m_projectSettingsSeenHash = hash;

        try
        {
            m_projectSettings = LamaPon::LoadProjectSettings(path);
            // 適用範囲はプロジェクト設定ダイアログの保存時と同じ
            // （入力・グラフィックス・タグ）。物理は適用しない:
            // fixedTimeStep等の実行中差し替えは再現性を壊すため、
            // プロジェクトを開いたときだけにする
            m_graphics.Input().SetActions(
                m_projectSettings.inputActions);
            m_graphics.SetGraphicsSettings(
                m_projectSettings.graphics);
            m_scene.SetRegisteredTags(
                m_projectSettings.tags);
            SetStatus(
                "プロジェクト設定の外部変更を再読み込みしました");
        }
        catch (const std::exception& error)
        {
            SetStatus(
                std::string("プロジェクト設定の再読み込みに失敗: ")
                    + error.what(),
                true);
        }
    }

    bool EditorLayer::LoadProjectConfiguration()
    {
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
            bool readable = false;
            m_projectSettingsSeenHash =
                HashProjectSettingsFile(path, readable);
            m_projectSettingsHashInitialized = readable;
        }
        return true;
    }

    void EditorLayer::SaveProjectConfiguration() const
    {
        LamaPon::SaveProjectSettings(
            ProjectSettingsPath(),
            m_projectSettings,
            ProjectSettingsFileType::Project);
        // 自分の保存を外部変更として誤検知しないよう基準を更新する
        bool readable = false;
        m_projectSettingsSeenHash = HashProjectSettingsFile(
            ProjectSettingsPath(),
            readable);
        m_projectSettingsHashInitialized = readable;
    }

    bool EditorLayer::AddProjectTag(std::string tag)
    {
        // 前後の空白を除去してから登録します。
        const auto first =
            tag.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
        {
            SetStatus("タグ名が空です", true);
            return false;
        }
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

    void EditorLayer::OpenProjectSettingsDialog()
    {
        strncpy_s(
            m_projectGameNameBuffer.data(),
            m_projectGameNameBuffer.size(),
            m_projectSettings.gameName.c_str(),
            _TRUNCATE);
        const std::string startupScene =
            PathToUtf8(m_projectSettings.startupScene);
        strncpy_s(
            m_projectStartupSceneBuffer.data(),
            m_projectStartupSceneBuffer.size(),
            startupScene.c_str(),
            _TRUNCATE);
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
        // ダイアログを開くたびに検出し直すことで、ダイアログを
        // 開いたまま新しくエディターをインストールした場合にも
        // 対応します（頻繁に呼ばれる処理ではないため許容範囲）。
        m_projectScriptEditorOptions = DetectScriptEditors();
        m_projectSettingsError.clear();
        m_projectSettingsDialogRequested = true;
    }

    // プロジェクト設定「スクリプト」カテゴリーで選択した
    // .cppを開く外部エディターを探します。
    void EditorLayer::BrowseForScriptEditor()
    {
        std::array<wchar_t, 1024> selectedFile{};
        constexpr wchar_t filter[] =
            L"実行可能ファイル (*.exe)\0*.exe\0"
            L"すべてのファイル (*.*)\0*.*\0\0";

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

    void EditorLayer::DrawProjectSettingsScriptingSection()
    {
        ImGui::TextUnformatted("スクリプト");
        ImGui::Separator();
        ImGui::TextWrapped(
            "アセットブラウザーで.cppをダブルクリックしたときに開く"
            "エディターを選べます。");
        ImGui::Spacing();

        const bool useSystemDefault =
            m_projectScriptEditorDraft.empty();
        std::string preview = useSystemDefault
            ? "システムの既定（ファイルの関連付け）"
            : PathToUtf8(m_projectScriptEditorDraft);
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

    // プロジェクト設定「ゲーム」カテゴリー（名前・解像度・アイコン・起動シーン）。
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
            const auto relativeScene =
                m_scenePath.lexically_relative(
                    m_graphics.Assets().AssetRoot());
            const std::string scenePath =
                PathToUtf8(relativeScene);
            strncpy_s(
                m_projectStartupSceneBuffer.data(),
                m_projectStartupSceneBuffer.size(),
                scenePath.c_str(),
                _TRUNCATE);
        }

        const std::string preview =
            m_projectStartupSceneBuffer.data();
        ImGui::SetNextItemWidth(360.0f);
        if (ImGui::BeginCombo(
            "シーン一覧",
            preview.empty()
                ? "選択してください"
                : preview.c_str()))
        {
            for (const auto& asset : m_assetFiles)
            {
                if (!IsSceneAsset(asset))
                {
                    continue;
                }

                const std::string assetPath =
                    PathToUtf8(asset);
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

    // プロジェクト設定「グラフィック」カテゴリー。
    void EditorLayer::DrawProjectSettingsGraphicsSection()
    {
        ImGui::SeparatorText("グラフィック品質");
        constexpr std::array qualityPresets{
            GraphicsQualityPreset::Low,
            GraphicsQualityPreset::Medium,
            GraphicsQualityPreset::High,
            GraphicsQualityPreset::Ultra,
            GraphicsQualityPreset::Custom
        };
        const auto qualityName =
            GraphicsQualityPresetName(
                m_projectGraphicsDraft.preset);
        if (ImGui::BeginCombo(
            "品質プリセット",
            qualityName.data()))
        {
            for (const auto preset : qualityPresets)
            {
                const auto name =
                    GraphicsQualityPresetName(preset);
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
                        const auto targetFrameRate =
                            m_projectGraphicsDraft
                                .targetFrameRate;
                        // 描画方式はプリセットの範囲外の選択
                        // （絵の作り方そのもの）なので引き継ぎます。
                        const auto renderingPath =
                            m_projectGraphicsDraft
                                .renderingPath;
                        m_projectGraphicsDraft =
                            GraphicsSettingsForPreset(
                                preset);
                        m_projectGraphicsDraft
                            .targetFrameRate =
                                targetFrameRate;
                        m_projectGraphicsDraft
                            .renderingPath =
                                renderingPath;
                    }
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        // 描画方式はプリセットの外に置きます。品質の上げ下げでは
        // なく「ライトの計算のしかた」を選ぶ項目で、切り替えると
        // 置けるライトの数が変わるためです。
        struct RenderingPathOption final
        {
            RenderingPath path;
            const char* label;
            const char* help;
        };
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
        const auto currentPath =
            m_projectGraphicsDraft.renderingPath;
        const char* currentPathLabel =
            renderingPathOptions.front().label;
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
            for (const auto& option :
                renderingPathOptions)
            {
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
            for (const auto frameRate :
                frameRateOptions)
            {
                const std::string label =
                    frameRate == 0
                    ? "無制限"
                    : std::to_string(frameRate)
                        + " FPS";
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
        // VSync有効時はモニターのリフレッシュレートが上限になるため、
        // フレームレート設定の直後に解除方法を案内します。
        ImGui::TextDisabled(
            "リフレッシュレートを超えるにはVSyncを切ってください。"
            "有効なままだと、上限を上げてもモニターの値で頭打ちです。");
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

    // プロジェクト設定「ビューポート設定」カテゴリー。
    void EditorLayer::DrawProjectSettingsViewportSection()
    {
        ImGui::SeparatorText("ビューポート操作");
        ImGui::TextWrapped(
            "Scene Viewのカメラ操作をプロジェクト単位で設定します。"
            "フライ操作は従来の操作、オービット操作は注視点を中心にした操作です。");

        const char* presetName =
            m_projectViewportDraft.navigationPreset
                == ViewportNavigationPreset::Orbit
            ? "オービット操作"
            : "フライ操作";
        if (ImGui::BeginCombo("操作プリセット", presetName))
        {
            constexpr std::array<std::pair<
                const char*, ViewportNavigationPreset>, 2> presets{
                std::pair{ "フライ操作", ViewportNavigationPreset::Fly },
                std::pair{ "オービット操作", ViewportNavigationPreset::Orbit }
            };
            for (const auto& [name, preset] : presets)
            {
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

    void EditorLayer::DrawProjectSettingsPhysicsSection()
    {
        ImGui::SeparatorText("重力");
        ImGui::TextDisabled(
            "Rigidbodyの「重力を使う」がオンのものへ掛かります"
            "（m/s²）。");
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
        // 秒だけだと直感が働かないので、Hzを併記します。
        ImGui::TextDisabled(
            "= %.1f Hz。小さいほど正確ですが重くなります。"
            "FixedUpdateの間隔でもあります。",
            m_projectPhysicsDraft.fixedTimeStep > 0.0f
                ? 1.0f / m_projectPhysicsDraft.fixedTimeStep
                : 0.0f);

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
        // 「一番薄い当たり判定 ÷ 刻み幅」が境目なので、
        // 今の設定で1歩あたり何メートル進むかを併記します。
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
        for (std::size_t layerIndex = 0;
            layerIndex < CollisionLayerCount;
            ++layerIndex)
        {
            ImGui::PushID(
                static_cast<int>(layerIndex) + 91000);
            std::array<char, 64> nameBuffer{};
            strncpy_s(
                nameBuffer.data(),
                nameBuffer.size(),
                m_projectPhysicsDraft
                    .layerNames[layerIndex].c_str(),
                _TRUNCATE);
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
            // 表に出すのは名前が付いたレイヤーだけです（0は常に出す）。
            // 32×32を全部出すと画面が升目で埋まるため。
            std::vector<std::size_t> usedLayers;
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
            // ScrollX付きのテーブルは子ウィンドウになるため、
            // 高さを明示しないと「残りの高さ」に合わせられます。
            // スクロール末尾でも高さ0にならないよう、行数から高さを
            // 決めます。
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
                for (const auto column : usedLayers)
                {
                    ImGui::TableSetupColumn(
                        std::to_string(column).c_str());
                }
                ImGui::TableHeadersRow();
                for (const auto row : usedLayers)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    const auto& rowName =
                        m_projectPhysicsDraft
                            .layerNames[row];
                    ImGui::Text(
                        "%zu: %s",
                        row,
                        rowName.empty()
                            ? "(無名)"
                            : rowName.c_str());
                    int cellColumn = 0;
                    for (const auto column : usedLayers)
                    {
                        ++cellColumn;
                        // 対称なので上三角だけ出します
                        // 対称行列なので、上三角だけ表示します。
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

    void EditorLayer::DrawProjectSettingsTagsSection()
    {
        ImGui::SeparatorText("タグ");
        ImGui::TextDisabled(
            "GameObjectのタグ候補です。InspectorのTag欄はこの一覧から選びます。");
        {
            std::optional<std::size_t> tagToDelete;
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
                const std::string newTag =
                    m_projectNewTagBuffer.data();
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

    // プロジェクト設定「入力」カテゴリー。
    void EditorLayer::DrawProjectSettingsInputSection()
    {
        ImGui::SeparatorText("入力アクション");
        ImGui::TextDisabled(
            "複数の入力値を合成し、Action値を -1～1 で取得します。");

        std::optional<std::size_t> actionToDelete;
        ImGui::BeginChild(
            "InputActionList",
            ImVec2{ -1.0f, 270.0f },
            true);
        for (std::size_t actionIndex = 0;
            actionIndex < m_projectInputActionsDraft.size();
            ++actionIndex)
        {
            auto& action =
                m_projectInputActionsDraft[actionIndex];
            ImGui::PushID(
                static_cast<int>(actionIndex));

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

            std::optional<std::size_t> bindingToDelete;
            for (std::size_t bindingIndex = 0;
                bindingIndex < action.bindings.size();
                ++bindingIndex)
            {
                auto& binding =
                    action.bindings[bindingIndex];
                ImGui::PushID(
                    static_cast<int>(bindingIndex));
                const auto controlName =
                    InputControlDisplayName(
                        binding.control);
                ImGui::SetNextItemWidth(285.0f);
                if (ImGui::BeginCombo(
                    "入力",
                    controlName.data()))
                {
                    for (const auto control :
                        AllInputControls())
                    {
                        const bool selected =
                            control == binding.control;
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
            std::string name = "NewAction";
            std::size_t suffix = 2;
            const auto nameExists =
                [this](const std::string_view candidate)
                {
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

    void EditorLayer::DrawProjectSettingsDialog()
    {
        constexpr const char* popupName =
            "プロジェクト設定##ProjectSettings";
        if (m_projectSettingsDialogRequested)
        {
            ImGui::OpenPopup(popupName);
            m_projectSettingsDialogRequested = false;
        }

        ImGui::SetNextWindowSize(
            ImVec2{ 760.0f, 700.0f },
            ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal(
            popupName,
            nullptr,
            ImGuiWindowFlags_None))
        {
            return;
        }

        // 左のカテゴリー一覧と右の内容ペインへ分割します。
        constexpr std::array<const char*, 7> categories{
            "ゲーム",
            "グラフィック",
            "ビューポート設定",
            "物理",
            "タグ",
            "入力",
            "スクリプト"
        };
        ImGui::BeginChild(
            "ProjectSettingsCategories",
            ImVec2{ 150.0f, -96.0f },
            true);
        for (std::size_t index = 0;
            index < categories.size();
            ++index)
        {
            if (ImGui::Selectable(
                categories[index],
                m_projectSettingsCategory
                    == static_cast<int>(index)))
            {
                m_projectSettingsCategory =
                    static_cast<int>(index);
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild(
            "ProjectSettingsContent",
            ImVec2{ 0.0f, -96.0f });
        // スクリーンショットモードの「:bottom」指定。設定の下の方
        // （衝突マトリクス等）を撮るため、末尾へスクロールし続けます。
        if (m_screenshotScrollToBottom
            && !m_screenshotRequest.imagePath.empty())
        {
            ImGui::SetScrollY(ImGui::GetScrollMaxY());
        }
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
        default:
            DrawProjectSettingsGameSection();
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
        if (ImGui::Button("保存", ImVec2{ 100.0f, 0.0f }))
        {
            try
            {
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
                ValidateProjectSettings(settings);

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

                m_projectSettings = std::move(settings);
                SaveProjectConfiguration();
                m_graphics.Input().SetActions(
                    m_projectSettings.inputActions);
                m_graphics.SetGraphicsSettings(
                    m_projectSettings.graphics);
                m_scene.SetRegisteredTags(
                    m_projectSettings.tags);
                SetStatus(
                    "プロジェクト設定を保存しました: "
                    + m_projectSettings.gameName);
                m_projectSettingsError.clear();
                ImGui::CloseCurrentPopup();
            }
            catch (const std::exception& exception)
            {
                m_projectSettingsError =
                    exception.what();
                SetStatus(
                    "プロジェクト設定を保存できませんでした: "
                    + m_projectSettingsError,
                    true);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("キャンセル"))
        {
            m_projectSettingsError.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    void EditorLayer::OpenGameExportDialog()
    {
        if (!m_gameExportDialog)
            m_gameExportDialog = std::make_unique<GameExportDialog>();
        m_gameExportDialog->Open(m_graphics.Assets().AssetRoot().parent_path());
    }

    void EditorLayer::DrawGameExportDialog()
    {
        if (!m_gameExportDialog) return;
        // UIとワーカーは専用担当へ渡し、Sceneの保存はUIスレッドで完了します。
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

    void EditorLayer::StartPlaying()
    {
        try
        {
            if (m_animationTimelineOpen)
            {
                CloseAnimationTimeline(true);
            }
            m_playSnapshot = m_scene.SerializeToJson();
            if (!m_scenePath.empty())
            {
                m_scene.Scenes().
                    SetCurrentScenePath(
                        m_scenePath);
            }
            for (const auto& gameObject :
                m_scene.GameObjects())
            {
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
            // 再生ごとにタイムスケールと時計を初期状態へ戻します。
            Time::Detail::Reset();
            SetStatus("再生モード");
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    void EditorLayer::SetPaused(const bool paused)
    {
        if (!m_playing)
        {
            return;
        }
        m_paused = paused;
        // 溜まったステップ要求を持ち越さないようにします。
        m_stepRequested = false;
        // 絵が止まっているのに音だけ進むと状態がズレるので、
        // 音声処理も一緒に止めます。エンジン側で
        // 止めるため、再開すると元の位置から続きます。
        m_graphics.Audio().SetSuspended(m_paused);
        SetStatus(
            m_paused
                ? "一時停止中（描画は続きます）"
                : "再生モード");
    }

    void EditorLayer::RequestSimulationStep()
    {
        if (!m_playing || !m_paused)
        {
            return;
        }
        m_stepRequested = true;
        SetStatus("1フレーム進めました");
    }

    void EditorLayer::StopPlaying()
    {
        try
        {
            m_scene.LoadFromJson(m_playSnapshot);
            m_scene.Scenes().
                CancelPending();
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
            m_paused = false;
            m_stepRequested = false;
            m_remoteInputSnapshot.reset();
            m_remoteInputFrames = 0;
            // 一時停止したまま停止しても音が止まったままにならないように。
            m_graphics.Audio().SetSuspended(false);
            m_playSnapshot.clear();
            // ゲームが変更したタイムスケールを編集モードへ持ち込まない。
            Time::Detail::Reset();
            SetStatus("停止しました。編集状態を復元しました");
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }
}
