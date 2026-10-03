#include "LamaPon/Editor/BgmLoopPanel.h"
#include "LamaPon/Editor/EditorLayer.h"
#include "LamaPon/Editor/EditorGuiRenderer.h"
#include "LamaPon/Editor/EditorModelPreviewRenderer.h"
#include "LamaPon/Editor/FrameDebuggerPanel.h"
#include "LamaPon/Editor/GameExportDialog.h"
#include "LamaPon/Editor/MemoryProfilerPanel.h"
#include "LamaPon/Editor/PhysicsDebuggerPanel.h"
#include "LamaPon/Editor/ProfileAnalyzerPanel.h"
#include "LamaPon/Editor/ProfilerPanel.h"
#include "LamaPon/Editor/VehicleParametersPanel.h"

#include "LamaPon/Editor/EditorLayerShared.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Assets/DataAsset.h"
#include "LamaPon/Animation/AnimatorController.h"
#include "LamaPon/Audio/AudioSystem.h"
#include "LamaPon/Components/AudioSourceComponent.h"
#include "LamaPon/Components/CameraComponent.h"
#include "LamaPon/Components/DirectionalLightComponent.h"
#include "LamaPon/Components/Light2DComponent.h"
#include "LamaPon/Components/MeshCollider3DComponent.h"
#include "LamaPon/Components/MeshRendererComponent.h"
#include "LamaPon/Components/ModelRendererComponent.h"
#include "LamaPon/Components/ParticleSystemComponent.h"
#include "LamaPon/Components/PointLightComponent.h"
#include "LamaPon/Components/SpotLightComponent.h"
#include "LamaPon/Components/UICanvasComponent.h"
#include "LamaPon/Components/UIButtonComponent.h"
#include "LamaPon/Components/UILayoutGroupComponent.h"
#include "LamaPon/Components/RigidbodyComponent.h"
#include "LamaPon/Components/SpriteParticles2DComponent.h"
#include "LamaPon/Components/SpriteRendererComponent.h"
#include "LamaPon/Components/TilemapComponent.h"
#include "LamaPon/Components/TransformAnimatorComponent.h"
#include "LamaPon/Components/UIImageComponent.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/PlayerPrefs.h"
#include "LamaPon/Core/Profiler.h"
#include "LamaPon/Core/SaveData.h"
#include "LamaPon/Core/Time.h"
#include "LamaPon/Core/BuildInfo.h"
#include "LamaPon/Core/Version.h"
#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/PngWriter.h"
#include "LamaPon/Graphics/RenderPipeline.h"
#include "LamaPon/Editor/UiRecorder.h"
#include "LamaPon/Input/InputSystem.h"
#include "LamaPon/Online/OnlineServices.h"
#include "LamaPon/Scene/Scene.h"
#include "LamaPon/Scene/SceneManager.h"

#include <commdlg.h>

#include <shellapi.h>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <imgui_internal.h>
#include <imgui_impl_win32.h>
#include <ImGuizmo.h>

#include <DirectXMath.h>

#include <algorithm>
#include <cctype>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <ctime>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>

using namespace LamaPon::EditorDetail;

// Win32入力をImGuiへ渡します(window: 対象ウィンドウ, message: メッセージ種別, wParam: 主パラメーター, lParam: 補助パラメーター)。
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam);

namespace
{
    // 上部ツールバーの高さ
    constexpr float ToolbarHeight = 52.0f;

    // オンラインマニュアルのURL
    constexpr wchar_t OnlineManualUrl[] =
        L"https://lamapon-wiki.lamapon.workers.dev";
    // 保存データ操作の確認Popup ID
    constexpr char OnlinePersistenceConfirmationPopup[] =
        "操作の確認##OnlinePersistenceConfirmation";

    // アカウント状態の表示名を返します(state: 認証状態)。
    [[nodiscard]] const char* OnlineAccountStateLabel(
        const LamaPon::OnlineAccountState state) noexcept
    {
        using enum LamaPon::OnlineAccountState;
        switch (state)
        {
        case Unconfigured:
            return "未設定";
        case SignedOut:
            return "サインアウト済み";
        case StartingSignIn:
            return "ログインを開始しています";
        case WaitingForAuthorization:
            return "ブラウザーでの認証を待っています";
        case PollingAuthorization:
            return "認証結果を確認しています";
        case SignedIn:
            return "ログイン済み";
        case SigningOut:
            return "サインアウトしています";
        case Error:
            return "認証エラー";
        case RestoringSession:
            return "保存済みセッションを復元しています";
        case RefreshingSession:
            return "セッションを更新しています";
        }
        return "不明";
    }

    // クラウド同期状態の表示名を返します(state: 同期状態)。
    [[nodiscard]] const char* OnlineCloudSyncStateLabel(
        const LamaPon::OnlineCloudSyncState state) noexcept
    {
        using enum LamaPon::OnlineCloudSyncState;
        switch (state)
        {
        case Unavailable:
            return "利用できません";
        case Idle:
            return "待機中";
        case Synchronizing:
            return "同期しています";
        case WaitingToRetry:
            return "再試行を待っています";
        case Conflict:
            return "競合の解決を待っています";
        case Unauthorized:
            return "セッションを確認しています";
        case Stopped:
            return "停止しました";
        }
        return "不明";
    }

    // 同期停止理由の表示名を返します(reason: 停止理由)。
    [[nodiscard]] const char* OnlineCloudSyncStopReasonLabel(
        const LamaPon::OnlineCloudSyncStopReason reason) noexcept
    {
        using enum LamaPon::OnlineCloudSyncStopReason;
        switch (reason)
        {
        case None:
            return "なし";
        case LocalUnavailable:
            return "端末のセーブデータを読み取れません";
        case LocalCorrupt:
            return "端末のセーブデータを検証できません";
        case RemoteRejected:
            return "クラウドが操作を受理しませんでした";
        case InvalidRemoteResponse:
            return "クラウドの応答を検証できません";
        case JournalFailure:
            return "同期状態を保存できません";
        case InternalFailure:
            return "同期処理を続けられません";
        }
        return "同期処理を続けられません";
    }

    // 保護された保存データの復旧状態を表示名へ変換します(state: 復旧状態)。
    [[nodiscard]] const char* OnlineRecoveryStateLabel(
        const LamaPon::OnlinePersistenceRecoveryState state) noexcept
    {
        using enum LamaPon::OnlinePersistenceRecoveryState;
        switch (state)
        {
        case None:
            return "復旧待ちのデータはありません";
        case MemorySnapshot:
            return "保護したデータを安全な領域へ退避しています";
        case DurableSidecar:
            return "保護したデータを復元または破棄できます";
        case UnavailableSidecar:
            return "保護したデータを読み取れません";
        }
        return "復旧状態を確認できません";
    }

    // 名前末尾のシーン拡張子を大文字小文字を区別せず判定します(path: 判定するパス)。
    bool HasSceneExtension(const std::filesystem::path& path)
    {
        return Lowercase(LamaPon::PathToUtf8(path.filename())).ends_with(".scene.json");
    }

    // 名前末尾のPrefab拡張子を大文字小文字を区別せず判定します(path: 判定するパス)。
    bool HasPrefabExtension(const std::filesystem::path& path)
    {
        return Lowercase(LamaPon::PathToUtf8(path.filename())).ends_with(".prefab.json");
    }

    // 日本語フォントを順に試し、取得できなければ既定フォントへ戻してfalseを返します(inputOutput: ImGuiの入出力設定)。
    bool LoadJapaneseFont(ImGuiIO& inputOutput)
    {
        // 日本語フォントの候補パス
        constexpr std::array fontCandidates{
            "C:/Windows/Fonts/YuGothM.ttc",
            "C:/Windows/Fonts/meiryo.ttc",
            "C:/Windows/Fonts/msgothic.ttc"
        };

        // 日本語フォントの読込設定
        ImFontConfig fontConfig{};
        fontConfig.FontNo = 0;
        fontConfig.OversampleH = 2;
        fontConfig.OversampleV = 1;

        // 試す日本語フォントのパス
        for (const char* fontPath : fontCandidates)
        {
            if (std::filesystem::exists(fontPath)
                && inputOutput.Fonts->AddFontFromFileTTF(
                    fontPath,
                    18.0f,
                    &fontConfig,
                    nullptr) != nullptr)
            {
                return true;
            }
        }

        inputOutput.Fonts->AddFontDefault();
        return false;
    }

    // メニュー定義の変更検出用ハッシュを計算します(text: 比較する定義本文)。
    std::uint64_t HashProjectMenuManifest(const std::string_view text)
    {
        // 変更検出用の累積ハッシュ
        std::uint64_t value = 1469598103934665603ull;
        // ハッシュに混ぜる本文の1byte
        for (const unsigned char byte : text)
        {
            value ^= byte;
            value *= 1099511628211ull;
        }
        return value;
    }

    // 1～8階層・各96byte以内のメニューパスを分割し、不正時は例外を投げます(path: スラッシュ区切りのパス)。
    std::vector<std::string> SplitProjectMenuPath(
        const std::string_view path)
    {
        // 分割したメニュー階層名
        std::vector<std::string> result;
        // 階層名の開始位置
        std::size_t begin = 0;
        while (begin <= path.size())
        {
            // 次のスラッシュ位置
            const std::size_t separator = path.find('/', begin);
            // 階層名の末尾位置
            const std::size_t end = separator == std::string_view::npos
                ? path.size()
                : separator;
            // 検証する階層名
            const std::string_view part = path.substr(begin, end - begin);
            if (part.empty() || part == "." || part == "..")
            {
                throw std::runtime_error(
                    "メニューパスに空欄または . / .. は使えません");
            }
            result.emplace_back(part);
            if (result.back().size() > 96u)
            {
                throw std::runtime_error(
                    "メニュー名は96バイト以内にしてください");
            }
            if (separator == std::string_view::npos)
            {
                break;
            }
            begin = separator + 1u;
        }
        if (result.empty() || result.size() > 8u)
        {
            throw std::runtime_error(
                "メニューパスは1～8階層にしてください");
        }
        return result;
    }

    // 引用符と末尾のバックスラッシュを保つWindows引数表現を返します(argument: 単一の引数文字列)。
    std::wstring QuoteWindowsArgument(const std::wstring_view argument)
    {
        if (argument.empty())
        {
            return L"\"\"";
        }
        if (argument.find_first_of(L" \t\n\v\"")
            == std::wstring_view::npos)
        {
            return std::wstring{ argument };
        }

        // 引用処理後の引数文字列
        std::wstring result{ L'\"' };
        // 連続するバックスラッシュ数
        std::size_t backslashes = 0;
        // 引用処理する文字
        for (const wchar_t character : argument)
        {
            if (character == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (character == L'\"')
            {
                result.append(backslashes * 2u + 1u, L'\\');
                result.push_back(L'\"');
                backslashes = 0;
                continue;
            }
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(character);
        }
        result.append(backslashes * 2u, L'\\');
        result.push_back(L'\"');
        return result;
    }

}

namespace LamaPon
{
    // 借用する描画・シーン・保存・オンラインサービスは、このEditorLayerより長寿命である必要があります。
    // 借用サービスから編集UIと設定を初期化します(window: 対象ウィンドウ, graphics: 描画サービス, scene: 編集シーン, playerPrefs: 個別設定, saveData: セーブデータ, onlineServices: オンラインサービス, scenePath: 開くシーンのパス, engineRoot: エンジンルート, buildConfiguration: ビルド構成)。
    EditorLayer::EditorLayer(
        const HWND window,
        GraphicsDevice& graphics,
        Scene& scene,
        PlayerPrefs& playerPrefs,
        SaveDataStore& saveData,
        OnlineServices& onlineServices,
        std::filesystem::path scenePath,
        std::filesystem::path engineRoot,
        std::string buildConfiguration)
        : m_window(window)
        , m_graphics(graphics)
        , m_scene(scene)
        , m_playerPrefs(playerPrefs)
        , m_saveData(saveData)
        , m_onlineServices(onlineServices)
        , m_persistencePanelState(
            playerPrefs.FilePath(),
            saveData.Directory())
        , m_scenePath(std::move(scenePath))
        , m_engineRoot(std::filesystem::weakly_canonical(
            std::move(engineRoot)))
        , m_buildConfiguration(std::move(buildConfiguration))
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        // ImGuiの入出力設定
        auto& inputOutput = ImGui::GetIO();
        inputOutput.ConfigFlags |=
            ImGuiConfigFlags_NavEnableKeyboard
            | ImGuiConfigFlags_DockingEnable;
        // 保存するUI配置のパス
        const auto layoutPath =
            EditorSettingsPath().parent_path()
            / "imgui-layout.ini";
        std::filesystem::create_directories(
            layoutPath.parent_path());
        m_imguiIniPath = PathToUtf8(layoutPath);
        inputOutput.IniFilename =
            m_imguiIniPath.c_str();
        inputOutput.ConfigWindowsMoveFromTitleBarOnly = true;
        // 日本語フォントの読込成功
        const bool japaneseFontLoaded = LoadJapaneseFont(inputOutput);

        ImGui::StyleColorsDark();
        // 初期化するUIスタイル
        auto& style = ImGui::GetStyle();
        style.WindowRounding = 4.0f;
        style.FrameRounding = 3.0f;
        style.GrabRounding = 3.0f;

        m_win32Initialized = ImGui_ImplWin32_Init(window);
        if (!m_win32Initialized)
        {
            ImGui::DestroyContext();
            throw std::runtime_error("ImGui Win32 backend initialization failed.");
        }
        try
        {
            m_editorGuiRenderer = CreateEditorGuiRenderer(
                graphics.ActiveRenderingApi());
            m_editorModelPreviewRenderer =
                CreateEditorModelPreviewRenderer(
                    graphics.ActiveRenderingApi(),
                    graphics);
            m_editorGuiRenderer->Initialize(graphics);
        }
        catch (...)
        {
            if (m_editorGuiRenderer)
            {
                m_editorGuiRenderer->Shutdown();
                m_editorGuiRenderer.reset();
            }
            ImGui_ImplWin32_Shutdown();
            m_win32Initialized = false;
            ImGui::DestroyContext();
            throw;
        }
        ResetHistory();
        MarkSceneSaved();
        // 起動経路では直前に SetAssetRoot が走査を終えています。
        RefreshAssets(true);
        CreateDefaultEditorPresets();
        RegisterBuiltInEditorExtensions();
        // プロジェクト設定の読込エラー
        std::string projectSettingsError;
        try
        {
            if (!LoadProjectConfiguration())
            {
                // アセットルート基準のシーン
                const auto relativeScene =
                    m_scenePath.lexically_relative(
                        m_graphics.Assets().AssetRoot());
                if (!relativeScene.empty())
                {
                    m_projectSettings.startupScene =
                        relativeScene;
                }
                SaveProjectConfiguration();
            }
        }
        // 起動時の設定読込エラー
        catch (const std::exception& exception)
        {
            projectSettingsError =
                std::string{
                    "プロジェクト設定を読み込めませんでした: "
                }
                + exception.what();
        }
        try
        {
            // 編集設定の復元成功
            const bool settingsLoaded = LoadEditorSettings();
            if (!projectSettingsError.empty())
            {
                SetStatus(projectSettingsError, true);
            }
            else
            {
                SetStatus(
                    settingsLoaded
                        ? "エディター設定を復元しました"
                        : (japaneseFontLoaded
                            ? "日本語エディターを起動しました"
                            : "日本語フォントが見つからないため既定フォントを使用します"),
                    !japaneseFontLoaded);
            }
        }
        // 起動時の設定読込エラー
        catch (const std::exception& exception)
        {
            SetStatus(
                std::string{ "エディター設定を読み込めませんでした: " }
                    + exception.what(),
                true);
        }
        DragAcceptFiles(m_window, TRUE);
    }

    // パッケージ処理を待ち、設定保存と拡張終了後にImGuiを破棄します。
    EditorLayer::~EditorLayer()
    {
        // 借用サービスを参照するパッケージ処理を、編集状態の破棄前に終了させます。
        JoinPackageWorker();

        DragAcceptFiles(m_window, FALSE);

        if (m_gameModuleBuildProcess != nullptr)
        {
            CloseHandle(m_gameModuleBuildProcess);
            m_gameModuleBuildProcess = nullptr;
        }

        try
        {
            SaveEditorSettings();
        }
        // 終了時の設定保存失敗は終了処理を止めません。
        catch (const std::exception&)
        {
        }

        // 拡張機能の終了処理はImGuiコンテキストが有効なうちに行います。
        m_editorExtensions.Shutdown();

        if (m_editorGuiRenderer)
        {
            m_editorGuiRenderer->Shutdown();
            m_editorGuiRenderer.reset();
        }
        if (m_win32Initialized)
        {
            ImGui_ImplWin32_Shutdown();
        }

        if (ImGui::GetCurrentContext() != nullptr)
        {
            if (!m_imguiIniPath.empty())
            {
                ImGui::SaveIniSettingsToDisk(
                    m_imguiIniPath.c_str());
            }
            ImGui::DestroyContext();
        }
    }

    // 外部ドロップを予約し、その他の入力をImGuiへ渡します(window: 対象ウィンドウ, message: メッセージ種別, wParam: 主パラメーター, lParam: 補助パラメーター)。
    bool EditorLayer::HandleMessage(
        const HWND window,
        const UINT message,
        const WPARAM wParam,
        const LPARAM lParam) const
    {
        if (message == WM_DROPFILES)
        {
            // 解放が必要なドロップ情報
            const auto drop = reinterpret_cast<HDROP>(wParam);
            // 予約する外部ドロップ情報
            PendingExternalAssetDrop pending;
            // ウィンドウ内の受付座標
            POINT clientPosition{};
            if (DragQueryPoint(drop, &clientPosition))
            {
                pending.screenPosition = clientPosition;
                ClientToScreen(
                    window,
                    &pending.screenPosition);
            }

            try
            {
                // 外部ドロップのファイル数
                const UINT fileCount = DragQueryFileW(
                    drop,
                    0xFFFFFFFF,
                    nullptr,
                    0);
                pending.sources.reserve(fileCount);
                // 列挙するファイル番号
                for (UINT index = 0; index < fileCount; ++index)
                {
                    // ファイルパスの文字数
                    const UINT length = DragQueryFileW(
                        drop,
                        index,
                        nullptr,
                        0);
                    // ドロップ元のファイルパス
                    std::wstring path(length + 1, L'\0');
                    if (DragQueryFileW(
                            drop,
                            index,
                            path.data(),
                            static_cast<UINT>(path.size()))
                        != 0)
                    {
                        path.resize(length);
                        pending.sources.emplace_back(
                            std::move(path));
                    }
                }
                if (!pending.sources.empty())
                {
                    m_pendingExternalAssetDrops.push_back(
                        std::move(pending));
                }
            }
            // ドロップ列挙が失敗してもハンドルを解放します。
            catch (...)
            {
            }
            DragFinish(drop);
            return true;
        }
        return ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam) != 0;
    }

    // リモート入力を注入してImGuiフレームを開始し、段階読込と初回の撮影指示を処理します。
    void EditorLayer::BeginFrame()
    {
        m_editorGuiRenderer->NewFrame();
        ImGui_ImplWin32_NewFrame();
        // このフレームへ反映するリモート入力はImGui::NewFrame前に注入します。
        if (!m_screenshotRequest.remoteDirectory.empty())
        {
            // 前フレームのUI記録を確定して、次の記録を開始します。
            UiRecorder::SetEnabled(true);
            UiRecorder::NextFrame();
            // 保持位置を先に注入し、新しい位置のコマンドを優先します。
            ReapplyRemoteMousePosition();
            if (!m_remoteMacro.empty())
            {
                // 手順への入力割込を避けるため、マクロ実行中は新しいコマンドを受けません。
                RunRemoteMacro();
            }
            else
            {
                PollRemoteCommands();
            }
        }
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();

        ProcessPendingAssetImports();

        // レイアウトができた最初のフレームで撮影用UIを開きます。
        if (!m_screenshotRequest.imagePath.empty())
        {
            ++m_screenshotFrame;
            if (m_screenshotFrame == 1)
            {
                ApplyScreenshotIntent();
            }
        }
    }

    // リモートのマウス位置を保持してImGuiへ注入します(x: 画面座標X, y: 画面座標Y)。
    void EditorLayer::InjectMousePosition(
        const float x,
        const float y)
    {
        // Win32が毎フレーム報告する実カーソルより後に位置を再注入するため、保持します。
        m_remoteMouseHeld = true;
        m_remoteMouseX = x;
        m_remoteMouseY = y;
        ImGui::GetIO().AddMousePosEvent(x, y);
    }

    // Win32の実カーソル報告より後に保持した位置を注入し、リモート位置を優先します。
    void EditorLayer::ReapplyRemoteMousePosition()
    {
        if (!m_remoteMouseHeld)
        {
            return;
        }

        ImGui::GetIO().AddMousePosEvent(
            m_remoteMouseX,
            m_remoteMouseY);
    }

    // 該当フレームの手順を実行し、最終フレーム経過後にマクロを消費します。
    void EditorLayer::RunRemoteMacro()
    {
        // このフレームの実行手順
        for (const auto& step : m_remoteMacro)
        {
            if (step.frame == m_remoteMacroFrame)
            {
                step.action();
            }
        }
        ++m_remoteMacroFrame;

        // マクロの最終実行フレーム
        std::uint32_t lastFrame = 0;
        // このフレームの実行手順
        for (const auto& step : m_remoteMacro)
        {
            lastFrame = std::max(lastFrame, step.frame);
        }
        // マクロを空にするとRender側がstate.jsonを保存できるため、全手順終了後に消去します。
        if (m_remoteMacroFrame > lastFrame)
        {
            m_remoteMacro.clear();
            m_remoteMacroFrame = 0;
        }
    }

    // 再生中のリモート入力を出力して残りフレーム数を消費します(snapshot: 消費した入力の出力先)。
    bool EditorLayer::ConsumeInputSnapshot(
        InputSnapshot& snapshot) noexcept
    {
        if (!m_playing
            || m_remoteInputFrames == 0
            || !m_remoteInputSnapshot.has_value())
        {
            return false;
        }
        snapshot = std::move(*m_remoteInputSnapshot);
        --m_remoteInputFrames;
        if (m_remoteInputFrames == 0)
        {
            m_remoteInputSnapshot.reset();
        }
        return true;
    }

    // 統計・オブジェクト・入力・最新計測と最後の64件中の警告以上をJSONで返します。
    nlohmann::json EditorLayer::BuildRemoteRuntimeState() const
    {
        // 直近フレームの描画統計
        const auto& frameStats = m_graphics.FrameStats();
        // 直近フレームのメモリ統計
        const auto& memoryStats = m_graphics.MemoryStats();
        // GPU計測情報の参照元
        const auto& gpu = m_graphics.Gpu();
        // 直近のGPUパイプライン統計
        const auto& pipeline =
            gpu.LatestPipelineStatistics();
        // 直近の物理演算統計
        const auto& physics = m_scene.PhysicsStats();
        // 直近の可視性判定統計
        const auto& visibility = m_scene.VisibilityStats();

        // 遠隔操作へ返す実行状態JSON
        nlohmann::json runtimeState{
            { "playing", m_playing },
            { "paused", m_paused },
            { "scene",
                PathToUtf8(
                    m_scene.Scenes().CurrentScenePath()) },
            { "objectCount", m_scene.GameObjects().size() },
            { "time", {
                { "deltaTime", Time::DeltaTime() },
                { "unscaledDeltaTime",
                    Time::UnscaledDeltaTime() },
                { "timeSinceStartup",
                    Time::TimeSinceStartup() },
                { "unscaledTimeSinceStartup",
                    Time::UnscaledTimeSinceStartup() },
                { "frameCount", Time::FrameCount() },
                { "timeScale", Time::TimeScale() },
                { "pausedByTimeScale", Time::IsPaused() },
            } },
            { "frame", {
                { "fps", frameStats.framesPerSecond },
                { "frameTimeMilliseconds",
                    frameStats.frameTimeMilliseconds },
                { "cpuTimeMilliseconds",
                    frameStats.cpuTimeMilliseconds },
                { "gpuTimeMilliseconds",
                    gpu.LatestFrameMilliseconds() },
                { "totalFrames", frameStats.totalFrames },
                { "shaderFallbackDraws",
                    frameStats.shaderFallbackDraws },
            } },
            { "memory", {
                { "processWorkingSetBytes",
                    memoryStats.processWorkingSetBytes },
                { "processPrivateBytes",
                    memoryStats.processPrivateBytes },
                { "systemPhysicalUsedBytes",
                    memoryStats.systemPhysicalUsedBytes },
                { "systemPhysicalTotalBytes",
                    memoryStats.systemPhysicalTotalBytes },
                { "dedicatedVideoMemoryBytes",
                    memoryStats.dedicatedVideoMemoryBytes },
                { "sharedSystemMemoryBytes",
                    memoryStats.sharedSystemMemoryBytes },
                { "localVideoMemoryUsageBytes",
                    memoryStats.localVideoMemoryUsageBytes },
                { "localVideoMemoryBudgetBytes",
                    memoryStats.localVideoMemoryBudgetBytes },
                { "nonLocalVideoMemoryUsageBytes",
                    memoryStats.nonLocalVideoMemoryUsageBytes },
                { "nonLocalVideoMemoryBudgetBytes",
                    memoryStats.nonLocalVideoMemoryBudgetBytes },
                { "videoMemoryBudgetAvailable",
                    memoryStats.videoMemoryBudgetAvailable },
            } },
            { "gpuPipeline", {
                { "valid", pipeline.valid },
                { "inputAssemblerVertices",
                    pipeline.inputAssemblerVertices },
                { "inputAssemblerPrimitives",
                    pipeline.inputAssemblerPrimitives },
                { "vertexShaderInvocations",
                    pipeline.vertexShaderInvocations },
                { "pixelShaderInvocations",
                    pipeline.pixelShaderInvocations },
                { "hullShaderInvocations",
                    pipeline.hullShaderInvocations },
                { "domainShaderInvocations",
                    pipeline.domainShaderInvocations },
                { "geometryShaderInvocations",
                    pipeline.geometryShaderInvocations },
                { "computeShaderInvocations",
                    pipeline.computeShaderInvocations },
            } },
            { "physics", {
                { "colliderCount2D", physics.colliderCount2D },
                { "colliderCount3D", physics.colliderCount3D },
                { "candidatePairCount2D",
                    physics.candidatePairCount2D },
                { "candidatePairCount3D",
                    physics.candidatePairCount3D },
                { "narrowPhaseTestCount2D",
                    physics.narrowPhaseTestCount2D },
                { "narrowPhaseTestCount3D",
                    physics.narrowPhaseTestCount3D },
                { "activeContactCount",
                    physics.activeContactCount },
                { "fixedStepsLastFrame",
                    m_scene.PhysicsFixedStepsLastFrame() },
            } },
            { "visibility", {
                { "rendererCount", visibility.rendererCount },
                { "visibleRendererCount",
                    visibility.visibleRendererCount },
                { "frustumCulledCount",
                    visibility.frustumCulledCount },
                { "occlusionCulledCount",
                    visibility.occlusionCulledCount },
                { "lodCulledCount", visibility.lodCulledCount },
                { "automaticLodRendererCount",
                    visibility.automaticLodRendererCount },
                { "automaticLodTrianglesSaved",
                    visibility.automaticLodTrianglesSaved },
                { "meshInstanceBatchCount",
                    visibility.meshInstanceBatchCount },
                { "meshInstancedRendererCount",
                    visibility.meshInstancedRendererCount },
                { "modelInstanceBatchCount",
                    visibility.modelInstanceBatchCount },
                { "modelInstancedRendererCount",
                    visibility.modelInstancedRendererCount },
                { "spatialNodeCount",
                    visibility.spatialNodeCount },
                { "spatialNodeTestCount",
                    visibility.spatialNodeTestCount },
                { "spatialIndexReused",
                    visibility.spatialIndexReused },
            } },
        };

        // キー順で出力するゲーム状態
        auto stateValues = nlohmann::json::object();
        // ゲーム状態値のスナップショット
        auto values = m_scene.Scenes().State().Snapshot();
        // キー名で昇順に並べます(left: 比較元の状態値, right: 比較先の状態値)。
        std::ranges::sort(
            values,
            [](const auto& left, const auto& right)
            {
                return left.first < right.first;
            });
        // key: 状態キー、value: 型付き状態値
        for (const auto& [key, value] : values)
        {
            // 型を保ってJSONへ格納します(item: 状態値の内容)。
            std::visit(
                [&stateValues, &key](const auto& item)
                {
                    stateValues[key] = item;
                },
                value);
        }
        runtimeState["gameState"] = std::move(stateValues);

        // 全オブジェクトの出力JSON
        auto objects = nlohmann::json::array();
        // 実行状態を出力する対象
        for (const auto& object : m_scene.GameObjects())
        {
            if (object == nullptr)
            {
                continue;
            }
            // 出力対象のローカル変換
            const auto& transform = object->GetTransform();
            // 出力するEuler角・rad
            const auto euler = transform.EulerAngles();
            // 対象のコンポーネント一覧
            auto components = nlohmann::json::array();
            // 出力するコンポーネント
            for (const auto& component : object->Components())
            {
                if (component == nullptr)
                {
                    continue;
                }
                components.push_back({
                    { "type", std::string(component->TypeName()) },
                    { "enabled", component->IsEnabled() },
                    { "activeAndEnabled",
                        component->IsActiveAndEnabled() },
                });
            }
            objects.push_back({
                { "id", object->Id() },
                { "name", object->Name() },
                { "tag", object->Tag() },
                { "enabled", object->IsEnabled() },
                { "activeInHierarchy",
                    object->IsActiveInHierarchy() },
                { "parentId",
                    object->Parent() == nullptr
                        ? 0
                        : object->Parent()->Id() },
                { "sourceScene", object->SourceScene() },
                { "position", {
                    transform.position.x,
                    transform.position.y,
                    transform.position.z,
                } },
                { "rotationEulerRadians", {
                    euler.x,
                    euler.y,
                    euler.z,
                } },
                { "rotationQuaternion", {
                    transform.rotationQuaternion.x,
                    transform.rotationQuaternion.y,
                    transform.rotationQuaternion.z,
                    transform.rotationQuaternion.w,
                } },
                { "scale", {
                    transform.scale.x,
                    transform.scale.y,
                    transform.scale.z,
                } },
                { "components", std::move(components) },
            });
        }
        runtimeState["objects"] = std::move(objects);

        // 追加読込したシーンの一覧
        auto loadedScenes = nlohmann::json::array();
        // 追加シーンの登録情報
        for (const auto& loaded : m_scene.AdditiveScenes())
        {
            loadedScenes.push_back({
                { "handle", loaded.handle },
                { "path", PathToUtf8(loaded.path) },
                { "name", loaded.name },
                { "rootCount", loaded.rootCount },
            });
        }
        runtimeState["additiveScenes"] = std::move(loadedScenes);

        // 入力アクションの状態一覧
        auto actions = nlohmann::json::array();
        // 出力する入力アクション
        for (const auto& action : m_graphics.Input().Actions())
        {
            // アクションの入力割当一覧
            auto bindings = nlohmann::json::array();
            // 出力する入力割当
            for (const auto& binding : action.bindings)
            {
                bindings.push_back({
                    { "control",
                        std::string(
                            InputControlName(binding.control)) },
                    { "scale", binding.scale },
                });
            }
            actions.push_back({
                { "name", action.name },
                { "bindings", std::move(bindings) },
                { "value",
                    m_graphics.Input().Value(action.name) },
                { "down",
                    m_graphics.Input().IsDown(action.name) },
                { "pressed",
                    m_graphics.Input().WasPressed(action.name) },
                { "released",
                    m_graphics.Input().WasReleased(action.name) },
            });
        }
        runtimeState["input"] = std::move(actions);

        // 保存済みの計測フレーム
        const auto profileFrames = Profiler::Instance().Snapshot();
        if (!profileFrames.empty())
        {
            // 最新の計測フレーム
            const auto& profile = profileFrames.back();
            // 最新フレームの計測区間一覧
            auto samples = nlohmann::json::array();
            // 出力する計測区間
            for (const auto& sample : profile.samples)
            {
                // 計測区間の出力JSON
                auto entry = nlohmann::json{
                    { "name", sample.name },
                    { "milliseconds", sample.milliseconds },
                    { "calls", sample.callCount },
                    { "depth", sample.depth },
                };
                // 最上位区間はparentを省略し、従来の読み手と同じ形にします。
                if (sample.parent != ProfileSample::NoParent)
                {
                    entry["parent"] = sample.parent;
                }
                samples.push_back(std::move(entry));
            }
            runtimeState["profiler"] = {
                { "enabled", Profiler::Instance().IsEnabled() },
                { "frameIndex", profile.index },
                { "milliseconds", profile.milliseconds },
                { "samples", std::move(samples) },
            };
        }
        else
        {
            runtimeState["profiler"] = {
                { "enabled", Profiler::Instance().IsEnabled() },
                { "frameIndex", 0 },
                { "milliseconds", 0.0 },
                { "samples", nlohmann::json::array() },
            };
        }

        // 警告・エラーの出力一覧
        auto logs = nlohmann::json::array();
        // 保存済みログのスナップショット
        const auto logEntries = Logger::Instance().Snapshot();
        // 最後の64件の開始位置
        const auto firstLog = logEntries.size() > 64
            ? logEntries.end() - 64
            : logEntries.begin();
        // 出力候補のログ位置
        for (auto iterator = firstLog;
            iterator != logEntries.end();
            ++iterator)
        {
            if (iterator->level == LogLevel::Info)
            {
                continue;
            }
            logs.push_back({
                { "sequence", iterator->sequence },
                { "level",
                    std::string(LogLevelName(iterator->level)) },
                { "message", iterator->message },
                { "gameObjectId", iterator->gameObjectId },
            });
        }
        runtimeState["logs"] = std::move(logs);
        return runtimeState;
    }

    // 未処理の指示JSONを順に実行し、UI入力マクロと描画後の結果出力を予約します。
    void EditorLayer::PollRemoteCommands()
    {
        // 遠隔操作の指示ファイル
        const auto commandPath =
            m_screenshotRequest.remoteDirectory
            / L"command.json";
        // 指示JSONの入力ストリーム
        std::ifstream input(
            commandPath,
            std::ios::binary);
        if (!input)
        {
            return;
        }
        // 受信した指示JSON
        nlohmann::json document =
            nlohmann::json::parse(
                input,
                nullptr,
                false);
        // 書込途中のJSONは無視し、次のフレームで再読込します。
        if (document.is_discarded()
            || !document.is_object())
        {
            return;
        }
        // 新しい指示の通し番号
        const auto sequence =
            document.value<std::uint64_t>("seq", 0);
        if (sequence == 0
            || sequence == m_remoteLastSequence)
        {
            return;
        }
        // 実行前にseqを消費するため、途中で失敗しても同じseqの指示は再実行されません。
        m_remoteLastSequence = sequence;
        m_remoteReportSequence = sequence;
        m_remoteReportPending = true;
        m_remoteReportError.clear();

        // 入力を注入するImGui状態
        auto& io = ImGui::GetIO();
        // 順に処理する指示一覧
        const auto commands =
            document.value(
                "commands",
                nlohmann::json::array());
        // 処理する遠隔操作指示
        for (const auto& command : commands)
        {
            // 指示の操作種別
            const std::string type =
                command.value("type", std::string{});
            if (type == "play")
            {
                if (m_playing)
                {
                    m_remoteReportError =
                        "play requested while the game is already playing.";
                }
                else
                {
                    StartPlaying();
                }
            }
            else if (type == "pause")
            {
                if (!m_playing)
                {
                    m_remoteReportError =
                        "pause requires the game to be playing.";
                }
                else
                {
                    SetPaused(true);
                }
            }
            else if (type == "resume")
            {
                if (!m_playing)
                {
                    m_remoteReportError =
                        "resume requires the game to be playing.";
                }
                else
                {
                    SetPaused(false);
                }
            }
            else if (type == "step")
            {
                if (!m_playing || !m_paused)
                {
                    m_remoteReportError =
                        "step requires a paused game.";
                }
                else
                {
                    RequestSimulationStep();
                }
            }
            else if (type == "runtime"
                || type == "observe")
            {

                m_remoteRuntimePending = true;
            }
            else if (type == "timescale")
            {
                if (!command.contains("value")
                    || !command.at("value").is_number())
                {
                    m_remoteReportError =
                        "timescale requires a numeric value.";
                }
                else
                {
                    // 指示された入力・設定値
                    const float value =
                        command.at("value").get<float>();
                    if (!std::isfinite(value))
                    {
                        m_remoteReportError =
                            "timescale must be finite.";
                    }
                    else
                    {
                        Time::SetTimeScale(value);
                    }
                }
            }
            else if (type == "input")
            {
                if (!m_playing)
                {
                    m_remoteReportError =
                        "input requires the game to be playing.";
                    continue;
                }
                if (!command.contains("value")
                    || !command.at("value").is_number())
                {
                    m_remoteReportError =
                        "input requires a numeric value.";
                    continue;
                }
                // 指示された入力・設定値
                const float value =
                    command.at("value").get<float>();
                if (!std::isfinite(value))
                {
                    m_remoteReportError =
                        "input value must be finite.";
                    continue;
                }
                // 解決した物理入力コントロール
                InputControl control{};
                // 割当倍率を戻した入力値
                float controlValue = value;
                // 入力先の解決に成功した
                bool resolved = false;
                if (command.contains("control")
                    && command.at("control").is_string())
                {
                    try
                    {
                        control = InputControlFromName(
                            command.at("control")
                                .get<std::string>());
                        resolved = true;
                    }
                    catch (const std::exception&)
                    {
                        m_remoteReportError =
                            "unknown input control: "
                            + command.at("control")
                                .get<std::string>();
                    }
                }
                else if (command.contains("action")
                    && command.at("action").is_string())
                {
                    // 指定された入力アクション名
                    const auto actionName =
                        command.at("action").get<std::string>();
                    // 指定名の入力先を探します(candidate: 登録入力アクション)。
                    const auto action = std::find_if(
                        m_graphics.Input().Actions().begin(),
                        m_graphics.Input().Actions().end(),
                        [&actionName](const auto& candidate)
                        {
                            return candidate.name == actionName;
                        });
                    if (action == m_graphics.Input().Actions().end())
                    {
                        m_remoteReportError =
                            "unknown input action: " + actionName;
                    }
                    else
                    {
                        // 倍率が有効な最初の割当を探します(candidate: 入力割当)。
                        const auto binding = std::find_if(
                            action->bindings.begin(),
                            action->bindings.end(),
                            [](const auto& candidate)
                            {
                                return std::abs(candidate.scale)
                                    > 1.0e-6f;
                            });
                        if (binding == action->bindings.end())
                        {
                            m_remoteReportError =
                                "input action has no usable binding: "
                                + actionName;
                        }
                        else
                        {
                            control = binding->control;
                            controlValue = std::clamp(
                                value / binding->scale,
                                -1.0f,
                                1.0f);
                            resolved = true;
                        }
                    }
                }
                else
                {
                    m_remoteReportError =
                        "input requires control or action.";
                }
                if (!resolved)
                {
                    continue;
                }
                if (!m_remoteInputSnapshot.has_value())
                {
                    m_remoteInputSnapshot.emplace();
                }
                m_remoteInputSnapshot->Set(
                    control,
                    std::clamp(controlValue, -1.0f, 1.0f));
                // 入力を保持するフレーム数
                const auto frames = std::clamp(
                    command.value("frames", 1u),
                    1u,
                    600u);
                m_remoteInputFrames = std::max(
                    m_remoteInputFrames,
                    frames);
            }
            else if (type == "move" || type == "click")
            {
                // 入力する画面座標X
                const float x =
                    command.value("x", 0.0f);
                // 入力する画面座標Y
                const float y =
                    command.value("y", 0.0f);
                InjectMousePosition(x, y);
                if (type == "click")
                {
                    // 操作するマウスボタン番号
                    const int button =
                        command.value("button", 0);
                    io.AddMouseButtonEvent(button, true);
                    io.AddMouseButtonEvent(button, false);
                    if (command.value("double", false))
                    {
                        io.AddMouseButtonEvent(
                            button,
                            true);
                        io.AddMouseButtonEvent(
                            button,
                            false);
                    }
                }
            }
            else if (type == "click-label")
            {

                // 検索するUIラベル
                const std::string label =
                    command.value("label", std::string{});
                // 絞り込むウィンドウ名
                const std::string window =
                    command.value("window", std::string{});
                // 直前の完成フレームのUI記録
                const auto items = UiRecorder::Snapshot();
                // 操作対象のUI記録への参照
                const UiRecorder::Item* match = nullptr;
                // ラベル照合するUI項目
                for (const auto& item : items)
                {
                    if (!window.empty()
                        && item.window.find(window)
                            == std::string::npos)
                    {
                        continue;
                    }
                    if (item.label == label)
                    {
                        match = &item;
                        break;
                    }
                    // 完全一致を優先し、なければ最初の部分一致を使います。
                    if (match == nullptr
                        && item.label.find(label)
                            != std::string::npos)
                    {
                        match = &item;
                    }
                }
                if (match != nullptr)
                {
                    // 入力する画面座標X
                    const float x =
                        match->x + match->width * 0.5f;
                    // 入力する画面座標Y
                    const float y =
                        match->y + match->height * 0.5f;
                    InjectMousePosition(x, y);
                    io.AddMouseButtonEvent(0, true);
                    io.AddMouseButtonEvent(0, false);
                }
                else
                {
                    m_remoteReportError =
                        "label not found: " + label;
                }
            }
            else if (type == "dump")
            {
                // all指定ではラベルのないUIも矩形付きで出力します。
                m_remoteDumpPending = true;
                m_remoteDumpAll =
                    command.value("all", false);
            }
            else if (type == "set-value")
            {
                // 値の指定はラベルまたは座標で対象を選び、真偽値はクリック、その他は入力マクロで設定します。
                if (!command.contains("value"))
                {
                    m_remoteReportError =
                        "set-value requires a value.";
                    continue;
                }
                // 入力する画面座標X
                float x{};
                // 入力する画面座標Y
                float y{};
                // 操作対象のUI記録への参照
                const UiRecorder::Item* match = nullptr;
                if (command.contains("x")
                    && command.contains("y"))
                {
                    x = command.value("x", 0.0f);
                    y = command.value("y", 0.0f);
                }
                else
                {
                    // 検索するUIラベル
                    const std::string label =
                        command.value(
                            "label",
                            std::string{});
                    // 絞り込むウィンドウ名
                    const std::string window =
                        command.value(
                            "window",
                            std::string{});
                    // 直前の完成フレームのUI記録
                    const auto items =
                        UiRecorder::Snapshot();
                    // ラベル照合するUI項目
                    for (const auto& item : items)
                    {
                        if (!window.empty()
                            && item.window.find(window)
                                == std::string::npos)
                        {
                            continue;
                        }
                        if (item.label == label)
                        {
                            match = &item;
                            break;
                        }
                        if (match == nullptr
                            && item.label.find(label)
                                != std::string::npos)
                        {
                            match = &item;
                        }
                    }
                    if (match == nullptr)
                    {
                        m_remoteReportError =
                            "label not found: " + label;
                        continue;
                    }
                    x = match->x + match->width * 0.5f;
                    y = match->y + match->height * 0.5f;
                }
                // ラベル指定のmatchは内側のitemsを参照し、そのスコープ終了後は参照先が失効します。
                // 指示された入力・設定値
                const auto& value = command.at("value");
                if (value.is_boolean())
                {
                    // 記録したCheckedフラグは1<<23で、ラベル指定時だけ現在値との差を判定できます。
                    if (match != nullptr)
                    {
                        // 対象の現在のチェック状態
                        const bool checked =
                            (match->statusFlags
                                & (1u << 23)) != 0;
                        if (checked == value.get<bool>())
                        {
                            continue;
                        }
                    }
                    InjectMousePosition(x, y);
                    io.AddMouseButtonEvent(0, true);
                    io.AddMouseButtonEvent(0, false);
                    continue;
                }
                // 値を入力するUTF8文字列
                const std::string text =
                    value.is_string()
                        ? value.get<std::string>()
                        : value.dump();
                m_remoteMacroFrame = 0;
                // Ctrlクリックで入力を開始し、全選択・値入力・Enter確定を順に実行します。
                m_remoteMacro = {
                    { 0, [this, x, y]
                        {
                            InjectMousePosition(x, y);
                            ImGui::GetIO().AddKeyEvent(
                                ImGuiMod_Ctrl, true);
                            ImGui::GetIO()
                                .AddMouseButtonEvent(
                                    0, true);
                        } },
                    { 2, []
                        {
                            ImGui::GetIO()
                                .AddMouseButtonEvent(
                                    0, false);
                        } },
                    { 4, []
                        {
                            ImGui::GetIO().AddKeyEvent(
                                ImGuiMod_Ctrl, false);
                        } },
                    { 6, []
                        {
                            // 入力を注入するImGui状態
                            auto& inputOutput =
                                ImGui::GetIO();
                            inputOutput.AddKeyEvent(
                                ImGuiMod_Ctrl, true);
                            inputOutput.AddKeyEvent(
                                ImGuiKey_A, true);
                        } },
                    { 8, []
                        {
                            // 入力を注入するImGui状態
                            auto& inputOutput =
                                ImGui::GetIO();
                            inputOutput.AddKeyEvent(
                                ImGuiKey_A, false);
                            inputOutput.AddKeyEvent(
                                ImGuiMod_Ctrl, false);
                        } },
                    { 10, [text]
                        {
                            ImGui::GetIO()
                                .AddInputCharactersUTF8(
                                    text.c_str());
                        } },
                    { 12, []
                        {
                            ImGui::GetIO().AddKeyEvent(
                                ImGuiKey_Enter, true);
                        } },
                    { 14, []
                        {
                            ImGui::GetIO().AddKeyEvent(
                                ImGuiKey_Enter, false);
                        } },
                };
            }
            else if (type == "drag")
            {

                // ドラッグ始点の画面座標X
                const float fromX =
                    command.value("x", 0.0f);
                // ドラッグ始点の画面座標Y
                const float fromY =
                    command.value("y", 0.0f);
                // ドラッグ終点の画面座標X
                const float toX =
                    command.value("toX", fromX);
                // ドラッグ終点の画面座標Y
                const float toY =
                    command.value("toY", fromY);
                // 操作するマウスボタン番号
                const int button =
                    command.value("button", 0);
                // ドラッグで移動するフレーム数
                const std::uint32_t moveFrames =
                    std::clamp(
                        command.value("frames", 10u),
                        2u,
                        120u);
                m_remoteMacroFrame = 0;
                m_remoteMacro.clear();
                // 始点へ移動してボタンを押し、ドラッグを開始します。
                m_remoteMacro.push_back(
                    { 0, [this, fromX, fromY, button]
                        {
                            InjectMousePosition(
                                fromX, fromY);
                            ImGui::GetIO()
                                .AddMouseButtonEvent(
                                    button, true);
                        } });
                // ドラッグ移動の段階番号
                for (std::uint32_t step = 1;
                    step <= moveFrames;
                    ++step)
                {
                    // 始点から終点への移動比率
                    const float ratio =
                        static_cast<float>(step)
                        / static_cast<float>(moveFrames);
                    // 入力する画面座標X
                    const float x =
                        fromX + (toX - fromX) * ratio;
                    // 入力する画面座標Y
                    const float y =
                        fromY + (toY - fromY) * ratio;
                    // 同フレームの移動をドラッグと認識しないUIがあるため、押下の次フレームは待ちます。
                    m_remoteMacro.push_back(
                        { step + 1, [this, x, y]
                            {
                                InjectMousePosition(x, y);
                            } });
                }
                // 終点に到達した後でボタンを離します。
                m_remoteMacro.push_back(
                    { moveFrames + 3, [button]
                        {
                            ImGui::GetIO()
                                .AddMouseButtonEvent(
                                    button, false);
                        } });
            }
            else if (type == "wheel")
            {

                // 入力する画面座標X
                const float x =
                    command.value("x", 0.0f);
                // 入力する画面座標Y
                const float y =
                    command.value("y", 0.0f);
                InjectMousePosition(x, y);
                io.AddMouseWheelEvent(
                    command.value("deltaX", 0.0f),
                    command.value("deltaY", 0.0f));
            }
            else if (type == "text")
            {
                io.AddInputCharactersUTF8(
                    command.value(
                        "value",
                        std::string{}).c_str());
            }
            else if (type == "key")
            {
                // 注入するキーの指定名
                const std::string name =
                    command.value(
                        "value",
                        std::string{});
                // 指定名に対応するImGuiキー
                ImGuiKey key = ImGuiKey_None;
                if (name == "enter") { key = ImGuiKey_Enter; }
                else if (name == "tab") { key = ImGuiKey_Tab; }
                else if (name == "escape") { key = ImGuiKey_Escape; }
                else if (name == "backspace") { key = ImGuiKey_Backspace; }
                else if (name == "delete") { key = ImGuiKey_Delete; }
                else if (name == "space") { key = ImGuiKey_Space; }
                else if (name == "up") { key = ImGuiKey_UpArrow; }
                else if (name == "down") { key = ImGuiKey_DownArrow; }
                else if (name == "left") { key = ImGuiKey_LeftArrow; }
                else if (name == "right") { key = ImGuiKey_RightArrow; }
                if (key != ImGuiKey_None)
                {
                    io.AddKeyEvent(key, true);
                    io.AddKeyEvent(key, false);
                }
                else
                {
                    m_remoteReportError =
                        "unknown key: " + name;
                }
            }
            else if (type == "screenshot")
            {
                // 描画後に撮影し、古い内容を再取得しないようseqを含むファイル名を使います。
                m_remotePendingShot =
                    m_screenshotRequest.remoteDirectory
                    / (L"screenshot-"
                        + std::to_wstring(sequence)
                        + L".png");
            }
            else if (type == "quit")
            {
                WriteRemoteState();
                PostQuitMessage(0);
            }
            else
            {
                m_remoteReportError =
                    "unknown command: " + type;
            }
        }
    }

    // 応答予約を消費し、指示の結果・要求されたUI一覧・実行状態をstate.jsonへ書きます。
    void EditorLayer::WriteRemoteState()
    {
        if (!m_remoteReportPending)
        {
            return;
        }
        // 書込結果は検査されず、予約は書込前に消費されるため、失敗時の自動再送はありません。
        m_remoteReportPending = false;
        // 遠隔操作の結果JSON
        nlohmann::json state{
            { "seq", m_remoteReportSequence },
            { "ok", m_remoteReportError.empty() },
        };
        if (!m_remoteReportError.empty())
        {
            state["error"] = m_remoteReportError;
        }
        if (!m_remotePendingShot.empty())
        {
            state["screenshot"] =
                PathToUtf8(m_remotePendingShot);
        }
        if (m_remoteDumpPending)
        {
            m_remoteDumpPending = false;
            // 報告する可視UI項目一覧
            auto items = nlohmann::json::array();
            // 報告する可視UI項目
            for (const auto& item :
                UiRecorder::Snapshot(m_remoteDumpAll))
            {
                items.push_back({
                    { "window", item.window },
                    { "label", item.label },
                    { "x", item.x },
                    { "y", item.y },
                    { "w", item.width },
                    { "h", item.height },
                    { "flags", item.statusFlags },
                });
            }
            state["items"] = std::move(items);
        }
        if (m_remoteRuntimePending)
        {
            m_remoteRuntimePending = false;
            state["runtime"] = BuildRemoteRuntimeState();
        }
        // 応答JSONの出力ストリーム
        std::ofstream output(
            m_screenshotRequest.remoteDirectory
                / L"state.json",
            std::ios::trunc);
        output << state.dump(
            2,
            ' ',
            false,
            nlohmann::json::error_handler_t::replace);
    }

    // 撮影指定から画面・設定カテゴリー・選択対象を開き、必要なら末尾スクロールを予約します。
    void EditorLayer::ApplyScreenshotIntent()
    {
        // 撮影前に開くUIの指定
        std::string show = m_screenshotRequest.show;
        if (show == "export-windows" || show == "export-web")
        {
            OpenGameExportDialog();
            m_gameExportDialog->SelectTarget(show == "export-web"
                ? GameExportTarget::Web : GameExportTarget::Windows);
            return;
        }
        if (show == "help")
        {
            OpenHelpCenter();
            return;
        }
        if (show.empty())
        {
            return;
        }

        // 末尾スクロール指定の接尾辞
        constexpr std::string_view bottomSuffix{
            ":bottom" };
        if (show.size() > bottomSuffix.size()
            && show.ends_with(bottomSuffix))
        {
            m_screenshotScrollToBottom = true;
            show.resize(
                show.size() - bottomSuffix.size());
        }
        // 設定カテゴリー指定の接頭辞
        constexpr std::string_view settingsPrefix{
            "project-settings:" };
        // 選択オブジェクト指定の接頭辞
        constexpr std::string_view inspectorPrefix{
            "inspector:" };

        // 登録パネル指定の接頭辞
        constexpr std::string_view panelPrefix{ "panel:" };
        if (show.starts_with(panelPrefix))
        {
            // 開く登録パネルのID
            const std::string panelId =
                show.substr(panelPrefix.size());
            if (!m_editorExtensions.SetPanelOpen(panelId, true))
            {
                Logger::Instance().Warning(
                    "スクリーンショット対象のパネルが見つかりません: "
                    + panelId);
            }
            return;
        }
        if (show.starts_with(settingsPrefix))
        {

            // 設定画面の順序で並ぶ日本語名
            constexpr std::array<const char*, 10>
                categories{
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
            // 同じ順序の英語カテゴリー名
            constexpr std::array<const char*, 10>
                aliases{
                    "game",
                    "graphics",
                    "viewport",
                    "physics",
                    "tags",
                    "input",
                    "scripts",
                    "build",
                    "online",
                    "services"
                };
            // 開く設定カテゴリー名
            const std::string category =
                show.substr(settingsPrefix.size());
            // 設定カテゴリーの番号
            for (std::size_t index = 0;
                index < categories.size();
                ++index)
            {
                if (category == categories[index]
                    || category == aliases[index])
                {
                    m_projectSettingsCategory =
                        static_cast<int>(index);
                    break;
                }
            }
            OpenProjectSettingsDialog();
            return;
        }
        if (show.starts_with(inspectorPrefix))
        {
            // 選択するオブジェクト名
            const std::string name =
                show.substr(inspectorPrefix.size());
            // 撮影時に選択する対象
            if (const auto* target =
                    m_scene.FindGameObjectByName(name))
            {
                m_selectedObjectId = target->Id();
            }
            else
            {
                Logger::Instance().Warning(
                    "スクリーンショット対象のGameObjectが"
                    "見つかりません: "
                    + name);
            }
            return;
        }
        Logger::Instance().Warning(
            "スクリーンショットの--showを解釈できません: "
            + show);
    }

    // バックバッファをPNGへ保存し、撮影結果の報告後に終了を要求します。
    void EditorLayer::CaptureScreenshotAndQuit()
    {
        // 撮影結果の応答JSON
        nlohmann::json report{
            { "ok", false },
            { "command", "editor-screenshot" },
            { "show", m_screenshotRequest.show },
            { "image",
                PathToUtf8(
                    m_screenshotRequest.imagePath) },
        };
        try
        {
            // 撮影画像の幅・pixel
            std::uint32_t width{};
            // 撮影画像の高さ・pixel
            std::uint32_t height{};
            // バックバッファの画像データ
            const auto pixels =
                m_graphics.CaptureBackBuffer(
                    width,
                    height);
            SavePng(
                m_screenshotRequest.imagePath,
                width,
                height,
                pixels);
            report["ok"] = true;
            report["width"] = width;
            report["height"] = height;
        }
        // 撮影結果へ返す保存エラー
        catch (const std::exception& exception)
        {
            report["error"] = exception.what();
        }
        if (!m_screenshotRequest.reportPath.empty())
        {
            // 撮影結果JSONの出力先
            std::ofstream output(
                m_screenshotRequest.reportPath,
                std::ios::trunc);
            output << report.dump(
                2,
                ' ',
                false,
                nlohmann::json::error_handler_t::
                    replace);
        }
        // 撮影モードは保存確認を経ずに終了を要求します。
        PostQuitMessage(report["ok"].get<bool>() ? 0 : 1);
    }

    // 更新と編集画面を描画し、GameModuleのビルド中は入力とUI操作を止めます。
    void EditorLayer::Draw()
    {
        UpdateGameModuleBuild();
        UpdateExternalSceneFile();
        UpdateExternalProjectSettings();
        UpdateProjectMenus();
        UpdateScriptAutoBuild();
        m_editorExtensions.Update();
        // GameModuleビルドで操作禁止
        const bool editorLocked =
            m_gameModuleBuildProcess != nullptr;
        if (editorLocked)
        {
            // 消去するUIの入力状態
            auto& inputOutput = ImGui::GetIO();
            inputOutput.ClearInputKeys();
            inputOutput.ClearInputMouse();
        }

        m_graphics.Input().SetPointerOverride(
            InputPointerState{});
        if (!m_playing)
        {
            // プレビューの経過秒・最大0.05
            const float deltaTime =
                std::min(
                    ImGui::GetIO().DeltaTime,
                    0.05f);
            // 編集時に更新する対象
            for (const auto& object :
                m_scene.GameObjects())
            {
                if (object->IsEnabled())
                {
                    // 編集時に動かす粒子
                    if (auto* particles =
                        object->GetComponent<
                            ParticleSystemComponent>();
                        particles != nullptr
                        && particles->IsEnabled())
                    {
                        particles->UpdatePreview(
                            deltaTime);
                    }

                    // 編集時に反映するUI配置
                    if (auto* layoutGroup =
                        object->GetComponent<
                            UILayoutGroupComponent>();
                        layoutGroup != nullptr
                        && layoutGroup->IsEnabled())
                    {
                        layoutGroup->ApplyLayout();
                    }
                }
            }
        }

        ImGui::BeginDisabled(editorLocked);
        DrawToolbar();
        DrawDockSpace();
        DrawRegisteredPanels();
        DrawProjectPanels();
        DrawHierarchy();
        DrawViewport();
        DrawInspector();
        DrawPackageBuildDialog();
        DrawAnimationTimeline();
        DrawAnimatorControllerGraph();
        DrawProjectSettingsDialog();
        DrawGameExportDialog();
        DrawHelpCenter();
        ImGui::EndDisabled();

        DrawGameModuleBuildOverlay();
    }

    // ビルド中に操作を覆う画面へ回転表示と経過時間を描画します。
    void EditorLayer::DrawGameModuleBuildOverlay()
    {
        if (m_gameModuleBuildProcess == nullptr)
        {
            return;
        }

        // 操作を覆う主ビューポート
        const ImGuiViewport* viewport =
            ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::SetNextWindowFocus();

        // 操作を遮る画面の表示フラグ
        constexpr ImGuiWindowFlags overlayFlags =
            ImGuiWindowFlags_NoDecoration
            | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoDocking
            | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoNav;
        ImGui::PushStyleVar(
            ImGuiStyleVar_WindowPadding,
            ImVec2{});
        ImGui::PushStyleColor(
            ImGuiCol_WindowBg,
            ImVec4{ 0.01f, 0.015f, 0.025f, 0.76f });
        ImGui::Begin(
            "##GameModuleBuildOverlay",
            nullptr,
            overlayFlags);

        // ビルド状況カードの幅
        const float cardWidth = std::clamp(
            viewport->Size.x - 40.0f,
            320.0f,
            480.0f);
        // ビルド状況カードの高さ
        constexpr float cardHeight = 224.0f;
        ImGui::SetCursorPos(ImVec2{
            std::max(
                (viewport->Size.x - cardWidth) * 0.5f,
                0.0f),
            std::max(
                (viewport->Size.y - cardHeight) * 0.5f,
                0.0f)
        });

        ImGui::PushStyleVar(
            ImGuiStyleVar_ChildRounding,
            10.0f);
        ImGui::PushStyleVar(
            ImGuiStyleVar_ChildBorderSize,
            1.0f);
        ImGui::PushStyleVar(
            ImGuiStyleVar_WindowPadding,
            ImVec2{ 24.0f, 20.0f });
        ImGui::PushStyleColor(
            ImGuiCol_ChildBg,
            ImVec4{ 0.075f, 0.09f, 0.13f, 1.0f });
        ImGui::PushStyleColor(
            ImGuiCol_Border,
            ImVec4{ 0.25f, 0.58f, 0.95f, 0.9f });
        ImGui::BeginChild(
            "##GameModuleBuildCard",
            ImVec2{ cardWidth, cardHeight },
            ImGuiChildFlags_Borders,
            ImGuiWindowFlags_NoScrollbar);

        // カード中央へ文を表示します(text: 表示する文字列)。
        const auto centeredText =
            [cardWidth](const char* text)
            {
                // 中央表示する文の横幅
                const float textWidth =
                    ImGui::CalcTextSize(text).x;
                ImGui::SetCursorPosX(std::max(
                    (cardWidth - textWidth) * 0.5f,
                    0.0f));
                ImGui::TextUnformatted(text);
            };

        centeredText("C++ Scriptをビルドしています");
        ImGui::Dummy(ImVec2{ 0.0f, 10.0f });

        // 回転表示に並べる点の数
        constexpr int spinnerDotCount = 12;
        // 回転表示の半径
        constexpr float spinnerRadius = 18.0f;
        // 回転表示の経過秒
        const double animationTime = ImGui::GetTime();
        // 回転表示の先頭点番号
        const int spinnerPhase = static_cast<int>(
            animationTime * 12.0)
            % spinnerDotCount;
        // 回転表示の中心座標
        const ImVec2 spinnerCenter{
            ImGui::GetWindowPos().x + cardWidth * 0.5f,
            ImGui::GetCursorScreenPos().y + spinnerRadius
        };
        // ビルド状況の描画リスト
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        // 回転表示の点番号
        for (int index = 0;
            // 回転表示に並べる点の数
            index < spinnerDotCount;
            ++index)
        {
            // 回転点の配置角・rad
            const float angle =
                (static_cast<float>(index)
                    / static_cast<float>(spinnerDotCount))
                * DirectX::XM_2PI;
            // 先頭からの点数距離
            const int distanceFromHead =
                (spinnerPhase - index + spinnerDotCount)
                % spinnerDotCount;
            // 先頭からの距離による明度
            const float brightness =
                1.0f
                - static_cast<float>(distanceFromHead)
                    / static_cast<float>(spinnerDotCount);
            drawList->AddCircleFilled(
                ImVec2{
                    spinnerCenter.x
                        + std::cos(angle) * spinnerRadius,
                    spinnerCenter.y
                        + std::sin(angle) * spinnerRadius
                },
                2.5f + brightness * 1.5f,
                ImGui::GetColorU32(ImVec4{
                    0.25f,
                    0.62f,
                    1.0f,
                    0.2f + brightness * 0.8f
                }));
        }
        ImGui::Dummy(ImVec2{
            0.0f,
            spinnerRadius * 2.0f + 10.0f
        });

        centeredText(
            m_pendingScriptAttachments.empty()
                ? "完了後にGame Moduleを自動で再読み込みします。"
                : "完了後に自動で読み込み、GameObjectへアタッチします。");
        ImGui::Dummy(ImVec2{ 0.0f, 7.0f });

        // 操作禁止中の説明文
        const char* lockMessage =
            "処理が完了するまでエディターは操作できません。";
        // 操作禁止説明の横幅
        const float lockMessageWidth =
            ImGui::CalcTextSize(lockMessage).x;
        ImGui::SetCursorPosX(std::max(
            (cardWidth - lockMessageWidth) * 0.5f,
            0.0f));
        ImGui::TextDisabled("%s", lockMessage);

        // ビルド開始からの経過秒
        const double elapsedSeconds = std::max(
            animationTime - m_gameModuleBuildStartedAt,
            0.0);
        // 経過時間の表示文
        const std::string elapsedText =
            "経過時間: "
            + std::to_string(
                static_cast<int>(elapsedSeconds))
            + " 秒";
        // 経過時間の表示幅
        const float elapsedTextWidth =
            ImGui::CalcTextSize(elapsedText.c_str()).x;
        ImGui::SetCursorPosX(std::max(
            (cardWidth - elapsedTextWidth) * 0.5f,
            0.0f));
        ImGui::TextDisabled(
            "%s",
            elapsedText.c_str());

        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    // Camera描画先を先に更新し、有効な選択ビューと各プレビュー画像を描画します。
    void EditorLayer::RenderSceneViews()
    {
        // Scene Viewの背景色
        constexpr float sceneClearColor[]{ 0.055f, 0.070f, 0.095f, 1.0f };
        // Game Viewの背景色
        constexpr float gameClearColor[]{ 0.025f, 0.035f, 0.055f, 1.0f };

        // ビュー内で使うCamera画像を、ビュー本体より先に更新します。
        m_scene.RenderTargetTextures();

        if (m_activeViewport == ViewportMode::Scene
            && m_sceneRenderTarget.IsValid())
        {

            // Scene ViewのGPU計測区間
            GpuProfiler::SectionScope sceneViewSection{
                m_graphics.Gpu(),
                "Scene View"
            };
            m_graphics.SetUIViewportSize(
                m_sceneRenderTarget.Width(),
                m_sceneRenderTarget.Height());
            m_graphics.BeginOffscreenTarget(
                m_sceneRenderTarget,
                sceneClearColor);
            m_scene.RenderWithMatrices(
                SceneViewMatrix(),
                SceneProjectionMatrix(),
                false,
                m_colliderDebugVisible,
                &m_sceneRenderTarget);

            RunPostProcess(
                m_graphics,
                m_sceneRenderTarget,
                m_scene.PostProcessFrameData());
            // UIの色と輪郭を保つため、ポスト処理の後に描画します。
            m_scene.Render2D();

            // 編集補助表示のGPU計測区間
            GpuProfiler::SectionScope helperOverlaySection{
                m_graphics.Gpu(),
                "エディター補助表示"
            };
            m_graphics.BindOffscreenTarget(
                m_sceneRenderTarget);
            if (m_gridVisible)
            {
                if (m_scene2DMode)
                {
                    m_graphics.Debug().DrawGridXY(
                        m_gridSpacing,
                        m_gridExtent,
                        SceneViewMatrix(),
                        SceneProjectionMatrix());
                }
                else
                {
                    m_graphics.Debug().DrawGridXZ(
                        m_gridSpacing,
                        m_gridExtent,
                        SceneViewMatrix(),
                        SceneProjectionMatrix());
                }
            }
            if (m_cameraGizmosVisible)
            {
                DrawCameraGizmos();
            }
            if (m_lightGizmosVisible)
            {
                DrawLightGizmos();
            }

            DrawAnalysisSceneOverlay();
            // 選択枠はデバッグ線の表示設定に依存させません。
            DrawSelectionHighlight();
            // ポスト処理で描画先が入れ替わるため、最終画像を表示用へ公開します。
            m_graphics.PublishOffscreenTarget(
                m_sceneRenderTarget);
            helperOverlaySection.End();
            sceneViewSection.End();

            // 選択中のオブジェクト
            const auto* selected =
                m_scene.FindGameObject(m_selectedObjectId);
            // 選択中のカメラ
            const auto* selectedCamera = selected != nullptr
                ? selected->GetComponent<CameraComponent>()
                : nullptr;
            if (selectedCamera != nullptr
                && m_cameraPreviewRenderTarget.IsValid())
            {
                // カメラ画像のGPU計測区間
                GpuProfiler::SectionScope cameraPreviewSection{
                    m_graphics.Gpu(),
                    "カメラプレビュー"
                };
                m_graphics.SetUIViewportSize(
                    m_cameraPreviewRenderTarget.Width(),
                    m_cameraPreviewRenderTarget.Height());
                m_graphics.BeginOffscreenTarget(
                    m_cameraPreviewRenderTarget,
                    gameClearColor);
                m_scene.RenderWithMatrices(
                    selectedCamera->ViewMatrix(),
                    selectedCamera->ProjectionMatrix(
                        m_cameraPreviewRenderTarget.AspectRatio()),
                    false,
                    false,
                    &m_cameraPreviewRenderTarget);

                RunPostProcess(
                    m_graphics,
                    m_cameraPreviewRenderTarget,
                    m_scene.PostProcessFrameData());
                m_scene.Render2D();
                m_graphics.PublishOffscreenTarget(
                    m_cameraPreviewRenderTarget);
            }
        }
        else if (m_activeViewport == ViewportMode::Game
            && m_gameRenderTarget.IsValid())
        {
            // Game ViewのGPU計測区間
            GpuProfiler::SectionScope gameViewSection{
                m_graphics.Gpu(),
                "Game View"
            };
            m_graphics.SetUIViewportSize(
                m_gameRenderTarget.Width(),
                m_gameRenderTarget.Height());
            m_graphics.BeginOffscreenTarget(
                m_gameRenderTarget,
                gameClearColor);
            m_scene.RenderMainCamera(
                m_gameRenderTarget.AspectRatio(),
                false,
                &m_gameRenderTarget);

            RunPostProcess(
                m_graphics,
                m_gameRenderTarget,
                m_scene.PostProcessFrameData());
            m_scene.Render2D();

            // 遷移・読込表示の管理元
            const auto& scenes =
                m_scene.Scenes();
            if (m_playing)
            {
                // 読込表示はポスト処理後に重ね、遷移の覆いはScene側が描きます。
                m_graphics.DrawLoadingScreen(
                    scenes.TransitionFrame(),
                    scenes.LoadingScreen(),
                    m_gameRenderTarget.Width(),
                    m_gameRenderTarget.Height());
            }
            m_graphics.PublishOffscreenTarget(
                m_gameRenderTarget);
        }

        RenderMaterialPreview();
    }

    // 初回だけMaterial確認用の球体と太陽光を持つシーンを作成します。
    void EditorLayer::EnsureMaterialPreviewScene()
    {
        if (m_materialPreviewScene != nullptr)
        {
            return;
        }

        m_materialPreviewScene = std::make_unique<Scene>(m_graphics);
        m_materialPreviewScene->SetAmbientLightColor(
            { 0.62f, 0.68f, 0.80f });
        m_materialPreviewScene->SetAmbientLightIntensity(0.32f);

        // Material確認用の球体
        auto& sphere = m_materialPreviewScene->CreateGameObject(
            "Material Preview Sphere");
        m_materialPreviewRenderer =
            &sphere.AddComponent<MeshRendererComponent>(
                PrimitiveShape::Sphere);

        // Material確認用の太陽光
        auto& light = m_materialPreviewScene->CreateGameObject(
            "Material Preview Light");
        light.GetTransform().SetRotationVector(
            { -0.55f, -0.75f, 0.0f });
        light.AddComponent<DirectionalLightComponent>(
            DirectX::XMFLOAT3{ 1.0f, 0.94f, 0.84f },
            1.65f,
            false);
    }

    // 編集中のMaterialを回転する球体へ適用し、確認画像を描画します。
    void EditorLayer::RenderMaterialPreview()
    {
        if (!m_materialInspectorLoaded
            || !IsMaterialAsset(m_selectedAsset)
            || !m_materialPreviewRenderTarget.IsValid())
        {
            return;
        }
        EnsureMaterialPreviewScene();
        if (m_materialPreviewRenderer == nullptr)
        {
            return;
        }

        m_materialPreviewRenderer->SetMaterial(
            m_materialInspectorDraft);
        m_materialPreviewRenderer->GetTransform().SetRotationVector(
            {
                -0.08f,
                static_cast<float>(ImGui::GetTime()) * 0.18f,
                0.0f
            });

        // Material確認画像の背景色
        constexpr float clearColor[]{
            0.035f, 0.045f, 0.065f, 1.0f
        };
        // Material確認用の視点行列
        const auto view = DirectX::XMMatrixLookAtLH(
            DirectX::XMVectorSet(0.0f, 0.0f, 2.15f, 1.0f),
            DirectX::XMVectorZero(),
            DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
        // Material確認用の透視投影
        const auto projection = DirectX::XMMatrixPerspectiveFovLH(
            DirectX::XMConvertToRadians(34.0f),
            1.0f,
            0.05f,
            20.0f);

        m_graphics.SetUIViewportSize(
            m_materialPreviewRenderTarget.Width(),
            m_materialPreviewRenderTarget.Height());
        m_graphics.BeginOffscreenTarget(
            m_materialPreviewRenderTarget,
            clearColor);
        m_materialPreviewScene->RenderWithMatrices(
            view,
            projection,
            false,
            false,
            &m_materialPreviewRenderTarget);
        m_graphics.PublishOffscreenTarget(
            m_materialPreviewRenderTarget);
    }

    // 編集UIを描画し、Present前の撮影と遠隔操作の完了結果を出力します。
    void EditorLayer::Render()
    {
        ImGui::Render();

        // 編集UIのGPU計測区間
        GpuProfiler::SectionScope editorUiSection{
            m_graphics.Gpu(),
            "エディターUI"
        };
        m_editorGuiRenderer->RenderDrawData(ImGui::GetDrawData());
        editorUiSection.End();

        // UI描画後かつPresent前にバックバッファを撮影します。
        if (!m_screenshotRequest.imagePath.empty()
            && m_screenshotFrame
                >= m_screenshotRequest.captureFrame)
        {
            CaptureScreenshotAndQuit();
            // 終了要求後もフレームが回る可能性があるため、撮影要求を消費します。
            m_screenshotRequest.imagePath.clear();
        }


        if (!m_remotePendingShot.empty())
        {
            try
            {
                // 遠隔撮影画像の幅・pixel
                std::uint32_t width{};
                // 遠隔撮影画像の高さ・pixel
                std::uint32_t height{};
                // 遠隔撮影の画像データ
                const auto pixels =
                    m_graphics.CaptureBackBuffer(
                        width,
                        height);
                SavePng(
                    m_remotePendingShot,
                    width,
                    height,
                    pixels);
            }
            // 遠隔撮影の応答へ返す保存エラー
            catch (const std::exception& exception)
            {
                m_remoteReportError = exception.what();
            }
            WriteRemoteState();
            m_remotePendingShot.clear();
        }
        else if (m_remoteReportPending
            && m_remoteMacro.empty())
        {
            // 描画完了後に応答し、入力マクロは全手順が終わるまで応答を待ちます。
            WriteRemoteState();
        }
    }


    // 文字入力中または編集時のUI入力取得中に、ゲーム側のキー入力を遮断します。
    bool EditorLayer::WantsKeyboard() const noexcept
    {
        // UIの入力取得要求
        const auto& inputOutput = ImGui::GetIO();

        if (inputOutput.WantTextInput)
        {
            return true;
        }

        if (m_playing)
        {
            return false;
        }
        return inputOutput.WantCaptureKeyboard;
    }

    // ウィンドウの位置とスタイルを保存して全画面へ切り替え、次回は保存した表示へ戻します。
    void EditorLayer::ToggleFullscreen()
    {
        if (!m_fullscreen)
        {
            m_windowedPlacement.length =
                sizeof(WINDOWPLACEMENT);
            if (GetWindowPlacement(
                    m_window,
                    &m_windowedPlacement) == FALSE)
            {
                SetStatus(
                    "フルスクリーンへ切り替えられませんでした",
                    true);
                return;
            }

            // 切替先モニターの表示情報
            MONITORINFO monitorInfo{
                sizeof(MONITORINFO)
            };
            // 最寄りモニターのハンドル
            const HMONITOR monitor = MonitorFromWindow(
                m_window,
                MONITOR_DEFAULTTONEAREST);
            if (monitor == nullptr
                || GetMonitorInfoW(
                    monitor,
                    &monitorInfo) == FALSE)
            {
                SetStatus(
                    "表示先のモニターを取得できませんでした",
                    true);
                return;
            }

            m_windowedStyle = GetWindowLongPtrW(
                m_window,
                GWL_STYLE);
            m_windowedExtendedStyle = GetWindowLongPtrW(
                m_window,
                GWL_EXSTYLE);

            SetWindowLongPtrW(
                m_window,
                GWL_STYLE,
                m_windowedStyle
                    & ~static_cast<LONG_PTR>(
                        WS_OVERLAPPEDWINDOW));
            SetWindowLongPtrW(
                m_window,
                GWL_EXSTYLE,
                m_windowedExtendedStyle
                    & ~static_cast<LONG_PTR>(
                        WS_EX_WINDOWEDGE
                        | WS_EX_CLIENTEDGE));

            // 切替先モニター全体の矩形
            const RECT& monitorBounds =
                monitorInfo.rcMonitor;
            if (SetWindowPos(
                    m_window,
                    HWND_TOP,
                    monitorBounds.left,
                    monitorBounds.top,
                    monitorBounds.right
                        - monitorBounds.left,
                    monitorBounds.bottom
                        - monitorBounds.top,
                    SWP_FRAMECHANGED
                        | SWP_NOOWNERZORDER) == FALSE)
            {
                SetWindowLongPtrW(
                    m_window,
                    GWL_STYLE,
                    m_windowedStyle);
                SetWindowLongPtrW(
                    m_window,
                    GWL_EXSTYLE,
                    m_windowedExtendedStyle);
                SetStatus(
                    "フルスクリーンへ切り替えられませんでした",
                    true);
                return;
            }

            m_fullscreen = true;
            SetStatus("フルスクリーンに切り替えました");
            return;
        }

        SetWindowLongPtrW(
            m_window,
            GWL_STYLE,
            m_windowedStyle);
        SetWindowLongPtrW(
            m_window,
            GWL_EXSTYLE,
            m_windowedExtendedStyle);
        SetWindowPos(
            m_window,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_FRAMECHANGED
                | SWP_NOMOVE
                | SWP_NOSIZE
                | SWP_NOZORDER
                | SWP_NOOWNERZORDER);
        m_windowedPlacement.length =
            sizeof(WINDOWPLACEMENT);
        SetWindowPlacement(
            m_window,
            &m_windowedPlacement);

        m_fullscreen = false;
        SetStatus("ウィンドウ表示に戻しました");
    }

    // 2秒間隔で定義の内容変更を検出し、検証成功時に専用メニューとパネルを置き換えます。
    void EditorLayer::UpdateProjectMenus()
    {
        // メニュー定義の監視時刻・秒
        const double now = ImGui::GetTime();
        if (now - m_lastProjectMenuScanAt < 2.0)
        {
            return;
        }
        m_lastProjectMenuScanAt = now;

        // プロジェクトメニュー定義パス
        const auto manifestPath = ProjectSettingsPath().parent_path()
            / L"editor-menu.json";
        // 定義ファイルの確認エラー
        std::error_code existsError;
        // メニュー定義ファイルがある
        const bool exists = std::filesystem::is_regular_file(
            manifestPath,
            existsError);
        if (!exists)
        {
            if (m_projectMenuManifestSeen)
            {
                m_projectMenus.clear();
                m_projectPanels.clear();
                m_projectMenuManifestHash = 0;
                m_projectMenuManifestSeen = false;
                SetStatus("プロジェクト専用メニューを解除しました");
            }
            return;
        }

        // メニュー定義の入力ストリーム
        std::ifstream input(manifestPath, std::ios::binary);
        if (!input)
        {
            SetStatus(
                "プロジェクト専用メニューを読み込めません: "
                    + PathToUtf8(manifestPath),
                true);
            return;
        }
        // メニュー定義の本文
        const std::string source{
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{}
        };
        // 本文の変更検出ハッシュ
        const std::uint64_t sourceHash = HashProjectMenuManifest(source);
        if (m_projectMenuManifestSeen
            && sourceHash == m_projectMenuManifestHash)
        {
            return;
        }
        m_projectMenuManifestSeen = true;
        // 検証失敗でもハッシュを記憶し、同じ本文は再検証せずメニューを解除します。
        m_projectMenuManifestHash = sourceHash;

        try
        {
            // 検証するメニュー定義JSON
            const auto document = nlohmann::json::parse(source);
            if (!document.is_object()
                || document.value("format", std::string{})
                    != "LamaPonEditorMenu"
                || document.value("version", 0) != 1)
            {
                throw std::runtime_error(
                    "format=LamaPonEditorMenu / version=1 が必要です");
            }
            // 検証するメニュー項目一覧
            const auto& items = document.at("items");
            if (!items.is_array() || items.size() > 128u)
            {
                throw std::runtime_error(
                    "items は128件以内の配列にしてください");
            }

            // 検証済みのメニューツリー
            std::vector<ProjectMenuNode> menus;
            // 検証済みの専用パネル一覧
            std::vector<ProjectPanelDefinition> panels;
            // 専用メニューに使えない先頭名
            constexpr std::array<std::string_view, 8> reservedRoots{
                "ファイル", "編集", "シーン", "GameObject",
                "アセット", "ウィンドウ", "拡張機能", "ヘルプ"
            };
            // 検証するメニュー項目
            for (const auto& item : items)
            {
                if (!item.is_object())
                {
                    throw std::runtime_error(
                        "items の各要素はオブジェクトにしてください");
                }
                // 分割・検証したメニュー階層
                const auto path = SplitProjectMenuPath(
                    item.at("path").get<std::string>());
                if (std::ranges::find(reservedRoots, path.front())
                    != reservedRoots.end())
                {
                    throw std::runtime_error(
                        "組み込みメニュー名は先頭に使えません: "
                        + path.front());
                }

                // この項目の実行コマンド
                std::optional<ProjectMenuCommand> action;
                // この項目の専用パネル番号
                std::optional<std::size_t> panelIndex;
                if (item.contains("panel"))
                {
                    // 専用パネルの定義JSON
                    const auto& value = item.at("panel");
                    if (!value.is_object())
                    {
                        throw std::runtime_error("panel はオブジェクトにしてください");
                    }
                    // 検証する専用パネル定義
                    ProjectPanelDefinition panel;
                    // 専用パネルの種別
                    const auto type = value.value("type", std::string{});
                    if (type == "bgm-loop")
                    {
                        panel.kind = ProjectPanelKind::BgmLoop;
                    }
                    else if (type == "vehicle-parameters")
                    {
                        panel.kind = ProjectPanelKind::VehicleParameters;
                    }
                    else
                    {
                        throw std::runtime_error("未対応のpanel.typeです: " + type);
                    }
                    panel.title = value.value("title", path.back());
                    panel.dataPath = PathFromUtf8(
                        value.at("data").get<std::string>());
                    if (value.contains("saveCommand"))
                    {
                        panel.saveCommand.command = value.at("saveCommand")
                            .get<std::string>();
                        panel.saveCommand.arguments = value.value(
                            "saveArguments", std::vector<std::string>{});
                        panel.saveCommand.workingDirectory = PathFromUtf8(
                            value.value("workingDirectory", std::string{ "." }));
                    }
                    panelIndex = panels.size();
                    panels.push_back(std::move(panel));
                }
                else
                {
                    // 検証するツール実行定義
                    ProjectMenuCommand command;
                    command.command = item.at("command").get<std::string>();
                    if (command.command.empty())
                    {
                        throw std::runtime_error(
                            "command は空にできません: "
                            + item.at("path").get<std::string>());
                    }
                    if (item.contains("arguments"))
                    {
                        command.arguments = item.at("arguments")
                            .get<std::vector<std::string>>();
                    }
                    command.workingDirectory = PathFromUtf8(
                        item.value("workingDirectory", std::string{ "." }));
                    command.enabledWhilePlaying = item.value(
                        "enabledWhilePlaying", false);
                    action = std::move(command);
                }

                // 挿入先の同階層ノード一覧
                auto* siblings = &menus;
                // 追加した末尾メニューノード
                ProjectMenuNode* node = nullptr;
                // 追加するメニュー階層名
                for (const auto& segment : path)
                {
                    // 同名ノードの位置
                    auto existing = std::ranges::find(
                        *siblings,
                        segment,
                        &ProjectMenuNode::label);
                    if (existing == siblings->end())
                    {
                        siblings->push_back(ProjectMenuNode{ segment });
                        existing = std::prev(siblings->end());
                    }
                    node = &*existing;
                    siblings = &node->children;
                }
                if (node == nullptr || node->action.has_value()
                    || node->panelIndex.has_value())
                {
                    throw std::runtime_error(
                        "同じメニューパスが重複しています: "
                        + item.at("path").get<std::string>());
                }
                node->action = std::move(action);
                node->panelIndex = panelIndex;
            }
            m_projectMenus = std::move(menus);
            m_projectPanels = std::move(panels);
            SetStatus(
                "プロジェクト専用メニューを読み込みました（"
                + std::to_string(items.size())
                + "件）");
        }
        // 定義の検証・生成を中断した原因
        catch (const std::exception& exception)
        {
            m_projectMenus.clear();
            m_projectPanels.clear();
            SetStatus(
                std::string{ "editor-menu.json を読み込めません: " }
                    + exception.what(),
                true);
        }
    }

    // 定義の引数と作業先でツールを起動し、起動成否を通知します(command: 実行するツール定義)。
    void EditorLayer::LaunchProjectMenuCommand(
        const ProjectMenuCommand& command)
    {
        // プロジェクトのルートパス
        const auto projectRoot = ProjectSettingsPath()
            .parent_path()
            .parent_path();
        // 起動する実行ファイルのパス
        std::filesystem::path executable = PathFromUtf8(command.command);
        if (!executable.is_absolute()
            && (command.command.find('/') != std::string::npos
                || command.command.find('\\') != std::string::npos))
        {
            executable = projectRoot / executable;
        }

        // ツールを起動する作業パス
        std::filesystem::path workingDirectory = command.workingDirectory;
        if (workingDirectory.empty())
        {
            workingDirectory = projectRoot;
        }
        else if (!workingDirectory.is_absolute())
        {
            workingDirectory = projectRoot / workingDirectory;
        }
        workingDirectory = workingDirectory.lexically_normal();

        // 引用処理した引数文字列
        std::wstring parameters;
        // 引用する起動引数
        for (const auto& argument : command.arguments)
        {
            if (!parameters.empty())
            {
                parameters.push_back(L' ');
            }
            parameters += QuoteWindowsArgument(Utf8ToWide(argument));
        }
        // ShellExecuteの起動結果
        const HINSTANCE result = ShellExecuteW(
            m_window,
            L"open",
            executable.c_str(),
            parameters.empty() ? nullptr : parameters.c_str(),
            workingDirectory.c_str(),
            SW_SHOWNORMAL);
        if (reinterpret_cast<std::intptr_t>(result) <= 32)
        {
            SetStatus(
                "プロジェクトツールを起動できません: "
                    + command.command,
                true);
            return;
        }
        SetStatus(
            "プロジェクトツールを起動しました: "
                + command.command);
    }

    // ノードの再生可否とパネル・コマンド操作を描画し、子へ再帰します(node: 描画するノード, idPath: 親階層の識別パス)。
    void EditorLayer::DrawProjectMenuNode(
        ProjectMenuNode& node,
        const std::string_view idPath)
    {
        // 階層を含むメニューID
        const std::string id = std::string{ idPath }
            + "/" + node.label;
        // 表示名と重複回避用ID
        const std::string itemLabel = node.label
            + "##ProjectMenu/" + id;
        // 再生状態による実行可否
        const bool enabled = !m_playing
            || (node.action.has_value()
                && node.action->enabledWhilePlaying);
        if (node.children.empty())
        {
            if ((node.action.has_value() || node.panelIndex.has_value())
                && ImGui::MenuItem(
                    itemLabel.c_str(),
                    nullptr,
                    false,
                    enabled))
            {
                if (node.panelIndex.has_value())
                {
                    m_projectPanels.at(*node.panelIndex).open = true;
                }
                else
                {
                    LaunchProjectMenuCommand(*node.action);
                }
            }
            return;
        }

        if (ImGui::BeginMenu(itemLabel.c_str()))
        {
            if (node.action.has_value() || node.panelIndex.has_value())
            {
                // 親項目を開くための表示名
                const std::string openLabel = "開く##ProjectMenuOpen/" + id;
                if (ImGui::MenuItem(
                    openLabel.c_str(),
                    nullptr,
                    false,
                    enabled))
                {
                    if (node.panelIndex.has_value())
                    {
                        m_projectPanels.at(*node.panelIndex).open = true;
                    }
                    else
                    {
                        LaunchProjectMenuCommand(*node.action);
                    }
                }
                ImGui::Separator();
            }
            // 描画する子メニューノード
            for (auto& child : node.children)
            {
                DrawProjectMenuNode(child, id);
            }
            ImGui::EndMenu();
        }
    }

    // 登録されたプロジェクト専用メニューを描画します。
    void EditorLayer::DrawProjectMenus()
    {
        // 描画する先頭メニューノード
        for (auto& menu : m_projectMenus)
        {
            DrawProjectMenuNode(menu, "root");
        }
    }

    // 編集中の専用パネルを描画し、閉じたBGMパネルと再生中の試聴を止めます。
    void EditorLayer::DrawProjectPanels()
    {
        // 再生中はパネルを描かないため、パネル側へ任せずここで試聴を止めます。
        if (m_playing && m_bgmPanel)
        {
            m_bgmPanel->StopPreview();
        }
        // 描画する専用パネル番号
        for (std::size_t index = 0; index < m_projectPanels.size(); ++index)
        {
            // 描画する専用パネル定義
            auto& panel = m_projectPanels[index];
            if (!panel.open)
            {

                if (m_bgmPanel
                    && m_bgmPanel->Matches(
                        ProjectSettingsPath().parent_path().parent_path()
                            / panel.dataPath))
                {
                    m_bgmPanel->StopPreview();
                }
                continue;
            }
            if (m_playing)
            {
                continue;
            }
            if (panel.kind == ProjectPanelKind::BgmLoop)
            {
                DrawProjectBgmPanel(index, panel);
            }
            else
            {
                DrawProjectVehiclePanel(index, panel);
            }
        }
    }

    // BGMカタログに対応する編集パネルを描画します(panel: 描画するパネル定義)。
    void EditorLayer::DrawProjectBgmPanel(
        const std::size_t, ProjectPanelDefinition& panel)
    {
        // プロジェクトのルートパス
        const auto root = ProjectSettingsPath().parent_path().parent_path();
        // BGMカタログの絶対パス
        const auto catalog = root / panel.dataPath;
        if (!m_bgmPanel || !m_bgmPanel->Matches(catalog))
        {
            // BGM編集の通知を転送します(message: 表示文, error: エラー表示か)。
            m_bgmPanel = std::make_unique<BgmLoopPanel>(
                m_graphics.Audio(), m_graphics.Assets(), root, catalog,
                [this](std::string message, const bool error)
                { SetStatus(std::move(message), error); });
        }
        // 保存後に定義されたツールを起動します。
        m_bgmPanel->Draw(panel.title, panel.open, [&]
        {
            if (!panel.saveCommand.command.empty())
            {
                LaunchProjectMenuCommand(panel.saveCommand);
            }
        });
    }

    // 車両データに対応する編集パネルを描画します(panel: 描画するパネル定義)。
    void EditorLayer::DrawProjectVehiclePanel(
        const std::size_t,
        ProjectPanelDefinition& panel)
    {
        // プロジェクトのルートパス
        const auto root = ProjectSettingsPath().parent_path().parent_path();
        // 車両設定データの絶対パス
        const auto dataPath = root / panel.dataPath;
        if (!m_vehicleParametersPanel
            || !m_vehicleParametersPanel->Matches(dataPath))
        {
            // 車両編集の通知を転送します(message: 表示文, error: エラー表示か)。
            m_vehicleParametersPanel =
                std::make_unique<VehicleParametersPanel>(
                    m_graphics,
                    m_graphics.Assets(),
                    dataPath,
                    [this](std::string message, const bool error)
                    { SetStatus(std::move(message), error); });
        }
        // 保存後に定義されたツールを起動します。
        m_vehicleParametersPanel->Draw(
            *m_editorGuiRenderer,
            *m_editorModelPreviewRenderer,
            panel.title,
            panel.open,
            [&]
            {
                if (!panel.saveCommand.command.empty())
                {
                    LaunchProjectMenuCommand(panel.saveCommand);
                }
            });
    }

    // メニュー・再生操作・統計を描画し、編集用ショートカットを処理します。
    void EditorLayer::DrawToolbar()
    {
        // ツールバー操作の入力状態
        const auto& inputOutput = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2{ 0.0f, 0.0f });
        ImGui::SetNextWindowSize(ImVec2{ inputOutput.DisplaySize.x, ToolbarHeight });

        // ツールバーの表示フラグ
        constexpr ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration
            | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoDocking
            | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoBringToFrontOnFocus
            | ImGuiWindowFlags_MenuBar;

        ImGui::Begin("##LamaPonToolbar", nullptr, flags);

        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("ファイル"))
            {
                if (ImGui::MenuItem(
                    "新規シーン",
                    "Ctrl+N",
                    false,
                    !m_playing))
                {
                    NewScene();
                }
                if (ImGui::MenuItem(
                    "シーンを開く...",
                    "Ctrl+O",
                    false,
                    !m_playing))
                {
                    OpenScene();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(
                    "シーンを保存",
                    "Ctrl+S",
                    false,
                    !m_playing))
                {
                    SaveScene();
                }
                if (ImGui::MenuItem(
                    "名前を付けて保存...",
                    "Ctrl+Shift+S",
                    false,
                    !m_playing))
                {
                    SaveSceneAs();
                }
                if (ImGui::MenuItem(
                    "シーンを再読み込み",
                    "Ctrl+R",
                    false,
                    !m_playing
                        && !m_scenePath.empty()))
                {
                    ReloadScene();
                }
                if (ImGui::MenuItem(
                    "選択をPrefabとして保存...",
                    nullptr,
                    false,
                    !m_playing
                        && m_scene.FindGameObject(
                            m_selectedObjectId) != nullptr))
                {
                    SaveSelectedAsPrefab();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(
                    "プロジェクト設定とビルド...",
                    nullptr,
                    false,
                    !m_playing))
                {
                    OpenProjectSettingsDialog();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("終了", "Alt+F4"))
                {
                    PostMessageW(
                        m_window,
                        WM_CLOSE,
                        0,
                        0);
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("編集"))
            {
                if (ImGui::MenuItem(
                    "元に戻す",
                    "Ctrl+Z",
                    false,
                    !m_playing && CanUndo()))
                {
                    Undo();
                }
                if (ImGui::MenuItem(
                    "やり直す",
                    "Ctrl+Y",
                    false,
                    !m_playing && CanRedo()))
                {
                    Redo();
                }
                ImGui::Separator();
                // 選択オブジェクトが存在する
                const bool hasSelection =
                    m_scene.FindGameObject(
                        m_selectedObjectId) != nullptr;
                if (ImGui::MenuItem(
                    "切り取り",
                    "Ctrl+X",
                    false,
                    !m_playing && hasSelection))
                {
                    CutSelectedGameObject();
                }
                if (ImGui::MenuItem(
                    "コピー",
                    "Ctrl+C",
                    false,
                    !m_playing && hasSelection))
                {
                    CopySelectedGameObject();
                }
                if (ImGui::MenuItem(
                    "貼り付け",
                    "Ctrl+V",
                    false,
                    !m_playing
                        && !m_clipboardSceneJson.empty()))
                {
                    PasteGameObject();
                }
                if (ImGui::MenuItem(
                    "複製",
                    "Ctrl+D",
                    false,
                    !m_playing && hasSelection))
                {
                    DuplicateSelectedGameObject();
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("シーン"))
            {
                ImGui::MenuItem(
                    "環境設定...",
                    nullptr,
                    &m_sceneEnvironmentOpen);
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("GameObject"))
            {
                // 選択オブジェクトが存在する
                const bool hasSelection =
                    m_scene.FindGameObject(
                        m_selectedObjectId) != nullptr;
                if (ImGui::MenuItem(
                    "空のGameObjectを作成",
                    "Ctrl+Shift+N",
                    false,
                    !m_playing))
                {
                    CreateRootGameObject();
                }
                if (ImGui::MenuItem(
                    "空の子を作成",
                    "Alt+Shift+N",
                    false,
                    !m_playing && hasSelection))
                {
                    CreateChildGameObject();
                }
                ImGui::Separator();
                if (ImGui::BeginMenu("2Dオブジェクト", !m_playing))
                {
                    if (ImGui::MenuItem("スプライト"))
                    {
                        CreateBuiltInGameObject(
                            BuiltInGameObjectKind::Sprite);
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("3Dオブジェクト", !m_playing))
                {
                    if (ImGui::MenuItem("立方体"))
                    {
                        CreateBuiltInGameObject(
                            BuiltInGameObjectKind::Cube);
                    }
                    if (ImGui::MenuItem("球"))
                    {
                        CreateBuiltInGameObject(
                            BuiltInGameObjectKind::Sphere);
                    }
                    if (ImGui::MenuItem("円柱"))
                    {
                        CreateBuiltInGameObject(
                            BuiltInGameObjectKind::Cylinder);
                    }
                    if (ImGui::MenuItem("平面"))
                    {
                        CreateBuiltInGameObject(
                            BuiltInGameObjectKind::Plane);
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("ライト", !m_playing))
                {
                    if (ImGui::MenuItem("平行光源"))
                    {
                        CreateBuiltInGameObject(
                            BuiltInGameObjectKind::DirectionalLight);
                    }
                    if (ImGui::MenuItem("ポイントライト"))
                    {
                        CreateBuiltInGameObject(
                            BuiltInGameObjectKind::PointLight);
                    }
                    if (ImGui::MenuItem("スポットライト"))
                    {
                        CreateBuiltInGameObject(
                            BuiltInGameObjectKind::SpotLight);
                    }
                    if (ImGui::MenuItem("2Dライト"))
                    {
                        CreateBuiltInGameObject(
                            BuiltInGameObjectKind::Light2D);
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("オーディオ", !m_playing))
                {
                    if (ImGui::MenuItem("オーディオソース"))
                    {
                        CreateBuiltInGameObject(
                            BuiltInGameObjectKind::AudioSource);
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem(
                    "カメラ",
                    nullptr,
                    false,
                    !m_playing))
                {
                    CreateBuiltInGameObject(
                        BuiltInGameObjectKind::Camera);
                }
                if (ImGui::BeginMenu("UI", !m_playing))
                {
                    if (ImGui::MenuItem("Canvas"))
                    {
                        CreateUICanvasGameObject();
                    }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(
                    "選択オブジェクトを削除",
                    "Delete",
                    false,
                    !m_playing && hasSelection))
                {
                    DeleteSelectedGameObject();
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("コンポーネント"))
            {
                // 選択オブジェクトが存在する
                const bool hasSelection =
                    m_scene.FindGameObject(
                        m_selectedObjectId) != nullptr;
                if (ImGui::MenuItem(
                    "選択中のGameObjectに追加...",
                    nullptr,
                    false,
                    !m_playing && hasSelection))
                {
                    // アセット選択を解除して、GameObjectのInspectorへ追加ピッカーを予約します。
                    m_selectedAsset.clear();
                    m_addComponentPickerRequested = true;
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("アセット"))
            {
                if (ImGui::BeginMenu(
                    "作成",
                    !m_playing
                        && m_gameModuleBuildProcess == nullptr))
                {
                    DrawCreateAssetMenuContents(
                        m_assetDirectory);
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem(
                    "新しいアセットをインポート...",
                    nullptr,
                    false,
                    !m_playing
                        && m_gameModuleBuildProcess == nullptr))
                {
                    OpenImportAssetsDialog();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(
                    "選択項目を再インポート",
                    nullptr,
                    false,
                    !m_playing
                        && m_gameModuleBuildProcess == nullptr
                        && !m_selectedAsset.empty()))
                {
                    ReimportSelectedAsset();
                }
                if (ImGui::MenuItem(
                    "すべてのアセットを再インポート",
                    nullptr,
                    false,
                    !m_playing
                        && m_gameModuleBuildProcess == nullptr))
                {
                    ReimportAllAssets();
                }
                if (ImGui::MenuItem(
                    "アセット一覧を更新",
                    "F5"))
                {
                    RefreshAssets();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(
                    "選択項目をエクスプローラーで表示",
                    nullptr,
                    false,
                    !m_selectedAsset.empty()))
                {
                    OpenAssetInExplorer(
                        m_selectedAsset,
                        true);
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("ウィンドウ"))
            {
                if (ImGui::MenuItem(
                        "フルスクリーン",
                        "F11",
                        m_fullscreen))
                {
                    ToggleFullscreen();
                }
                ImGui::Separator();
                DrawRegisteredPanelMenuItems();
                ImGui::Separator();
                if (ImGui::MenuItem(
                    "レイアウトを保存"))
                {
                    ImGui::SaveIniSettingsToDisk(
                        m_imguiIniPath.c_str());
                    SaveEditorSettings();
                    SetStatus(
                        "ウィンドウレイアウトを保存しました");
                }
                if (ImGui::MenuItem(
                    "標準レイアウトに戻す"))
                {
                    m_editorExtensions.ResetPanelVisibility();
                    m_resetDockLayout = true;
                    SetStatus(
                        "標準レイアウトへ戻しました");
                }
                ImGui::EndMenu();
            }

            DrawProjectMenus();


            if (ImGui::BeginMenu("拡張機能"))
            {
                DrawRegisteredExtensionMenuItems();
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("ヘルプ"))
            {
                if (ImGui::MenuItem("ヘルプとサポート..."))
                {
                    OpenHelpCenter();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("オンラインマニュアル"))
                {
                    OpenOnlineManual();
                }
                if (ImGui::MenuItem("ローカルドキュメント"))
                {
                    OpenLocalDocumentation();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("コンソールを表示"))
                {
                    static_cast<void>(
                        m_editorExtensions.SetPanelOpen(
                            ConsolePanelId,
                            true));
                    m_focusConsoleRequested = true;
                }
                if (ImGui::MenuItem("エディターログを開く"))
                {
                    OpenEditorLog(false);
                }
                if (ImGui::MenuItem("ログフォルダーを開く"))
                {
                    OpenEditorLog(true);
                }
                ImGui::EndMenu();
            }

            ImGui::EndMenuBar();
        }

        // 操作ボタン行のY座標
        const float rowY = ImGui::GetCursorPosY();
        // ツールバーの横幅
        const float windowWidth =
            ImGui::GetWindowWidth();

        ImGui::SetCursorPos(
            ImVec2{ 8.0f, rowY + 3.0f });
        // 操作結果の表示色
        const ImVec4 statusColor = m_statusIsError
            ? ImVec4{ 1.0f, 0.35f, 0.30f, 1.0f }
            : ImVec4{ 0.35f, 0.85f, 0.55f, 1.0f };
        // 開いているシーンの表示名
        const std::string sceneLabel =
            m_scenePath.empty()
                ? "無題のシーン"
                : PathToUtf8(m_scenePath.filename());

        // Script無効状態の表示文
        const std::string safeModeLabel =
            "セーフモード：C++スクリプトは読み込まれていません";
        // 左端に表示する状態文
        const std::string& leftText = m_safeMode
            ? safeModeLabel
            : (m_statusMessage.empty()
                ? sceneLabel
                : m_statusMessage);
        // ツールバー左上の座標
        const auto windowPosition =
            ImGui::GetWindowPos();
        ImGui::PushClipRect(
            ImVec2{
                windowPosition.x + 8.0f,
                windowPosition.y + rowY
            },
            ImVec2{
                windowPosition.x
                    + windowWidth * 0.5f - 52.0f,
                windowPosition.y
                    + ToolbarHeight
            },
            true);
        if (m_safeMode)
        {
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.75f, 0.30f, 1.0f },
                "%s",
                leftText.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(
                "通常モードで開き直す"))
            {
                // 保存確認を二重にしないようウィンドウを直接破棄し、再起動はプロジェクトのロック解放後に行います。
                if (ConfirmClose())
                {
                    s_normalModeRestartRequested = true;
                    DestroyWindow(m_window);
                }
            }
        }
        else if (m_statusMessage.empty())
        {
            ImGui::TextDisabled(
                "%s",
                leftText.c_str());
        }
        else
        {
            ImGui::TextColored(
                statusColor,
                "%s",
                leftText.c_str());
        }
        ImGui::PopClipRect();


        // 再生・停止ボタンの幅
        constexpr float playButtonWidth = 72.0f;
        // 一時停止・再開ボタンの幅
        constexpr float pauseButtonWidth = 96.0f;
        // 1フレーム進行ボタンの幅
        constexpr float stepButtonWidth = 112.0f;
        // 再生操作ボタン間の余白
        const float spacing =
            ImGui::GetStyle().ItemSpacing.x;
        // 中央へ置く操作ボタン全幅
        const float groupWidth = m_playing
            ? playButtonWidth
                + pauseButtonWidth
                + stepButtonWidth
                + spacing * 2.0f
            : playButtonWidth;
        ImGui::SetCursorPos(
            ImVec2{
                (windowWidth - groupWidth) * 0.5f,
                rowY
            });
        if (!m_playing)
        {
            ImGui::PushStyleColor(
                ImGuiCol_Button,
                ImVec4{ 0.12f, 0.35f, 0.62f, 1.0f });
            if (ImGui::Button(
                "再生",
                ImVec2{ playButtonWidth, 0.0f }))
            {
                StartPlaying();
            }
            ImGui::PopStyleColor();
        }
        else
        {
            ImGui::PushStyleColor(
                ImGuiCol_Button,
                ImVec4{ 0.65f, 0.18f, 0.16f, 1.0f });
            if (ImGui::Button(
                "停止",
                ImVec2{ playButtonWidth, 0.0f }))
            {
                StopPlaying();
            }
            ImGui::PopStyleColor();

            ImGui::SameLine();
            ImGui::PushStyleColor(
                ImGuiCol_Button,
                m_paused
                    ? ImVec4{ 0.20f, 0.48f, 0.26f, 1.0f }
                    : ImVec4{ 0.48f, 0.40f, 0.12f, 1.0f });
            if (ImGui::Button(
                m_paused ? "再開" : "一時停止",
                ImVec2{ pauseButtonWidth, 0.0f }))
            {
                SetPaused(!m_paused);
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "ゲームの更新だけを止めます。描画は続くので"
                    "Scene Viewで自由に見回せます。");
            }

            ImGui::SameLine();

            ImGui::BeginDisabled(!m_paused);
            if (ImGui::Button(
                "次のフレーム",
                ImVec2{ stepButtonWidth, 0.0f }))
            {
                RequestSimulationStep();
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    m_paused
                        ? "1フレームだけ進めます。"
                        : "一時停止中に使えます。");
            }
        }

        // 右端に表示する統計文
        const std::string statistics =
            "FPS: "
            + std::to_string(
                static_cast<int>(
                    std::lround(
                        m_graphics.FrameStats()
                            .framesPerSecond)))
            + "  オブジェクト: "
            + std::to_string(
                m_scene.GameObjects().size())
            + "  画像: "
            + std::to_string(
                m_graphics.Assets().CachedTextureCount())
            + "  モデル: "
            + std::to_string(
                m_graphics.Assets().CachedModelCount())
            + "  文字: "
            + std::to_string(
                m_graphics.Assets().CachedTextCount());
        // 統計文の表示幅
        const float statisticsWidth =
            ImGui::CalcTextSize(
                statistics.c_str()).x;
        ImGui::SetCursorPos(
            ImVec2{
                std::max(
                    windowWidth - statisticsWidth - 12.0f,
                    windowWidth * 0.5f + 52.0f),
                rowY + 3.0f
            });
        ImGui::TextDisabled(
            "%s",
            statistics.c_str());

        if (!inputOutput.WantTextInput
            && !m_playing
            && inputOutput.KeyShift
            && inputOutput.KeyCtrl
            && !inputOutput.KeyAlt
            && ImGui::IsKeyPressed(
                ImGuiKey_N,
                false))
        {
            CreateRootGameObject();
        }
        else if (!inputOutput.WantTextInput
            && !m_playing
            && inputOutput.KeyShift
            && inputOutput.KeyAlt
            && !inputOutput.KeyCtrl
            && ImGui::IsKeyPressed(
                ImGuiKey_N,
                false))
        {
            CreateChildGameObject();
        }

        if (!inputOutput.WantTextInput
            && !m_playing
            && inputOutput.KeyCtrl
            && !inputOutput.KeyShift
            && ImGui::IsKeyPressed(
                ImGuiKey_N,
                false))
        {
            NewScene();
        }
        if (!inputOutput.WantTextInput
            && !m_playing
            && inputOutput.KeyCtrl
            && !inputOutput.KeyShift
            && ImGui::IsKeyPressed(
                ImGuiKey_O,
                false))
        {
            OpenScene();
        }
        if (!inputOutput.WantTextInput
            && !m_playing
            && inputOutput.KeyCtrl
            && !inputOutput.KeyShift
            && ImGui::IsKeyPressed(
                ImGuiKey_R,
                false))
        {
            ReloadScene();
        }
        if (!inputOutput.WantTextInput
            && ImGui::IsKeyPressed(
                ImGuiKey_F5,
                false))
        {
            RefreshAssets();
        }
        if (!inputOutput.WantTextInput
            && ImGui::IsKeyPressed(
                ImGuiKey_F11,
                false))
        {
            ToggleFullscreen();
        }

        if (inputOutput.KeyCtrl
            && inputOutput.KeyShift
            && ImGui::IsKeyPressed(ImGuiKey_S, false))
        {
            SaveSceneAs();
        }
        else if (inputOutput.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
        {
            SaveScene();
        }

        if (!m_playing && inputOutput.KeyCtrl && !inputOutput.WantTextInput)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Z, false))
            {
                if (inputOutput.KeyShift)
                {
                    Redo();
                }
                else
                {
                    Undo();
                }
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_Y, false))
            {
                Redo();
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_C, false))
            {
                CopySelectedGameObject();
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_X, false))
            {
                CutSelectedGameObject();
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_V, false))
            {
                PasteGameObject();
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_D, false))
            {
                DuplicateSelectedGameObject();
            }
        }
        if (!m_playing
            && !inputOutput.WantTextInput
            && ImGui::IsKeyPressed(
                ImGuiKey_Delete,
                false))
        {
            DeleteSelectedGameObject();
        }

        ImGui::End();
    }

    // ツールバー下にドックスペースを作り、要求時と初回に既定のパネル配置を組み立てます。
    void EditorLayer::DrawDockSpace()
    {
        // 配置元の主ビューポート
        const ImGuiViewport* viewport =
            ImGui::GetMainViewport();
        // ツールバー下の配置原点
        const ImVec2 dockPosition{
            viewport->Pos.x,
            viewport->Pos.y + ToolbarHeight
        };
        // ツールバーを除く表示サイズ
        const ImVec2 dockSize{
            viewport->Size.x,
            std::max(
                viewport->Size.y - ToolbarHeight,
                1.0f)
        };

        ImGui::SetNextWindowPos(dockPosition);
        ImGui::SetNextWindowSize(dockSize);
        ImGui::SetNextWindowViewport(viewport->ID);

        // ドック配置元の表示フラグ
        constexpr ImGuiWindowFlags hostFlags =
            ImGuiWindowFlags_NoDocking
            | ImGuiWindowFlags_NoTitleBar
            | ImGuiWindowFlags_NoCollapse
            | ImGuiWindowFlags_NoResize
            | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoBringToFrontOnFocus
            | ImGuiWindowFlags_NoNavFocus
            | ImGuiWindowFlags_NoSavedSettings;

        ImGui::PushStyleVar(
            ImGuiStyleVar_WindowRounding,
            0.0f);
        ImGui::PushStyleVar(
            ImGuiStyleVar_WindowBorderSize,
            0.0f);
        ImGui::PushStyleVar(
            ImGuiStyleVar_WindowPadding,
            ImVec2{ 0.0f, 0.0f });
        ImGui::Begin(
            "##LamaPonDockSpaceHost",
            nullptr,
            hostFlags);
        ImGui::PopStyleVar(3);

        // 主ドックスペースのID
        const ImGuiID dockspaceId =
            ImGui::GetID("LamaPonDockSpace");
        if (m_resetDockLayout
            || ImGui::DockBuilderGetNode(
                dockspaceId) == nullptr)
        {
            ImGui::DockBuilderRemoveNode(
                dockspaceId);
            ImGui::DockBuilderAddNode(
                dockspaceId,
                ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodePos(
                dockspaceId,
                dockPosition);
            ImGui::DockBuilderSetNodeSize(
                dockspaceId,
                dockSize);

            // 中央ビューポートのノードID
            ImGuiID centerId = dockspaceId;
            // ヒエラルキーのノードID
            ImGuiID leftId{};
            // インスペクターのノードID
            ImGuiID rightId{};
            ImGui::DockBuilderSplitNode(
                centerId,
                ImGuiDir_Left,
                0.23f,
                &leftId,
                &centerId);
            ImGui::DockBuilderSplitNode(
                centerId,
                ImGuiDir_Right,
                0.25f,
                &rightId,
                &centerId);

            // 下部共有パネルのノードID
            ImGuiID consoleId{};
            ImGui::DockBuilderSplitNode(
                centerId,
                ImGuiDir_Down,
                0.28f,
                &consoleId,
                &centerId);

            ImGui::DockBuilderDockWindow(
                "ヒエラルキー",
                leftId);
            ImGui::DockBuilderDockWindow(
                "ビューポート",
                centerId);
            // アセット・コンソールと補助パネルは下部ノードを共有します。
            ImGui::DockBuilderDockWindow(
                "アセット",
                consoleId);
            ImGui::DockBuilderDockWindow(
                "コンソール",
                consoleId);
            ImGui::DockBuilderDockWindow(
                "セーブデータ",
                consoleId);
            ImGui::DockBuilderDockWindow(
                "パフォーマンス",
                consoleId);
            ImGui::DockBuilderDockWindow(
                "タイルパレット",
                consoleId);
            ImGui::DockBuilderDockWindow(
                "インスペクター",
                rightId);
            ImGui::DockBuilderFinish(
                dockspaceId);
            m_resetDockLayout = false;
            m_selectAssetTabAfterLayoutReset = true;
        }

        ImGui::DockSpace(
            dockspaceId,
            ImVec2{ 0.0f, 0.0f });
        ImGui::End();
    }

    // ログの一時停止・検索・レベル別表示・コピーと関連対象の選択を描画します(open: ウィンドウの開閉状態)。
    void EditorLayer::DrawConsole(bool& open)
    {
        if (!open)
        {
            return;
        }

        if (m_focusConsoleRequested)
        {
            ImGui::SetNextWindowFocus();
            m_focusConsoleRequested = false;
        }
        ImGui::SetNextWindowSize(
            ImVec2{ 720.0f, 230.0f },
            ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(
            "コンソール",
                &open,
                ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            return;
        }

        // ログの取得・消去元
        auto& logger = Logger::Instance();
        if (!m_consolePaused)
        {
            m_consoleEntries =
                logger.Snapshot();
        }

        if (ImGui::Button("クリア"))
        {
            logger.Clear();
            m_consoleEntries.clear();
            m_consoleLastSequence = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button(
                m_consolePaused
                    ? "再開"
                    : "一時停止"))
        {
            m_consolePaused =
                !m_consolePaused;
            if (!m_consolePaused)
            {
                m_consoleEntries =
                    logger.Snapshot();
            }
        }
        ImGui::SameLine();
        ImGui::Checkbox(
            "自動スクロール",
            &m_consoleAutoScroll);

        // 保存済みInfoログの件数
        std::size_t infoCount{};
        // 保存済み警告ログの件数
        std::size_t warningCount{};
        // 保存済みエラーログの件数
        std::size_t errorCount{};
        // 集計・表示するログ項目
        for (const auto& entry :
            m_consoleEntries)
        {
            switch (entry.level)
            {
            case LogLevel::Warning:
                ++warningCount;
                break;
            case LogLevel::Error:
                ++errorCount;
                break;
            default:
                ++infoCount;
                break;
            }
        }

        ImGui::SameLine();
        ImGui::Checkbox(
            ("Info "
                + std::to_string(infoCount)
                + "##ConsoleInfo").c_str(),
            &m_consoleShowInfo);
        ImGui::SameLine();
        ImGui::Checkbox(
            ("Warning "
                + std::to_string(warningCount)
                + "##ConsoleWarning").c_str(),
            &m_consoleShowWarning);
        ImGui::SameLine();
        ImGui::Checkbox(
            ("Error "
                + std::to_string(errorCount)
                + "##ConsoleError").c_str(),
            &m_consoleShowError);

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint(
            "##ConsoleFilter",
            "メッセージまたはソースを検索",
            m_consoleFilter.data(),
            m_consoleFilter.size());

        // 小文字化したログ検索文字列
        const std::string filter =
            Lowercase(
                std::string{
                    m_consoleFilter.data() });
        // 保存済みログの最新番号
        const std::uint64_t newestSequence =
            m_consoleEntries.empty()
                ? 0
                : m_consoleEntries.back().
                    sequence;
        // 前回と最新ログ番号が異なる
        const bool hasNewEntries =
            newestSequence
                != m_consoleLastSequence;

        ImGui::BeginChild(
            "ConsoleEntries",
            ImVec2{ 0.0f, 0.0f },
            ImGuiChildFlags_Borders,
            ImGuiWindowFlags_HorizontalScrollbar);
        // 集計・表示するログ項目
        for (const auto& entry :
            m_consoleEntries)
        {
            if ((entry.level
                        == LogLevel::Info
                    && !m_consoleShowInfo)
                || (entry.level
                        == LogLevel::Warning
                    && !m_consoleShowWarning)
                || (entry.level
                        == LogLevel::Error
                    && !m_consoleShowError))
            {
                continue;
            }

            // ログ発生元のファイル名
            const std::string sourceName =
                entry.sourceFile.empty()
                    ? std::string{}
                    : PathToUtf8(
                        std::filesystem::path(
                            entry.sourceFile)
                            .filename());
            if (!filter.empty())
            {
                // 検索する本文と発生元名
                const std::string searchable =
                    Lowercase(
                        entry.message
                        + " "
                        + sourceName);
                if (searchable.find(filter)
                    == std::string::npos)
                {
                    continue;
                }
            }

            // 発生時刻のミリ秒部分
            const auto milliseconds =
                std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                        entry.timestamp.
                            time_since_epoch())
                    % 1000;
            // 発生時刻の暦変換用値
            const std::time_t rawTime =
                std::chrono::system_clock::
                    to_time_t(
                        entry.timestamp);
            // 発生時刻のローカル暦時刻
            std::tm localTime{};
            localtime_s(
                &localTime,
                &rawTime);
            // 発生時刻の表示バッファ
            std::array<char, 32>
                timeBuffer{};
            std::snprintf(
                timeBuffer.data(),
                timeBuffer.size(),
                "%02d:%02d:%02d.%03lld",
                localTime.tm_hour,
                localTime.tm_min,
                localTime.tm_sec,
                static_cast<long long>(
                    milliseconds.count()));

            // ログレベルの短縮表示
            const char* levelText =
                entry.level
                    == LogLevel::Warning
                    ? "WARN"
                    : entry.level
                        == LogLevel::Error
                        ? "ERROR"
                        : "INFO";
            // ログレベルの表示色
            const ImVec4 color =
                entry.level
                    == LogLevel::Warning
                    ? ImVec4{
                        1.0f, 0.78f,
                        0.24f, 1.0f }
                    : entry.level
                        == LogLevel::Error
                        ? ImVec4{
                            1.0f, 0.34f,
                            0.32f, 1.0f }
                        : ImVec4{
                            0.76f, 0.84f,
                            0.92f, 1.0f };
            // 日時・本文・発生元の表示文
            std::string display =
                std::string{ "[" }
                + timeBuffer.data()
                + "] ["
                + levelText
                + "] "
                + entry.message;
            if (entry.gameObjectId != 0)
            {
                display +=
                    "  [GameObject "
                    + std::to_string(
                        entry.gameObjectId)
                    + "]";
            }
            if (!sourceName.empty())
            {
                display +=
                    "  ("
                    + sourceName
                    + ":"
                    + std::to_string(
                        entry.sourceLine)
                    + ")";
            }

            ImGui::PushID(
                static_cast<int>(
                    entry.sequence
                    & 0x7fffffff));
            ImGui::PushStyleColor(
                ImGuiCol_Text,
                color);
            ImGui::Selectable(
                display.c_str(),
                false,
                ImGuiSelectableFlags_AllowDoubleClick);
            ImGui::PopStyleColor();

            if (ImGui::IsItemHovered()
                && ImGui::
                    IsMouseDoubleClicked(
                        ImGuiMouseButton_Left)
                && entry.gameObjectId != 0
                && m_scene.FindGameObject(
                    entry.gameObjectId)
                    != nullptr)
            {
                m_selectedObjectId =
                    entry.gameObjectId;
            }
            if (ImGui::
                BeginPopupContextItem(
                    "ConsoleEntryMenu"))
            {
                if (ImGui::MenuItem(
                        "メッセージをコピー"))
                {
                    ImGui::SetClipboardText(
                        entry.message.c_str());
                }
                if (ImGui::MenuItem(
                        "行全体をコピー"))
                {
                    ImGui::SetClipboardText(
                        display.c_str());
                }
                ImGui::EndPopup();
            }
            if (ImGui::IsItemHovered()
                && !entry.sourceFile.empty())
            {
                ImGui::SetTooltip(
                    "%s:%u",
                    entry.sourceFile.c_str(),
                    entry.sourceLine);
            }
            ImGui::PopID();
        }

        if (m_consoleAutoScroll
            && hasNewEntries
            && !m_consolePaused)
        {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();

        m_consoleLastSequence =
            newestSequence;
        ImGui::End();
    }

    // ヘルプとサポート画面の表示を予約します。
    void EditorLayer::OpenHelpCenter()
    {
        m_helpCenterRequested = true;
    }

    // ローカル文書の候補を順に調べ、見つからなければ空パスを返します。
    std::filesystem::path
        EditorLayer::LocalDocumentationIndexPath() const
    {
        // 実行ファイルの所在パス
        const auto executableDirectory =
            ExecutableDirectory();
        // ローカル文書の候補パス
        const std::array candidates{
            m_engineRoot / L"docs" / L"index.md",
            executableDirectory / L"docs" / L"index.md",
            executableDirectory.parent_path()
                / L"docs" / L"index.md"
        };
        // 存在を調べる文書パス
        for (const auto& candidate : candidates)
        {
            // 文書ファイルの確認エラー
            std::error_code error;
            if (std::filesystem::is_regular_file(
                candidate,
                error))
            {
                return candidate;
            }
        }
        return {};
    }

    // 既定ブラウザーでオンラインマニュアルを開き、起動成否を通知します。
    void EditorLayer::OpenOnlineManual()
    {
        // ブラウザー起動の結果
        const HINSTANCE result = ShellExecuteW(
            m_window,
            L"open",
            OnlineManualUrl,
            nullptr,
            nullptr,
            SW_SHOWNORMAL);
        if (reinterpret_cast<std::intptr_t>(result) <= 32)
        {
            SetStatus(
                "オンラインマニュアルを開けませんでした",
                true);
            return;
        }
        SetStatus("オンラインマニュアルを開きました");
    }

    // 存在するローカル文書を関連付けで開き、起動成否を通知します。
    void EditorLayer::OpenLocalDocumentation()
    {
        // ローカル文書の所在パス
        const auto documentation =
            LocalDocumentationIndexPath();
        if (documentation.empty())
        {
            SetStatus(
                "ローカルドキュメントが見つかりませんでした",
                true);
            return;
        }
        // 文書を開く起動結果
        const HINSTANCE result = ShellExecuteW(
            m_window,
            L"open",
            documentation.c_str(),
            nullptr,
            documentation.parent_path().c_str(),
            SW_SHOWNORMAL);
        if (reinterpret_cast<std::intptr_t>(result) <= 32)
        {
            SetStatus(
                "ローカルドキュメントを開けませんでした: "
                    + PathToUtf8(documentation),
                true);
            return;
        }
        SetStatus(
            "ローカルドキュメントを開きました: "
                + PathToUtf8(documentation));
    }

    // 存在するログまたは保存先を関連付けで開きます(openFolder: 保存先フォルダーを開くか)。
    void EditorLayer::OpenEditorLog(
        const bool openFolder)
    {
        // 出力中のログファイルパス
        const auto logPath =
            Logger::Instance().FilePath();
        // 開くログ・フォルダーのパス
        const auto target = openFolder
            ? logPath.parent_path()
            : logPath;
        // ログ所在の確認エラー
        std::error_code error;
        // 開く対象が存在する
        const bool exists = openFolder
            ? std::filesystem::is_directory(target, error)
            : std::filesystem::is_regular_file(target, error);
        if (target.empty() || !exists)
        {
            SetStatus(
                openFolder
                    ? "ログフォルダーが見つかりませんでした"
                    : "エディターログがまだ作成されていません",
                true);
            return;
        }
        // ログ閲覧の起動結果
        const HINSTANCE result = ShellExecuteW(
            m_window,
            L"open",
            target.c_str(),
            nullptr,
            openFolder
                ? nullptr
                : target.parent_path().c_str(),
            SW_SHOWNORMAL);
        if (reinterpret_cast<std::intptr_t>(result) <= 32)
        {
            SetStatus(
                openFolder
                    ? "ログフォルダーを開けませんでした"
                    : "エディターログを開けませんでした",
                true);
            return;
        }
        SetStatus(
            openFolder
                ? "ログフォルダーを開きました"
                : "エディターログを開きました");
    }

    // ビルド・動作モード・プロジェクト・シーン・ログの情報をクリップボードへコピーします。
    void EditorLayer::CopySupportInformation()
    {
        // プロジェクトのルートパス
        const auto projectRoot =
            ProjectSettingsPath().parent_path().
                parent_path();
        // 現在のログ出力パス
        const auto logPath =
            Logger::Instance().FilePath();
        // サポート情報の整形先
        std::ostringstream information;
        information
            << "LamaPon Engine: " << FormatBuildLabel()
            << '\n'
            << FormatBuildDetails()
            << '\n'
            << "Build configuration: "
            << m_buildConfiguration
            << '\n'
            << "Safe mode: "
            << (m_safeMode ? "yes" : "no")
            << '\n'
            << "Project: " << PathToUtf8(projectRoot)
            << '\n'
            << "Scene: "
            << (m_scenePath.empty()
                ? "(unsaved)"
                : PathToUtf8(m_scenePath))
            << '\n'
            << "Log: " << PathToUtf8(logPath);
        // コピーするサポート情報
        const std::string text = information.str();
        ImGui::SetClipboardText(text.c_str());
        SetStatus("サポート情報をクリップボードへコピーしました");
    }

    // ヘルプの表示予約を消費し、文書・ログ閲覧とサポート情報のコピーを描画します。
    void EditorLayer::DrawHelpCenter()
    {
        // ヘルプ画面のPopup ID
        constexpr const char* popupName =
            "ヘルプとサポート##HelpCenter";
        if (m_helpCenterRequested)
        {
            ImGui::OpenPopup(popupName);
            m_helpCenterRequested = false;
        }

        ImGui::SetNextWindowSize(
            ImVec2{ 720.0f, 470.0f },
            ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal(
            popupName,
            nullptr,
            ImGuiWindowFlags_None))
        {
            return;
        }

        // 実行中ビルドの識別情報
        const auto& buildInfo = GetBuildInfo();
        // 表示するビルド名
        const std::string buildLabel = FormatBuildLabel();
        ImGui::Text(
            "LamaPon Engine  %s",
            buildLabel.c_str());
        if (!buildInfo.commitSubject.empty())
        {
            ImGui::TextWrapped(
                "%.*s",
                static_cast<int>(buildInfo.commitSubject.size()),
                buildInfo.commitSubject.data());
        }
        ImGui::TextDisabled(
            "互換バージョン %.*s  /  %s",
            static_cast<int>(VersionString.size()),
            VersionString.data(),
            m_buildConfiguration.c_str());
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::BeginTable(
            "HelpSections",
            2,
            ImGuiTableFlags_BordersInnerV
                | ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("ドキュメント");
            ImGui::TextWrapped(
                "オンライン版と、エンジンに同梱されたローカル版を"
                "選べます。");
            if (ImGui::Button(
                "オンラインマニュアルを開く",
                ImVec2{ -1.0f, 0.0f }))
            {
                OpenOnlineManual();
            }
            // 利用可能なローカル文書
            const auto documentation =
                LocalDocumentationIndexPath();
            ImGui::BeginDisabled(documentation.empty());
            if (ImGui::Button(
                "ローカルドキュメントを開く",
                ImVec2{ -1.0f, 0.0f }))
            {
                OpenLocalDocumentation();
            }
            ImGui::EndDisabled();
            ImGui::TextDisabled(
                "%s",
                documentation.empty()
                    ? "ローカル版は見つかりませんでした"
                    : "場所: docs/index.md");
            if (!documentation.empty()
                && ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "%s",
                    PathToUtf8(documentation).c_str());
            }

            ImGui::TableNextColumn();
            ImGui::TextUnformatted("診断とサポート");
            ImGui::TextWrapped(
                "Consoleとログを確認し、問い合わせに必要な環境情報を"
                "まとめてコピーできます。");
            if (ImGui::Button(
                "コンソールを表示",
                ImVec2{ -1.0f, 0.0f }))
            {
                static_cast<void>(
                    m_editorExtensions.SetPanelOpen(
                        ConsolePanelId,
                        true));
                m_focusConsoleRequested = true;
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::Button(
                "エディターログを開く",
                ImVec2{ -1.0f, 0.0f }))
            {
                OpenEditorLog(false);
            }
            if (ImGui::Button(
                "ログフォルダーを開く",
                ImVec2{ -1.0f, 0.0f }))
            {
                OpenEditorLog(true);
            }
            if (ImGui::Button(
                "サポート情報をコピー",
                ImVec2{ -1.0f, 0.0f }))
            {
                CopySupportInformation();
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextWrapped(
            "ログ: %s",
            PathToUtf8(
                Logger::Instance().FilePath()).c_str());
        ImGui::TextDisabled(
            "不具合を報告するときは、サポート情報とログを添えると"
            "原因を追いやすくなります。");
        ImGui::Spacing();
        if (ImGui::Button(
            "閉じる",
            ImVec2{ 100.0f, 0.0f }))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // 描画時間・GPU・メモリ・物理・可視性と最新CPU計測を描画します(open: ウィンドウの開閉状態)。
    void EditorLayer::DrawPerformancePanel(bool& open)
    {
        if (!open)
        {
            return;
        }

        ImGui::SetNextWindowSize(
            ImVec2{ 560.0f, 300.0f },
            ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(
            "パフォーマンス",
                &open,
                ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            return;
        }

        // 直近フレームの描画統計
        const auto& frame =
            m_graphics.FrameStats();
        m_performanceFrameTimes[
            m_performanceSampleIndex] =
                frame.frameTimeMilliseconds;
        m_performanceCpuTimes[
            m_performanceSampleIndex] =
                frame.cpuTimeMilliseconds;
        m_performanceSampleIndex =
            (m_performanceSampleIndex + 1)
            % m_performanceFrameTimes.size();

        // 描画速度と経路の設定
        const auto& settings =
            m_graphics.Settings();
        // 目標フレーム時間・ms
        const float frameBudget =
            settings.targetFrameRate > 0
            ? 1000.0f
                / static_cast<float>(
                    settings.targetFrameRate)
            : 16.6667f;
        // 目標達成状態の表示色
        const ImVec4 frameColor =
            frame.frameTimeMilliseconds
                <= frameBudget
            ? ImVec4{
                0.35f,
                0.85f,
                0.55f,
                1.0f }
            : ImVec4{
                1.0f,
                0.45f,
                0.25f,
                1.0f };
        ImGui::TextColored(
            frameColor,
            "%.1f FPS",
            frame.framesPerSecond);
        ImGui::SameLine();
        ImGui::Text(
            "  Frame %.2f ms  CPU+Present %.2f ms",
            frame.frameTimeMilliseconds,
            frame.cpuTimeMilliseconds);
        ImGui::Text(
            "VSync: %s  FPS上限: %s",
            settings.vSyncEnabled
                ? "ON"
                : "OFF",
            settings.targetFrameRate == 0
                ? "無制限"
                : std::to_string(
                    settings.targetFrameRate)
                    .c_str());

        if (settings.vSyncEnabled)
        {
            ImGui::TextDisabled(
                "VSyncが有効なので、実際の上限はモニターの"
                "リフレッシュレートです");
        }
        else if (!m_graphics.TearingAllowed())
        {
            ImGui::TextDisabled(
                "この環境はティアリング許可に対応していないため、"
                "実際の上限はモニターのリフレッシュレートです");
        }

        // 時間グラフの表示上限・ms
        const float graphMaximum =
            std::max(
                33.3333f,
                *std::max_element(
                    m_performanceFrameTimes.begin(),
                    m_performanceFrameTimes.end())
                    * 1.1f);
        ImGui::PlotLines(
            "Frame ms",
            m_performanceFrameTimes.data(),
            static_cast<int>(
                m_performanceFrameTimes.size()),
            static_cast<int>(
                m_performanceSampleIndex),
            nullptr,
            0.0f,
            graphMaximum,
            ImVec2{ 0.0f, 54.0f });
        ImGui::PlotLines(
            "CPU+Present ms",
            m_performanceCpuTimes.data(),
            static_cast<int>(
                m_performanceCpuTimes.size()),
            static_cast<int>(
                m_performanceSampleIndex),
            nullptr,
            0.0f,
            graphMaximum,
            ImVec2{ 0.0f, 54.0f });

        ImGui::SeparatorText("GPU");
        // GPU計測結果の取得元
        const auto& gpu = m_graphics.Gpu();
        if (!gpu.IsSupported())
        {
            ImGui::TextDisabled(
                "この環境ではGPU計測を利用できません。");
        }
        else
        {
            // GPU計測値は非同期回収した数フレーム前の結果です。
            ImGui::Text(
                "GPU合計 %.2f ms",
                gpu.LatestFrameMilliseconds());
            // 確定済みGPUパイプライン統計
            const auto& pipeline =
                gpu.LatestPipelineStatistics();
            if (pipeline.valid)
            {
                ImGui::Text(
                    "GPU workload: IA %llu primitives / %llu vertices",
                    static_cast<unsigned long long>(
                        pipeline.inputAssemblerPrimitives),
                    static_cast<unsigned long long>(
                        pipeline.inputAssemblerVertices));
                ImGui::Text(
                    "Shader calls: VS %llu  PS %llu  CS %llu",
                    static_cast<unsigned long long>(
                        pipeline.vertexShaderInvocations),
                    static_cast<unsigned long long>(
                        pipeline.pixelShaderInvocations),
                    static_cast<unsigned long long>(
                        pipeline.computeShaderInvocations));
            }
            // 区間は入れ子にできるので、深さでインデントし、合計は最上位（depth==0）だけを足します。
            // 最上位GPU区間の合計・ms
            // 入れ子の区間を二重集計しないよう、depth==0だけを合計します。
            float topLevelTotal = 0.0f;
            // 確定済みのGPU計測区間
            for (const auto& section :
                gpu.LatestSections())
            {
                if (section.depth == 0)
                {
                    topLevelTotal += section.milliseconds;
                }
                ImGui::Text(
                    "%*s%s: %.2f ms",
                    static_cast<int>(
                        2 + section.depth * 2),
                    "",
                    section.name.c_str(),
                    section.milliseconds);
            }
            // 区間で囲われていないGPU作業には、Present待ちや計測対象外の描画が含まれます。
            // GPU合計との差分・ms
            const float unmeasured =
                gpu.LatestFrameMilliseconds()
                - topLevelTotal;
            if (unmeasured > 0.01f)
            {
                ImGui::TextDisabled(
                    "  その他（未計測）: %.2f ms",
                    unmeasured);
            }
        }

        ImGui::SeparatorText("メモリ");
        // 使用メモリとVRAM予算の統計
        const auto& memory = m_graphics.MemoryStats();
        // MiBへ変換するbyte数
        constexpr double bytesPerMiB = 1024.0 * 1024.0;
        ImGui::Text(
            "Process RAM: working %.1f MiB  private %.1f MiB",
            static_cast<double>(memory.processWorkingSetBytes)
                / bytesPerMiB,
            static_cast<double>(memory.processPrivateBytes)
                / bytesPerMiB);
        ImGui::Text(
            "System RAM: %.1f / %.1f MiB",
            static_cast<double>(memory.systemPhysicalUsedBytes)
                / bytesPerMiB,
            static_cast<double>(memory.systemPhysicalTotalBytes)
                / bytesPerMiB);
        if (memory.videoMemoryBudgetAvailable)
        {
            ImGui::Text(
                "VRAM local: %.1f / %.1f MiB  dedicated: %.1f MiB",
                static_cast<double>(
                    memory.localVideoMemoryUsageBytes) / bytesPerMiB,
                static_cast<double>(
                    memory.localVideoMemoryBudgetBytes) / bytesPerMiB,
                static_cast<double>(
                    memory.dedicatedVideoMemoryBytes) / bytesPerMiB);
            if (memory.nonLocalVideoMemoryBudgetBytes > 0)
            {
                ImGui::Text(
                    "VRAM non-local: %.1f / %.1f MiB  shared: %.1f MiB",
                    static_cast<double>(
                        memory.nonLocalVideoMemoryUsageBytes)
                        / bytesPerMiB,
                    static_cast<double>(
                        memory.nonLocalVideoMemoryBudgetBytes)
                        / bytesPerMiB,
                    static_cast<double>(
                        memory.sharedSystemMemoryBytes)
                        / bytesPerMiB);
            }
        }
        else
        {
            ImGui::TextDisabled(
                "このアダプターではDXGIのVRAM予算を取得できません。");
        }

        ImGui::SeparatorText("固定物理");
        // 補間が有効なボディを数えます(object: 設定を調べる対象)。
        const std::size_t interpolatedBodies =
            static_cast<std::size_t>(
                std::ranges::count_if(
                    m_scene.GameObjects(),
                    [](const auto& object)
                    {
                        // 補間設定を調べる物理ボディ
                        const auto* body =
                            object->template GetComponent<
                                RigidbodyComponent>();
                        return body != nullptr
                            && body->IsEnabled()
                            && body->Interpolates();
                    }));
        ImGui::Text(
            "Rate: 60 Hz  Steps: %zu  Interpolation α: %.2f  Bodies: %zu",
            m_scene.PhysicsFixedStepsLastFrame(),
            m_scene.PhysicsInterpolationAlpha(),
            interpolatedBodies);
        // 直近の物理演算統計
        const auto& physics =
            m_scene.PhysicsStats();
        ImGui::Text(
            "Collider 2D/3D: %zu / %zu  Candidate: %zu  Narrow: %zu  Contact: %zu",
            physics.colliderCount2D,
            physics.colliderCount3D,
            physics.candidatePairCount2D
                + physics.candidatePairCount3D,
            physics.narrowPhaseTestCount2D
                + physics.narrowPhaseTestCount3D,
            physics.activeContactCount);

        // 直近の可視性判定統計
        const auto& visibility =
            m_scene.VisibilityStats();
        ImGui::SeparatorText("描画");

        ImGui::Text(
            "描画方式: %s",
            settings.renderingPath
                == RenderingPath::ForwardPlus
                ? "Forward+（クラスタライトカリング）"
                : "Forward（ライト上限まで）");
        ImGui::Text(
            "Renderer: %zu  Visible: %zu  Frustum: %zu  Occlusion: %zu  LOD: %zu",
            visibility.rendererCount,
            visibility.visibleRendererCount,
            visibility.frustumCulledCount,
            visibility.occlusionCulledCount,
            visibility.lodCulledCount);
        ImGui::Text(
            "BVH: %zu nodes / %zu tests  Cache: %s",
            visibility.spatialNodeCount,
            visibility.spatialNodeTestCount,
            visibility.spatialIndexReused
                ? "reuse"
                : "rebuild");
        ImGui::Text(
            "Auto LOD: %zu renderers  Saved: %llu triangles",
            visibility.automaticLodRendererCount,
            static_cast<unsigned long long>(
                visibility.automaticLodTrianglesSaved));
        ImGui::Text(
            "Instancing mesh/model: %zu/%zu batches  %zu/%zu renderers",
            visibility.meshInstanceBatchCount,
            visibility.modelInstanceBatchCount,
            visibility.meshInstancedRendererCount,
            visibility.modelInstancedRendererCount);
        ImGui::Text(
            "Asset upload: texture queue %zu  model queue %zu  model %.2f MiB/frame",
            m_graphics.Assets().PendingTextureUploadCount(),
            m_graphics.Assets().PendingModelUploadCount(),
            static_cast<double>(
                m_graphics.Assets().ModelUploadBytesLastFrame())
                / (1024.0 * 1024.0));

        ImGui::SeparatorText("CPUプロファイラー");

        if (ImGui::SmallButton("プロファイラーを開く"))
        {
            static_cast<void>(
                m_editorExtensions.SetPanelOpen(ProfilerPanelId, true));
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("メモリプロファイラーを開く"))
        {
            static_cast<void>(m_editorExtensions.SetPanelOpen(
                MemoryProfilerPanelId,
                true));
        }
        // CPU計測の設定・出力元
        auto& profiler = Profiler::Instance();
        // CPUフレーム計測の有効状態
        bool profilerEnabled = profiler.IsEnabled();
        if (ImGui::Checkbox(
                "フレーム計測を有効化",
                &profilerEnabled))
        {
            profiler.SetEnabled(profilerEnabled);
        }
        ImGui::SameLine();
        if (ImGui::Button("JSONを書き出す"))
        {
            // CPU計測JSONの出力先
            const auto profilePath =
                m_graphics.Assets().AssetRoot().
                    parent_path()
                / ".lamapon"
                / "profile.json";
            if (profiler.WriteJson(profilePath))
            {
                SetStatus(
                    "プロファイルを書き出しました: "
                    + PathToUtf8(profilePath));
            }
            else
            {
                SetStatus(
                    "プロファイルを書き出せませんでした",
                    true);
            }
        }

        // 保存済みのCPU計測フレーム
        const auto profileFrames = profiler.Snapshot();
        if (!profileFrames.empty()
            && ImGui::BeginTable(
                "ProfilerSamples",
                3,
                ImGuiTableFlags_Borders
                    | ImGuiTableFlags_RowBg
                    | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("区間");
            ImGui::TableSetupColumn(
                "ms",
                ImGuiTableColumnFlags_WidthFixed,
                90.0f);
            ImGui::TableSetupColumn(
                "呼出",
                ImGuiTableColumnFlags_WidthFixed,
                60.0f);
            ImGui::TableHeadersRow();
            // 最新フレームのCPU計測区間
            for (const auto& sample :
                profileFrames.back().samples)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);

                ImGui::Text(
                    "%*s%s",
                    static_cast<int>(sample.depth * 2),
                    "",
                    sample.name.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.3f", sample.milliseconds);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%u", sample.callCount);
            }
            ImGui::EndTable();
        }
        ImGui::End();
    }

    // 保存先と確認対象の変化を反映し、個別設定・セーブ・同期・保護データの操作を描画します(open: ウィンドウの開閉状態)。
    void EditorLayer::DrawPersistencePanel(bool& open)
    {
        static_cast<void>(
            m_persistencePanelState.SynchronizeBinding(
                m_playerPrefs.FilePath(),
                m_saveData.Directory()));
        if (!open)
        {
            m_persistencePanelState.DismissOnlineConfirmation();
            return;
        }

        // オンライン認証の状態
        const auto accountState = m_onlineServices.State();
        // オンラインへサインイン済み
        const bool onlineAccountActive =
            m_onlineServices.IsSignedIn();
        // クラウド同期の状態
        const auto syncStatus =
            m_onlineServices.CloudSyncStatus();
        // 現在のクラウド競合一覧
        std::vector<OnlineCloudConflict> cloudConflicts;
        // 競合一覧の取得に成功した
        bool cloudConflictsAvailable = true;
        try
        {
            cloudConflicts = m_onlineServices.CloudConflicts();
        }
        catch (const std::exception&)
        {
            // 競合情報の取得失敗は詳細を表示せず、対象消失として既存の確認を閉じます。
            cloudConflictsAvailable = false;
        }
        // 保護データの復旧状態
        const auto recoveryStatus =
            m_onlineServices.PersistenceRecoveryStatus();
        // 確認中のオンライン操作種別
        const auto confirmationKind =
            m_persistencePanelState.OnlineConfirmationKind();
        // 競合解決の確認中
        const bool confirmingConflict =
            confirmationKind
                == Detail::PersistenceConfirmationKind::UseRemote
            || confirmationKind
                == Detail::PersistenceConfirmationKind::RetryLocal;
        // 確認中の競合が現在も存在する
        bool confirmationConflictStillExists = false;
        if (confirmingConflict)
        {
            // 確認対象の不透明な競合ID
            const auto& confirmationConflictId =
                m_persistencePanelState.ConfirmationConflictId();
            // 確認対象が残っているか調べます(conflict: 現在の競合)。
            confirmationConflictStillExists =
                std::ranges::any_of(
                    cloudConflicts,
                    [&confirmationConflictId](const auto& conflict)
                    {
                        return conflict.id == confirmationConflictId;
                    });
        }
        // アカウント・競合・復旧改訂が変わった操作確認を失効させます。
        static_cast<void>(
            m_persistencePanelState.SynchronizeOnlineConfirmation(
                onlineAccountActive,
                confirmationConflictStillExists,
                recoveryStatus.revision));

        ImGui::SetNextWindowSize(
            ImVec2{ 720.0f, 620.0f },
            ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(
            "セーブデータ",
                &open,
                ImGuiWindowFlags_NoCollapse))
        {
            m_persistencePanelState.DismissOnlineConfirmation();
            ImGui::End();
            return;
        }
        // 旧保存先の全削除確認を閉じる
        const bool closeStaleDeleteAllPopup =
            m_persistencePanelState.CloseDeleteAllPopupRequested();
        // 失効したオンライン確認を閉じる
        bool closeStaleOnlinePopup =
            m_persistencePanelState.
                CloseOnlineConfirmationPopupRequested();
        if (closeStaleOnlinePopup
            && !ImGui::IsPopupOpen(
                OnlinePersistenceConfirmationPopup))
        {
            m_persistencePanelState.
                AcknowledgeCloseOnlineConfirmationPopup();
            closeStaleOnlinePopup = false;
        }

        // 操作結果を固定文で通知します(result: オンライン操作結果, successMessage: 成功時の表示文)。
        const auto reportOnlineOperationResult =
            [this](
                const OnlinePersistenceOperationResult result,
                const char* const successMessage)
            {
                switch (result)
                {
                case OnlinePersistenceOperationResult::Succeeded:
                    SetStatus(successMessage);
                    break;
                case OnlinePersistenceOperationResult::Unavailable:
                    SetStatus(
                        "現在の状態では操作できません",
                        true);
                    break;
                case OnlinePersistenceOperationResult::Busy:
                    SetStatus(
                        "別のオンライン操作を処理しています",
                        true);
                    break;
                case OnlinePersistenceOperationResult::Stale:
                    SetStatus(
                        "対象の状態が更新されました。もう一度選択してください",
                        true);
                    break;
                case OnlinePersistenceOperationResult::Failed:
                    SetStatus(
                        "オンライン操作を完了できませんでした",
                        true);
                    break;
                }
            };

        ImGui::SeparatorText("オンラインアカウント");
        ImGui::Text(
            "状態: %s",
            OnlineAccountStateLabel(accountState));
        if (onlineAccountActive)
        {
            // 認証したプレイヤーの表示名
            const auto& displayName =
                m_onlineServices.Player().displayName;
            if (displayName.empty())
            {
                ImGui::TextUnformatted(
                    "アカウント: ログイン済み");
            }
            else
            {
                ImGui::Text(
                    "アカウント: %s",
                    displayName.c_str());
            }
        }

        // 保護データの復旧待ちがある
        const bool recoveryPending =
            recoveryStatus.state
            != OnlinePersistenceRecoveryState::None;
        switch (accountState)
        {
        case OnlineAccountState::Unconfigured:
            ImGui::TextDisabled(
                "プロジェクト設定でオンライン接続を設定してください");
            if (ImGui::Button("オンライン設定を開く"))
            {
                m_projectSettingsCategory = 8;
                OpenProjectSettingsDialog();
            }
            break;
        case OnlineAccountState::SignedOut:
        case OnlineAccountState::Error:
            ImGui::BeginDisabled(recoveryPending);
            if (ImGui::Button("Discordでログイン"))
            {
                // Discord認証の開始成功
                bool started = false;
                try
                {
                    started =
                        m_onlineServices.BeginDiscordSignIn();
                }
                catch (const std::exception&)
                {
                    // 例外の接続先や内部状態を表示せず、固定文で開始失敗を通知します。
                }
                SetStatus(
                    started
                        ? "Discordログインを開始しました"
                        : "Discordログインを開始できませんでした",
                    !started);
            }
            ImGui::EndDisabled();
            if (recoveryPending)
            {
                ImGui::SameLine();
                ImGui::TextDisabled(
                    "先に保護データを復元または破棄してください");
            }
            break;
        case OnlineAccountState::StartingSignIn:
        case OnlineAccountState::WaitingForAuthorization:
        case OnlineAccountState::PollingAuthorization:
            if (ImGui::Button("ログインをキャンセル"))
            {
                m_onlineServices.CancelDiscordSignIn();
                SetStatus("Discordログインをキャンセルしました");
            }
            break;
        case OnlineAccountState::SignedIn:
        case OnlineAccountState::RefreshingSession:
            if (ImGui::Button("サインアウト"))
            {
                try
                {
                    m_onlineServices.SignOut();
                    SetStatus("サインアウトを開始しました");
                }
                catch (const std::exception&)
                {
                    SetStatus(
                        "サインアウトを開始できませんでした",
                        true);
                }
            }
            break;
        case OnlineAccountState::SigningOut:
        case OnlineAccountState::RestoringSession:
            break;
        }

        ImGui::SeparatorText("クラウド同期");
        ImGui::Text(
            "状態: %s",
            OnlineCloudSyncStateLabel(syncStatus.state));
        if (syncStatus.state
                == OnlineCloudSyncState::WaitingToRetry
            && syncStatus.retryAfterSeconds > 0.0f)
        {
            ImGui::TextDisabled(
                "約%.0f秒後に再試行します",
                syncStatus.retryAfterSeconds);
        }
        if (syncStatus.stopReason
            != OnlineCloudSyncStopReason::None)
        {
            ImGui::TextWrapped(
                "理由: %s",
                OnlineCloudSyncStopReasonLabel(
                    syncStatus.stopReason));
        }
        ImGui::BeginDisabled(!onlineAccountActive);
        if (ImGui::Button("今すぐ同期"))
        {
            reportOnlineOperationResult(
                m_onlineServices.RequestCloudSync(),
                "クラウド同期を要求しました");
        }
        ImGui::EndDisabled();

        ImGui::SeparatorText("保護データの復旧");
        ImGui::TextWrapped(
            "%s",
            OnlineRecoveryStateLabel(recoveryStatus.state));
        if (recoveryPending)
        {
            // 保護データを復元可能
            const bool restoreAvailable =
                recoveryStatus.state
                    == OnlinePersistenceRecoveryState::MemorySnapshot
                || recoveryStatus.state
                    == OnlinePersistenceRecoveryState::DurableSidecar;
            ImGui::BeginDisabled(!restoreAvailable);
            if (ImGui::Button("保護データを復元"))
            {
                m_persistencePanelState.BeginRecoveryConfirmation(
                    Detail::PersistenceConfirmationKind::Restore,
                    recoveryStatus.revision);
                ImGui::OpenPopup(
                    OnlinePersistenceConfirmationPopup);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("保護データを破棄"))
            {
                m_persistencePanelState.BeginRecoveryConfirmation(
                    Detail::PersistenceConfirmationKind::Discard,
                    recoveryStatus.revision);
                ImGui::OpenPopup(
                    OnlinePersistenceConfirmationPopup);
            }
            if (!restoreAvailable)
            {
                ImGui::TextDisabled(
                    "読み取り可能になるまで復元は実行できません");
            }
        }

        ImGui::SeparatorText("同期の競合");
        if (!cloudConflictsAvailable)
        {
            ImGui::TextDisabled("競合情報を確認できません");
        }
        else if (cloudConflicts.empty())
        {
            ImGui::TextDisabled("未解決の競合はありません");
        }
        else
        {
            ImGui::TextDisabled(
                "%zu件の競合があります",
                cloudConflicts.size());
            // 競合一覧の表示番号
            for (std::size_t index = 0;
                index < cloudConflicts.size();
                ++index)
            {
                // 表示する競合の状態
                const auto& conflict = cloudConflicts[index];
                // 不透明な競合IDはUI内部の識別だけに使い、表示文や記録へ渡しません。
                ImGui::PushID(conflict.id.c_str());
                if (conflict.kind
                    == OnlineCloudResourceKind::Preferences)
                {
                    ImGui::TextUnformatted("PlayerPrefs");
                }
                else
                {
                    ImGui::Text(
                        "セーブスロット: %s",
                        conflict.slot.c_str());
                }
                ImGui::TextDisabled(
                    "端末側: %s / クラウド側: %s",
                    conflict.localDeleted
                        ? "削除済み"
                        : "保存データあり",
                    conflict.remoteDeleted
                        ? "削除済み"
                        : "保存データあり");
                if (ImGui::SmallButton("クラウド版を使用"))
                {
                    m_persistencePanelState.BeginConflictConfirmation(
                        Detail::PersistenceConfirmationKind::UseRemote,
                        conflict.id);
                    ImGui::OpenPopup(
                        OnlinePersistenceConfirmationPopup);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(
                        "競合時点の端末版を再送"))
                {
                    m_persistencePanelState.BeginConflictConfirmation(
                        Detail::PersistenceConfirmationKind::RetryLocal,
                        conflict.id);
                    ImGui::OpenPopup(
                        OnlinePersistenceConfirmationPopup);
                }
                ImGui::PopID();
            }
        }

        if (ImGui::BeginPopupModal(
                OnlinePersistenceConfirmationPopup,
                nullptr,
                ImGuiWindowFlags_AlwaysAutoResize))
        {
            // Popupが確認中の操作種別
            const auto pendingKind =
                m_persistencePanelState.OnlineConfirmationKind();
            if (closeStaleOnlinePopup
                || pendingKind
                    == Detail::PersistenceConfirmationKind::None)
            {
                ImGui::CloseCurrentPopup();
                m_persistencePanelState.
                    AcknowledgeCloseOnlineConfirmationPopup();
            }
            else
            {
                switch (pendingKind)
                {
                case Detail::PersistenceConfirmationKind::UseRemote:
                    ImGui::TextWrapped(
                        "端末の競合版をクラウド版で置き換えます。"
                        "クラウド側が削除済みの場合は端末側も削除されます。");
                    break;
                case Detail::PersistenceConfirmationKind::RetryLocal:
                    ImGui::TextWrapped(
                        "競合が起きた時点の端末版をクラウドへ再送します。"
                        "競合後の端末変更は、解決後の同期対象として残ります。");
                    break;
                case Detail::PersistenceConfirmationKind::Restore:
                    ImGui::TextWrapped(
                        "保護したデータを元のオンラインアカウントへ復元します。"
                        "現在の保存内容は上書きされます。");
                    break;
                case Detail::PersistenceConfirmationKind::Discard:
                    ImGui::TextWrapped(
                        "保護したデータを完全に破棄します。"
                        "この操作は取り消せません。");
                    break;
                case Detail::PersistenceConfirmationKind::None:
                    break;
                }

                if (ImGui::Button("実行する"))
                {
                    // 確認したオンライン操作の結果
                    OnlinePersistenceOperationResult result{
                        OnlinePersistenceOperationResult::Failed
                    };
                    // 成功時に表示する通知文
                    const char* successMessage =
                        "オンライン操作を完了しました";
                    switch (pendingKind)
                    {
                    case Detail::PersistenceConfirmationKind::UseRemote:
                        result = m_onlineServices.ResolveCloudConflict(
                            m_persistencePanelState.
                                ConfirmationConflictId(),
                            OnlineCloudConflictResolution::UseRemote);
                        successMessage =
                            "クラウド版を端末へ反映しました";
                        break;
                    case Detail::PersistenceConfirmationKind::RetryLocal:
                        result = m_onlineServices.ResolveCloudConflict(
                            m_persistencePanelState.
                                ConfirmationConflictId(),
                            OnlineCloudConflictResolution::UseLocal);
                        successMessage =
                            "競合時点の端末版を再送キューへ戻しました";
                        break;
                    case Detail::PersistenceConfirmationKind::Restore:
                        result = m_onlineServices.RestorePersistence(
                            m_persistencePanelState.
                                ConfirmationRecoveryRevision());
                        successMessage =
                            "保護データを復元しました";
                        break;
                    case Detail::PersistenceConfirmationKind::Discard:
                        result = m_onlineServices.DiscardPersistence(
                            m_persistencePanelState.
                                ConfirmationRecoveryRevision());
                        successMessage =
                            "保護データを破棄しました";
                        break;
                    case Detail::PersistenceConfirmationKind::None:
                        break;
                    }
                    reportOnlineOperationResult(
                        result,
                        successMessage);
                    m_persistencePanelState.
                        DismissOnlineConfirmation();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("キャンセル"))
                {
                    m_persistencePanelState.
                        DismissOnlineConfirmation();
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
        else if (closeStaleOnlinePopup
            && !ImGui::IsPopupOpen(
                OnlinePersistenceConfirmationPopup))
        {
            m_persistencePanelState.
                AcknowledgeCloseOnlineConfirmationPopup();
        }

        ImGui::SeparatorText("ローカル保存");
        if (onlineAccountActive)
        {
            ImGui::TextUnformatted(
                "プロファイル: オンラインアカウント（アカウント専用・固定）");
        }
        else
        {
            ImGui::TextUnformatted("プロファイル: ゲスト");
            ImGui::TextWrapped(
                "保存先: %s",
                PathToUtf8(
                    m_playerPrefs.FilePath().
                        parent_path()).c_str());
        }

        // 保存先変更を識別する改訂番号
        const auto persistenceBindingRevision =
            m_persistencePanelState.BindingRevision();
        ImGui::PushID(static_cast<int>(
            static_cast<std::uint32_t>(persistenceBindingRevision)));
        ImGui::PushID(static_cast<int>(
            static_cast<std::uint32_t>(
                persistenceBindingRevision >> 32u)));
        if (ImGui::Button("PlayerPrefsを保存"))
        {
            try
            {
                m_playerPrefs.Save();
                SetStatus(
                    "PlayerPrefsを保存しました");
            }
            catch (const std::exception&)
            {
                SetStatus(
                    "PlayerPrefsを保存できませんでした",
                    true);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("再読み込み"))
        {
            try
            {
                m_playerPrefs.Load();
                SetStatus(
                    "PlayerPrefsを再読み込みしました");
            }
            catch (const std::exception&)
            {
                SetStatus(
                    "PlayerPrefsを再読み込みできませんでした",
                    true);
            }
        }
        if (!onlineAccountActive)
        {
            ImGui::SameLine();
            if (ImGui::Button("保存フォルダーを開く"))
            {
                std::filesystem::create_directories(
                    m_playerPrefs.FilePath().
                        parent_path());
                ShellExecuteW(
                    m_window,
                    L"open",
                    m_playerPrefs.FilePath().
                        parent_path().c_str(),
                    nullptr,
                    nullptr,
                    SW_SHOWNORMAL);
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled(
            m_playerPrefs.IsDirty()
                ? "未保存の変更あり"
                : "保存済み");

        if (ImGui::BeginTable(
                "PlayerPrefsTable",
                4,
                ImGuiTableFlags_Borders
                    | ImGuiTableFlags_RowBg
                    | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("キー");
            ImGui::TableSetupColumn("型");
            ImGui::TableSetupColumn("値");
            ImGui::TableSetupColumn(
                "操作",
                ImGuiTableColumnFlags_WidthFixed,
                64.0f);
            ImGui::TableHeadersRow();
            // 表示・編集する個別設定のキー
            for (const auto& key :
                m_playerPrefs.Keys())
            {
                ImGui::PushID(key.c_str());
                // 個別設定の保存型
                const auto type =
                    m_playerPrefs.TypeOf(key);
                // 個別設定の型表示名
                std::string typeName;
                // 表示・入力する個別設定値
                std::string value;
                switch (type)
                {
                case PlayerPrefType::Integer:
                    typeName = "整数";
                    value = std::to_string(
                        m_playerPrefs.GetInteger(key));
                    break;
                case PlayerPrefType::Number:
                    typeName = "小数";
                    value = std::to_string(
                        m_playerPrefs.GetNumber(key));
                    break;
                case PlayerPrefType::Boolean:
                    typeName = "真偽値";
                    value = m_playerPrefs.GetBoolean(key)
                        ? "true"
                        : "false";
                    break;
                case PlayerPrefType::String:
                    typeName = "文字列";
                    value =
                        m_playerPrefs.GetString(key);
                    break;
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(key.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(typeName.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(value.c_str());
                ImGui::TableSetColumnIndex(3);
                if (ImGui::SmallButton("削除"))
                {
                    m_playerPrefs.DeleteKey(key);
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::InputText(
            "キー##PlayerPref",
            m_persistencePanelState.playerPrefKey.data(),
            m_persistencePanelState.playerPrefKey.size());
        // 個別設定の入力型表示一覧
        constexpr const char* types[]{
            "整数",
            "小数",
            "真偽値",
            "文字列"
        };
        ImGui::Combo(
            "型##PlayerPref",
            &m_persistencePanelState.playerPrefType,
            types,
            static_cast<int>(std::size(types)));
        if (m_persistencePanelState.playerPrefType == 2)
        {
            ImGui::Checkbox(
                "値##PlayerPrefBoolean",
                &m_persistencePanelState.playerPrefBoolean);
        }
        else
        {
            ImGui::InputText(
                "値##PlayerPref",
                m_persistencePanelState.playerPrefValue.data(),
                m_persistencePanelState.playerPrefValue.size());
        }
        if (ImGui::Button("追加／更新"))
        {
            try
            {
                // 表示・編集する個別設定のキー
                const std::string key(
                    m_persistencePanelState.playerPrefKey.data());
                // 表示・入力する個別設定値
                const std::string value(
                    m_persistencePanelState.playerPrefValue.data());
                switch (m_persistencePanelState.playerPrefType)
                {
                case 0:
                    m_playerPrefs.SetInteger(
                        key,
                        std::stoll(value));
                    break;
                case 1:
                    m_playerPrefs.SetNumber(
                        key,
                        std::stod(value));
                    break;
                case 2:
                    m_playerPrefs.SetBoolean(
                        key,
                        m_persistencePanelState.playerPrefBoolean);
                    break;
                default:
                    m_playerPrefs.SetString(
                        key,
                        value);
                    break;
                }
                SetStatus(
                    "PlayerPrefsを更新しました");
            }
            catch (const std::exception&)
            {
                SetStatus(
                    "PlayerPrefsを更新できませんでした",
                    true);
            }
        }
        ImGui::PopID();
        ImGui::PopID();
        ImGui::SameLine();
        if (ImGui::Button("すべて削除"))
        {
            ImGui::OpenPopup(
                "DeleteAllPlayerPrefs");
        }
        if (ImGui::BeginPopupModal(
                "DeleteAllPlayerPrefs",
                nullptr,
                ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (closeStaleDeleteAllPopup)
            {
                ImGui::CloseCurrentPopup();
                m_persistencePanelState.
                    AcknowledgeCloseDeleteAllPopup();
            }
            else
            {
                ImGui::TextUnformatted(
                    "すべてのPlayerPrefsを削除しますか？");
                if (ImGui::Button("削除する"))
                {
                    m_playerPrefs.DeleteAll();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("キャンセル"))
                {
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
        else if (closeStaleDeleteAllPopup
            && !ImGui::IsPopupOpen("DeleteAllPlayerPrefs"))
        {
            // Popupが存在しないと確認できた時点で、閉じる要求を消費します。
            m_persistencePanelState.AcknowledgeCloseDeleteAllPopup();
        }

        ImGui::PushID(static_cast<int>(
            static_cast<std::uint32_t>(persistenceBindingRevision)));
        ImGui::PushID(static_cast<int>(
            static_cast<std::uint32_t>(
                persistenceBindingRevision >> 32u)));
        ImGui::SeparatorText("JSONセーブスロット");
        ImGui::BeginChild(
            "SaveSlotList",
            ImVec2{ 190.0f, 150.0f },
            ImGuiChildFlags_Borders);
        // 選択するセーブスロット名
        for (const auto& slot :
            m_saveData.ListSlots())
        {
            if (ImGui::Selectable(
                    slot.c_str(),
                    slot
                        == m_persistencePanelState.selectedSaveSlot))
            {
                try
                {
                    m_persistencePanelState.selectedSaveSlot = slot;
                    strncpy_s(
                        m_persistencePanelState.saveSlot.data(),
                        m_persistencePanelState.saveSlot.size(),
                        slot.c_str(),
                        _TRUNCATE);
                    // 選択スロットのJSON本文
                    const auto json =
                        m_saveData.LoadJson(slot);
                    strncpy_s(
                        m_persistencePanelState.saveJson.data(),
                        m_persistencePanelState.saveJson.size(),
                        json
                            ? json->c_str()
                            : "{}",
                        _TRUNCATE);
                }
                catch (const std::exception&)
                {
                    SetStatus(
                        "セーブスロットを読み込めませんでした",
                        true);
                }
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::InputText(
            "スロット名",
            m_persistencePanelState.saveSlot.data(),
            m_persistencePanelState.saveSlot.size());
        ImGui::InputTextMultiline(
            "JSON",
            m_persistencePanelState.saveJson.data(),
            m_persistencePanelState.saveJson.size(),
            ImVec2{ -1.0f, 92.0f });
        if (ImGui::Button("スロットを保存"))
        {
            try
            {
                m_saveData.SaveJson(
                    m_persistencePanelState.saveSlot.data(),
                    m_persistencePanelState.saveJson.data());
                m_persistencePanelState.selectedSaveSlot =
                    m_persistencePanelState.saveSlot.data();
                SetStatus(
                    "セーブスロットを保存しました");
            }
            catch (const std::exception&)
            {
                SetStatus(
                    "セーブスロットを保存できませんでした",
                    true);
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(
            m_persistencePanelState.selectedSaveSlot.empty());
        if (ImGui::Button("スロットを削除"))
        {
            try
            {
                m_saveData.DeleteSlot(
                    m_persistencePanelState.selectedSaveSlot);
                m_persistencePanelState.selectedSaveSlot.clear();
                m_persistencePanelState.saveSlot = {};
                m_persistencePanelState.saveJson = { '{', '}', '\0' };
                SetStatus(
                    "セーブスロットを削除しました");
            }
            catch (const std::exception&)
            {
                SetStatus(
                    "セーブスロットを削除できませんでした",
                    true);
            }
        }
        ImGui::EndDisabled();
        ImGui::EndGroup();
        ImGui::PopID();
        ImGui::PopID();

        ImGui::End();
    }

    // 階層・名前検索・選択・ドロップを描画し、走査完了後に予約した変更を実行します。
    void EditorLayer::DrawHierarchy()
    {
        ImGui::SetNextWindowSize(
            ImVec2{ HierarchyWidth, 420.0f },
            ImGuiCond_FirstUseEver);

        // ヒエラルキー画面の表示フラグ
        constexpr ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoCollapse;

        ImGui::Begin("ヒエラルキー", nullptr, flags);

        m_hierarchyContextAction =
            HierarchyContextAction::None;


        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint(
            "##HierarchyFilter",
            "名前で検索",
            m_hierarchyFilter.data(),
            m_hierarchyFilter.size());
        // 対象名の検索文字列
        const std::string hierarchyFilter =
            m_hierarchyFilter.data();
        // 選択中のオブジェクト数
        const int selectionCount =
            static_cast<int>(SelectedObjects().size());
        if (selectionCount > 1)
        {
            ImGui::TextDisabled(
                "%d個を選択中（Ctrl+クリックで増減）",
                selectionCount);
        }

        ImGui::Selectable(
            "シーンルート（ここへドロップ）",
            m_selectedObjectId == 0);
        if (ImGui::IsItemClicked())
        {
            SelectObject(0, false);
        }
        DrawHierarchyRootContextMenu();
        if (!m_playing && ImGui::BeginDragDropTarget())
        {
            // 受け取った移動・素材データ
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(GameObjectPayload))
            {
                // 移動するオブジェクトID
                GameObjectId draggedId{};
                std::memcpy(&draggedId, payload->Data, sizeof(draggedId));
                // 移動するオブジェクト
                if (auto* dragged = m_scene.FindGameObject(draggedId);
                    dragged != nullptr && dragged->Parent() != nullptr)
                {
                    m_pendingHierarchyParentChange = {
                        dragged->Id(),
                        {},
                        true
                    };
                }
            }
            // 受け取った移動・素材データ
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload(AssetPayload))
            {
                // 割り当てる相対アセットパス
                const auto asset = PathFromUtf8(
                    static_cast<const char*>(payload->Data));
                if (!IsCppScriptAsset(asset)
                    && !IsSceneAsset(asset)
                    && !IsPrefabAsset(asset))
                {
                    // 素材を割り当てる新規対象
                    auto& created = m_scene.CreateGameObject(
                        PathToUtf8(asset.stem()));
                    if (!ApplyDroppedAsset(created, asset))
                    {
                        static_cast<void>(
                            m_scene.DestroyGameObject(
                                created));
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (hierarchyFilter.empty())
        {
            // 表示・検索する対象
            for (const auto& gameObject :
                m_scene.GameObjects())
            {
                if (gameObject->Parent() == nullptr
                    && gameObject->SourceScene()
                        == Scene::PrimarySceneHandle())
                {
                    DrawHierarchyNode(*gameObject);
                }
            }

            DrawAdditiveSceneNodes();
        }
        else
        {
            // 検索用に文字列を小文字化します(value: 変換する文字列)。
            const auto lowered =
                [](std::string value)
                {
                    // 1byteずつ小文字へ変換します(character: 変換する文字)。
                    std::ranges::transform(
                        value,
                        value.begin(),
                        [](const unsigned char character)
                        {
                            return static_cast<char>(
                                std::tolower(character));
                        });
                    return value;
                };
            // 小文字化した検索文字列
            const auto needle = lowered(hierarchyFilter);
            // 検索に一致する対象がある
            bool matched = false;
            // 表示・検索する対象
            for (const auto& gameObject :
                m_scene.GameObjects())
            {
                if (lowered(gameObject->Name()).find(needle)
                    == std::string::npos)
                {
                    continue;
                }
                matched = true;
                ImGui::PushID(
                    static_cast<int>(gameObject->Id()));
                if (ImGui::Selectable(
                    gameObject->Name().c_str(),
                    IsObjectSelected(gameObject->Id())))
                {
                    SelectObject(
                        gameObject->Id(),
                        ImGui::GetIO().KeyCtrl);
                }
                if (ImGui::IsItemHovered()
                    && ImGui::IsMouseDoubleClicked(
                        ImGuiMouseButton_Left))
                {
                    SelectObject(gameObject->Id(), false);
                    FocusSelection();
                }
                ImGui::PopID();
            }
            if (!matched)
            {
                ImGui::TextDisabled(
                    "一致するGameObjectがありません");
            }
        }

        if (ImGui::IsWindowHovered()
            && ImGui::IsMouseClicked(
                ImGuiMouseButton_Right)
            && !ImGui::IsAnyItemHovered())
        {
            m_selectedObjectId = 0;
        }
        if (ImGui::BeginPopupContextWindow(
            "##HierarchyBackgroundContext",
            ImGuiPopupFlags_MouseButtonRight
                | ImGuiPopupFlags_NoOpenOverItems))
        {
            ImGui::BeginDisabled(m_playing);
            if (ImGui::MenuItem("空のルートを作成"))
            {
                m_selectedObjectId = 0;
                m_hierarchyContextAction =
                    HierarchyContextAction::CreateRoot;
            }
            if (ImGui::MenuItem(
                "貼り付け",
                "Ctrl+V",
                false,
                !m_clipboardSceneJson.empty()))
            {
                m_selectedObjectId = 0;
                m_hierarchyContextAction =
                    HierarchyContextAction::Paste;
            }
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }

        if (ImGui::IsWindowHovered()
            && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && !ImGui::IsAnyItemHovered())
        {
            m_selectedObjectId = 0;
        }

        ExecuteHierarchyContextAction();
        ExecutePendingHierarchyParentChange();
        ExecutePendingHierarchyReorder();
        ImGui::End();
    }

    // 親変更の予約を消費し、存在する対象の親を変更して履歴へ記録します。
    void EditorLayer::ExecutePendingHierarchyParentChange()
    {
        if (!m_pendingHierarchyParentChange.requested)
        {
            return;
        }
        // 消費する親変更の予約
        const auto request = m_pendingHierarchyParentChange;
        m_pendingHierarchyParentChange = {};

        // 親を変更する対象
        auto* const moved =
            m_scene.FindGameObject(request.moved);
        if (moved == nullptr)
        {
            return;
        }

        // 移動先の親・nullptrはルート
        GameObject* parent = nullptr;
        if (request.parent != 0)
        {
            parent = m_scene.FindGameObject(request.parent);
            if (parent == nullptr)
            {
                SetStatus("親GameObjectが見つかりません", true);
                return;
            }
        }
        if (moved->Parent() == parent)
        {
            return;
        }

        try
        {
            moved->SetParent(parent);
            RecordHistory();
            SetStatus(
                parent != nullptr
                    ? "親子関係を変更しました"
                    : "シーンルートへ移動しました");
        }
        // 階層変更を中断した失敗原因
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    // 並び替え予約を消費し、必要なら基準の親へ移してから挿入位置を変更します。
    void EditorLayer::ExecutePendingHierarchyReorder()
    {
        if (!m_pendingHierarchyReorder.requested)
        {
            return;
        }
        // 消費する並び替えの予約
        const auto request = m_pendingHierarchyReorder;
        m_pendingHierarchyReorder = {};

        // 並び替える対象
        auto* const moved =
            m_scene.FindGameObject(request.moved);
        // 挿入位置の基準対象
        auto* const reference =
            m_scene.FindGameObject(request.reference);
        if (moved == nullptr || reference == nullptr)
        {
            return;
        }

        try
        {
            if (request.reparentToReferenceLevel)
            {
                // 基準の親へ移した後に並び替えが失敗しても、親変更は元へ戻りません。
                moved->SetParent(reference->Parent());
            }
            if (m_scene.ReorderGameObject(
                    *moved,
                    *reference,
                    request.insertAfter))
            {
                RecordHistory();
                SetStatus("並び順を変更しました");
            }
            else
            {
                SetStatus(
                    "そこへは並び替えできません",
                    true);
            }
        }
        // 階層変更を中断した失敗原因
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    // 追加シーンごとの階層と破棄操作を描画し、走査後に破棄要求を実行します。
    void EditorLayer::DrawAdditiveSceneNodes()
    {
        if (m_scene.AdditiveScenes().empty())
        {
            return;
        }

        // 走査後に破棄する追加シーン
        SceneHandle unloadRequest =
            Scene::PrimarySceneHandle();
        // 表示する追加シーン
        for (const auto& additiveScene :
            m_scene.AdditiveScenes())
        {
            ImGui::PushID(
                static_cast<int>(
                    additiveScene.handle));
            // 追加シーンの表示名
            const std::string label =
                "[追加] " + additiveScene.name;
            // 追加シーンのツリーが開いている
            const bool open = ImGui::TreeNodeEx(
                label.c_str(),
                ImGuiTreeNodeFlags_DefaultOpen
                | ImGuiTreeNodeFlags_SpanAvailWidth);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "%s",
                    PathToUtf8(
                        additiveScene.path).c_str());
            }
            if (ImGui::BeginPopupContextItem(
                "##AdditiveSceneContext"))
            {
                if (ImGui::MenuItem(
                    "この追加シーンを破棄",
                    nullptr,
                    false,
                    !m_playing))
                {
                    unloadRequest =
                        additiveScene.handle;
                }
                ImGui::EndPopup();
            }
            if (open)
            {
                // 追加シーンのルート候補
                for (const auto& gameObject :
                    m_scene.GameObjects())
                {
                    if (gameObject->Parent() == nullptr
                        && gameObject->SourceScene()
                            == additiveScene.handle)
                    {
                        DrawHierarchyNode(*gameObject);
                    }
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }

        // 走査中の破棄は避け、追加シーンは主シーンのUndo履歴へ記録しません。
        if (unloadRequest
            != Scene::PrimarySceneHandle())
        {
            // 追加シーンはUndo履歴（主シーンのスナップショット）に含まれないため、履歴は記録しません。
            if (m_scene.UnloadScene(unloadRequest))
            {
                if (m_selectedObjectId != 0
                    && m_scene.FindGameObject(
                        m_selectedObjectId) == nullptr)
                {
                    m_selectedObjectId = 0;
                }
                SetStatus("追加シーンを破棄しました");
            }
        }
    }

    // 対象行の選択・操作・移動予約を描画し、開いた子ノードへ再帰します(gameObject: 描画する対象)。
    void EditorLayer::DrawHierarchyNode(GameObject& gameObject)
    {
        // 階層ノードの表示フラグ
        ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_OpenOnArrow
            | ImGuiTreeNodeFlags_SpanAvailWidth;

        // 対象に子オブジェクトがある
        const bool hasChildren = !gameObject.Children().empty();
        if (!hasChildren)
        {
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }
        if (IsObjectSelected(gameObject.Id()))
        {
            flags |= ImGuiTreeNodeFlags_Selected;
        }

        // 対象ID由来の階層ノードID
        const auto nodeId = reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(gameObject.Id()));
        // 対象ノードが開いている
        const bool open = ImGui::TreeNodeEx(
            nodeId,
            flags,
            "%s%s%s",
            gameObject.IsPersistent()
                ? "[維持] "
                : "",
            gameObject.IsPrefabInstanceRoot()
                ? "[Prefab] "
                : "",
            gameObject.Name().c_str());

        // 対象行の矩形左上
        // Popup描画で直前の項目が変わる前に、ドロップ判定用の行矩形を記録します。
        const ImVec2 nodeRectMinimum = ImGui::GetItemRectMin();
        // 対象行の矩形右下
        const ImVec2 nodeRectMaximum = ImGui::GetItemRectMax();

        if (ImGui::IsItemClicked())
        {

            SelectObject(
                gameObject.Id(),
                ImGui::GetIO().KeyCtrl);
        }
        if (ImGui::IsItemHovered()
            && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {

            SelectObject(gameObject.Id(), false);
            FocusSelection();
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            // 右クリックした選択済み対象は、複数選択を保って操作します。
            if (!IsObjectSelected(gameObject.Id()))
            {
                SelectObject(gameObject.Id(), false);
            }
            else
            {
                m_selectedObjectId = gameObject.Id();
                m_selectedAsset.clear();
            }
        }

        if (ImGui::BeginPopupContextItem())
        {
            ImGui::BeginDisabled(m_playing);
            if (ImGui::MenuItem("空の子を作成"))
            {
                m_hierarchyContextAction =
                    HierarchyContextAction::CreateChild;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("切り取り", "Ctrl+X"))
            {
                m_hierarchyContextAction =
                    HierarchyContextAction::Cut;
            }
            if (ImGui::MenuItem("コピー", "Ctrl+C"))
            {
                m_hierarchyContextAction =
                    HierarchyContextAction::Copy;
            }
            if (ImGui::MenuItem(
                "貼り付け",
                "Ctrl+V",
                false,
                !m_clipboardSceneJson.empty()))
            {
                m_hierarchyContextAction =
                    HierarchyContextAction::Paste;
            }
            if (ImGui::MenuItem("複製", "Ctrl+D"))
            {
                m_hierarchyContextAction =
                    HierarchyContextAction::Duplicate;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(
                "Prefabとして保存..."))
            {
                m_hierarchyContextAction =
                    HierarchyContextAction::SaveAsPrefab;
            }
            if (ImGui::MenuItem("削除", "Delete"))
            {
                m_hierarchyContextAction =
                    HierarchyContextAction::Delete;
            }
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }

        if (!m_playing && ImGui::BeginDragDropSource())
        {
            // ドラッグする対象のID
            const GameObjectId id = gameObject.Id();
            ImGui::SetDragDropPayload(GameObjectPayload, &id, sizeof(id));
            ImGui::TextUnformatted(gameObject.Name().c_str());
            ImGui::EndDragDropSource();
        }

        if (!m_playing && ImGui::BeginDragDropTarget())
        {
            // ドロップ判定用の矩形左上
            const ImVec2 itemMinimum = nodeRectMinimum;
            // ドロップ判定用の矩形右下
            const ImVec2 itemMaximum = nodeRectMaximum;
            // 対象行の高さ
            const float itemHeight = std::max(
                itemMaximum.y - itemMinimum.y,
                1.0f);
            // 行内のドロップ位置・0～1
            const float positionRatio = std::clamp(
                (ImGui::GetMousePos().y - itemMinimum.y)
                    / itemHeight,
                0.0f,
                1.0f);
            // 並び替え帯の上下端割合
            constexpr float reorderEdgeRatio = 0.3f;
            // insertBefore: 対象rowの前へ挿入する位置か。
            const bool insertBefore =
                positionRatio < reorderEdgeRatio;
            // 対象行の後へ挿入する
            const bool insertAfter =
                positionRatio > 1.0f - reorderEdgeRatio;
            // 子へ移動せず並び替える
            const bool reordering = insertBefore || insertAfter;

            // 移動データ・既定の枠を抑制
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload(
                        GameObjectPayload,
                        ImGuiDragDropFlags_AcceptBeforeDelivery
                            | ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
            {
                // ドロップ位置の描画先
                auto* const drawList =
                    ImGui::GetWindowDrawList();
                // ドロップ位置の表示色
                const auto highlight = ImGui::GetColorU32(
                    ImGuiCol_DragDropTarget);
                if (reordering)
                {
                    // 並び替え挿入線のY座標
                    const float lineY = insertBefore
                        ? itemMinimum.y
                        : itemMaximum.y;
                    drawList->AddLine(
                        ImVec2{ itemMinimum.x, lineY },
                        ImVec2{ itemMaximum.x, lineY },
                        highlight,
                        3.0f);
                    drawList->AddCircleFilled(
                        ImVec2{ itemMinimum.x + 3.0f, lineY },
                        4.0f,
                        highlight);
                }
                else
                {

                    drawList->AddRect(
                        itemMinimum,
                        itemMaximum,
                        highlight,
                        2.0f,
                        0,
                        2.0f);
                }

                if (payload->IsDelivery())
                {
                    // 移動するオブジェクトID
                    GameObjectId draggedId{};
                    std::memcpy(&draggedId, payload->Data, sizeof(draggedId));
                    // 移動するオブジェクト
                    auto* dragged = m_scene.FindGameObject(draggedId);

                    if (dragged != nullptr
                        && dragged != &gameObject)
                    {
                        try
                        {
                            if (reordering)
                            {
                                // GameObjectsの反復を壊さないよう、並び替えは予約して走査後に適用します。
                                m_pendingHierarchyReorder = {
                                    dragged->Id(),
                                    gameObject.Id(),
                                    insertAfter,
                                    dragged->Parent()
                                        != gameObject.Parent(),
                                    true
                                };
                            }
                            else if (dragged->Parent()
                                != &gameObject)
                            {
                                // 子の反復とTreeNodeの対応を保つため、親変更は予約して走査後に適用します。
                                m_pendingHierarchyParentChange = {
                                    dragged->Id(),
                                    gameObject.Id(),
                                    true
                                };
                            }
                        }
                        // 移動予約を中断した失敗原因
                        catch (const std::exception& exception)
                        {
                            SetStatus(exception.what(), true);
                        }
                    }
                }
            }
            // 受け取った移動・素材データ
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload(AssetPayload))
            {
                // 割り当てる相対アセットパス
                const auto asset = PathFromUtf8(
                    static_cast<const char*>(payload->Data));
                if (IsCppScriptAsset(asset))
                {
                    QueueCppScriptAttachment(
                        gameObject,
                        asset);
                }
                else
                {
                    static_cast<void>(
                        ApplyDroppedAsset(
                            gameObject,
                            asset));
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (open && hasChildren)
        {
            // 描画する子オブジェクト
            for (auto* child : gameObject.Children())
            {
                DrawHierarchyNode(*child);
            }
            ImGui::TreePop();
        }
    }

    // シーンルートの操作メニューを描画し、作成と貼り付けを予約します。
    void EditorLayer::DrawHierarchyRootContextMenu()
    {
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            m_selectedObjectId = 0;
        }
        if (!ImGui::BeginPopupContextItem(
            "##HierarchyRootContext"))
        {
            return;
        }

        ImGui::BeginDisabled(m_playing);
        if (ImGui::MenuItem("空のルートを作成"))
        {
            m_hierarchyContextAction =
                HierarchyContextAction::CreateRoot;
        }
        if (ImGui::BeginMenu("UI"))
        {
            if (ImGui::MenuItem("Canvas"))
            {
                m_hierarchyContextAction =
                    HierarchyContextAction::CreateUICanvas;
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(
            "貼り付け",
            "Ctrl+V",
            false,
            !m_clipboardSceneJson.empty()))
        {
            m_hierarchyContextAction =
                HierarchyContextAction::Paste;
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }

    // 描画中に予約した階層操作を実行し、予約を解除します。
    void EditorLayer::ExecuteHierarchyContextAction()
    {
        switch (m_hierarchyContextAction)
        {
        case HierarchyContextAction::CreateRoot:
            CreateRootGameObject();
            break;
        case HierarchyContextAction::CreateChild:
            CreateChildGameObject();
            break;
        case HierarchyContextAction::CreateUICanvas:
            CreateUICanvasGameObject();
            break;
        case HierarchyContextAction::Cut:
            CutSelectedGameObject();
            break;
        case HierarchyContextAction::Copy:
            CopySelectedGameObject();
            break;
        case HierarchyContextAction::Paste:
            PasteGameObject();
            break;
        case HierarchyContextAction::Duplicate:
            DuplicateSelectedGameObject();
            break;
        case HierarchyContextAction::SaveAsPrefab:
            SaveSelectedAsPrefab();
            break;
        case HierarchyContextAction::Delete:
            DeleteSelectedGameObject();
            break;
        case HierarchyContextAction::None:
        default:
            break;
        }
        m_hierarchyContextAction =
            HierarchyContextAction::None;
    }

    // 空のルート対象を作成して選択し、履歴へ記録します。
    void EditorLayer::CreateRootGameObject()
    {
        // 作成したルート対象
        auto& gameObject = m_scene.CreateGameObject("GameObject");
        m_selectedObjectId = gameObject.Id();
        m_selectedAsset.clear();
        RecordHistory();
        SetStatus("ルートGameObjectを作成しました");
    }

    // 選択対象の空の子を作成して選択し、履歴へ記録します。
    void EditorLayer::CreateChildGameObject()
    {
        // 選択中の親対象
        auto* parent = m_scene.FindGameObject(m_selectedObjectId);
        if (parent == nullptr)
        {
            return;
        }

        // 作成した子対象
        auto& gameObject = m_scene.CreateGameObject("GameObject");
        gameObject.SetParent(parent);
        m_selectedObjectId = gameObject.Id();
        m_selectedAsset.clear();
        RecordHistory();
        SetStatus("子GameObjectを作成しました");
    }

    // 編集中に種別に応じた対象とコンポーネントを作成します(kind: 作成する組込種別)。
    void EditorLayer::CreateBuiltInGameObject(
        const BuiltInGameObjectKind kind)
    {
        if (m_playing)
        {
            return;
        }

        // 組込種別に対応する対象名
        const char* name = "GameObject";
        switch (kind)
        {
        case BuiltInGameObjectKind::Camera:
            name = "カメラ";
            break;
        case BuiltInGameObjectKind::Sprite:
            name = "スプライト";
            break;
        case BuiltInGameObjectKind::Cube:
            name = "立方体";
            break;
        case BuiltInGameObjectKind::Sphere:
            name = "球";
            break;
        case BuiltInGameObjectKind::Cylinder:
            name = "円柱";
            break;
        case BuiltInGameObjectKind::Plane:
            name = "平面";
            break;
        case BuiltInGameObjectKind::DirectionalLight:
            name = "平行光源";
            break;
        case BuiltInGameObjectKind::PointLight:
            name = "ポイントライト";
            break;
        case BuiltInGameObjectKind::SpotLight:
            name = "スポットライト";
            break;
        case BuiltInGameObjectKind::Light2D:
            name = "2Dライト";
            break;
        case BuiltInGameObjectKind::AudioSource:
            name = "オーディオソース";
            break;
        case BuiltInGameObjectKind::UICanvas:
            name = "Canvas";
            break;
        }

        // 作成した組込対象
        auto& gameObject = m_scene.CreateGameObject(name);
        switch (kind)
        {
        case BuiltInGameObjectKind::Camera:
        {
            // 追加したカメラ
            auto& camera =
                gameObject.AddComponent<CameraComponent>();
            if (m_scene.MainCamera() == nullptr)
            {
                m_scene.SetMainCamera(camera);
            }
            break;
        }
        case BuiltInGameObjectKind::Sprite:
            gameObject.AddComponent<SpriteRendererComponent>();
            break;
        case BuiltInGameObjectKind::Cube:
            gameObject.AddComponent<MeshRendererComponent>(
                PrimitiveShape::Cube);
            break;
        case BuiltInGameObjectKind::Sphere:
            gameObject.AddComponent<MeshRendererComponent>(
                PrimitiveShape::Sphere);
            break;
        case BuiltInGameObjectKind::Cylinder:
            gameObject.AddComponent<MeshRendererComponent>(
                PrimitiveShape::Cylinder);
            break;
        case BuiltInGameObjectKind::Plane:
            gameObject.AddComponent<MeshRendererComponent>(
                PrimitiveShape::Plane);
            break;
        case BuiltInGameObjectKind::DirectionalLight:
            gameObject.GetTransform().SetEulerAngles({
                DirectX::XMConvertToRadians(-45.0f),
                DirectX::XMConvertToRadians(-35.0f),
                0.0f
            });
            gameObject.AddComponent<
                DirectionalLightComponent>();
            break;
        case BuiltInGameObjectKind::PointLight:
            gameObject.AddComponent<PointLightComponent>();
            break;
        case BuiltInGameObjectKind::SpotLight:
            gameObject.AddComponent<SpotLightComponent>();
            break;
        case BuiltInGameObjectKind::Light2D:
            gameObject.AddComponent<Light2DComponent>();
            break;
        case BuiltInGameObjectKind::AudioSource:
            gameObject.AddComponent<AudioSourceComponent>();
            break;
        case BuiltInGameObjectKind::UICanvas:
            gameObject.AddComponent<UICanvasComponent>();
            break;
        }

        m_selectedObjectId = gameObject.Id();
        m_selectedAsset.clear();
        RecordHistory();
        SetStatus(std::string{ name } + "を作成しました");
    }

    // Canvasコンポーネントを持つ対象を作成します。
    void EditorLayer::CreateUICanvasGameObject()
    {
        CreateBuiltInGameObject(
            BuiltInGameObjectKind::UICanvas);
    }

    // 選択対象をIDで再確認しながら階層ごと削除し、選択解除と履歴記録を行います。
    void EditorLayer::DeleteSelectedGameObject()
    {
        // 削除前の選択対象一覧
        const auto selection = SelectedObjects();
        if (selection.empty())
        {
            return;
        }

        // 削除時に引き直す対象ID一覧
        std::vector<GameObjectId> ids;
        ids.reserve(selection.size());
        // 削除候補の対象
        for (const auto* object : selection)
        {
            ids.push_back(object->Id());
        }
        // 削除を実行した階層数
        std::size_t deleted = 0;
        // 削除候補の対象ID
        // 親の削除で子も消えるため、保持したIDから生存する対象だけを引き直します。
        for (const auto id : ids)
        {
            // 削除候補の対象
            if (auto* object = m_scene.FindGameObject(id))
            {
                m_scene.DestroyGameObject(*object);
                ++deleted;
            }
        }

        m_selectedObjectId = 0;
        ClearMultiSelection();
        RecordHistory();
        SetStatus(
            deleted > 1
                ? std::to_string(deleted)
                    + "個のGameObject階層を削除しました"
                : std::string{
                    "GameObject階層を削除しました" });
    }

    // 編集中の選択階層を同じ親へ複製し、複製した対象を選択して履歴へ記録します。
    void EditorLayer::DuplicateSelectedGameObject()
    {
        // 複製する選択対象一覧
        const auto selection = SelectedObjects();
        if (selection.empty() || m_playing)
        {
            return;
        }

        try
        {
            // 複製後に選択する対象ID一覧
            // 複製の途中で失敗しても、作成済みの複製は元へ戻りません。
            std::vector<GameObjectId> duplicates;
            duplicates.reserve(selection.size());
            // 複製元の対象
            for (auto* object : selection)
            {
                // 新たに複製した対象
                auto& duplicate =
                    m_scene.DuplicateGameObject(
                        *object,
                        object->Parent());
                duplicates.push_back(duplicate.Id());
            }

            ClearMultiSelection();
            m_selectedObjectId = duplicates.front();
            m_additionalSelection.assign(
                duplicates.begin() + 1,
                duplicates.end());
            RecordHistory();
            SetStatus(
                duplicates.size() > 1
                    ? std::to_string(duplicates.size())
                        + "個のGameObject階層を複製しました"
                    : std::string{
                        "GameObject階層を複製しました" });
        }
        // 作成・コピー・再読込の失敗原因
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    // シーン全体のJSONと主選択対象のIDを、貼り付け用に保持します。
    void EditorLayer::CopySelectedGameObject()
    {
        // コピー元の主選択対象
        const auto* selected = m_scene.FindGameObject(m_selectedObjectId);
        if (selected == nullptr)
        {
            return;
        }

        try
        {
            m_clipboardSceneJson = m_scene.SerializeToJson();
            m_clipboardObjectId = selected->Id();
            SetStatus("GameObject階層をコピーしました");
        }
        // 作成・コピー・再読込の失敗原因
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    // 編集中にシーンJSONと主選択対象IDを保持し、その階層を削除して履歴へ記録します。
    void EditorLayer::CutSelectedGameObject()
    {
        // 切り取る主選択対象
        auto* selected = m_scene.FindGameObject(m_selectedObjectId);
        if (selected == nullptr || m_playing)
        {
            return;
        }

        try
        {
            m_clipboardSceneJson = m_scene.SerializeToJson();
            m_clipboardObjectId = selected->Id();
            m_scene.DestroyGameObject(*selected);
            m_selectedObjectId = 0;
            RecordHistory();
            SetStatus("GameObject階層を切り取りました");
        }
        // 作成・コピー・再読込の失敗原因
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    // コピー時のシーンJSONから対象を復元し、現在の選択対象と同じ親へ複製します。
    void EditorLayer::PasteGameObject()
    {
        if (m_clipboardSceneJson.empty() || m_playing)
        {
            return;
        }

        try
        {
            // コピー時のシーン復元先
            Scene clipboardScene(m_graphics);
            clipboardScene.LoadFromJson(m_clipboardSceneJson);
            // コピー時の主選択対象
            const auto* clipboardObject =
                clipboardScene.FindGameObject(m_clipboardObjectId);
            if (clipboardObject == nullptr)
            {
                throw std::runtime_error("The copied GameObject is no longer available.");
            }

            // 現在の主選択対象
            auto* currentSelection = m_scene.FindGameObject(m_selectedObjectId);
            // 貼付先の親・nullptrはルート
            auto* targetParent = currentSelection != nullptr
                ? currentSelection->Parent()
                : nullptr;
            // 新たに貼り付けた対象
            auto& pasted = m_scene.DuplicateGameObject(
                *clipboardObject,
                targetParent);
            m_selectedObjectId = pasted.Id();
            RecordHistory();
            SetStatus("GameObject階層を貼り付けました");
        }
        // 作成・コピー・再読込の失敗原因
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    // 編集中かつビルド停止中に、選択アセットを再インポートします。
    void EditorLayer::ReimportSelectedAsset()
    {
        if (m_selectedAsset.empty()
            || m_playing
            || m_gameModuleBuildProcess != nullptr)
        {
            return;
        }

        ReimportAssets(m_selectedAsset);
    }

    // 編集中かつビルド停止中に、全アセットを再インポートします。
    void EditorLayer::ReimportAllAssets()
    {
        if (m_playing
            || m_gameModuleBuildProcess != nullptr)
        {
            return;
        }

        ReimportAssets(std::nullopt);
    }

    // キャッシュを無効化し、使用中の該当参照を再読込します(asset: 対象相対パス・nulloptは全件)。
    void EditorLayer::ReimportAssets(
        const std::optional<std::filesystem::path>& asset)
    {
        // 全アセットを再インポートする
        const bool allAssets = !asset.has_value();
        // 空でない対象参照を選びます(reference: 使用中の参照パス)。
        const auto matches =
            [&asset, allAssets](
                const std::filesystem::path& reference)
            {
                return !reference.empty()
                    && (allAssets
                        || IsSameAssetReference(
                            reference,
                            *asset));
            };

        try
        {
            static_cast<void>(
                m_graphics.Assets().Database().Refresh(
                    true));
            if (allAssets)
            {
                m_graphics.Assets().Clear();
                m_graphics.Audio().Clear();
            }
            else
            {
                m_graphics.Assets().Invalidate(*asset);
                if (IsAudioAsset(*asset))
                {
                    m_graphics.Audio().Clear();
                }
            }

            // 参照を再読込する対象
            // 再読込は順に適用するため、途中で失敗しても先に更新した参照は元へ戻りません。
            for (const auto& gameObject :
                m_scene.GameObjects())
            {
                // 画像・Shaderの再読込対象
                if (auto* sprite =
                        gameObject->GetComponent<
                            SpriteRendererComponent>();
                    sprite != nullptr
                    && matches(sprite->TexturePath()))
                {
                    sprite->SetTexturePath(
                        sprite->TexturePath());
                }
                // 画像・Shaderの再読込対象
                if (auto* sprite =
                        gameObject->GetComponent<
                            SpriteRendererComponent>();
                    sprite != nullptr
                    && matches(sprite->ShaderPath()))
                {
                    sprite->ReloadShader();
                }
                // 音声の再読込対象
                if (auto* audio =
                        gameObject->GetComponent<
                            AudioSourceComponent>();
                    audio != nullptr
                    && matches(audio->AudioPath()))
                {
                    audio->SetAudioPath(
                        audio->AudioPath());
                }
                // アニメーションの再読込対象
                if (auto* animator =
                        gameObject->GetComponent<
                            TransformAnimatorComponent>();
                    animator != nullptr)
                {
                    if (matches(
                            animator->ControllerPath()))
                    {
                        animator->ReloadController();
                    }
                    if (matches(animator->ClipPath()))
                    {
                        animator->ReloadClip();
                    }
                }
                // モデル描画素材の再読込対象
                if (auto* model =
                        gameObject->GetComponent<
                            ModelRendererComponent>();
                    model != nullptr)
                {
                    if (matches(
                            model->AnimationControllerPath()))
                    {
                        model->ReloadAnimationController();
                    }
                    if (matches(model->ModelPath()))
                    {
                        model->SetModelPath(
                            model->ModelPath());
                    }
                    if (matches(
                            model->MaterialAssetPath()))
                    {
                        model->ReloadMaterialAsset();
                    }
                    if (matches(
                            model->AlbedoTexturePath()))
                    {
                        model->SetAlbedoTexturePath(
                            model->AlbedoTexturePath());
                    }
                    if (matches(
                            model->NormalTexturePath()))
                    {
                        model->SetNormalTexturePath(
                            model->NormalTexturePath());
                    }
                    if (matches(
                            model->RoughnessTexturePath()))
                    {
                        model->SetRoughnessTexturePath(
                            model->RoughnessTexturePath());
                    }
                    if (matches(
                            model->MetallicTexturePath()))
                    {
                        model->SetMetallicTexturePath(
                            model->MetallicTexturePath());
                    }
                    if (matches(
                            model->OcclusionTexturePath()))
                    {
                        model->SetOcclusionTexturePath(
                            model->OcclusionTexturePath());
                    }
                    if (matches(
                            model->EmissiveTexturePath()))
                    {
                        model->SetEmissiveTexturePath(
                            model->EmissiveTexturePath());
                    }
                    if (matches(model->ShaderPath()))
                    {
                        model->ReloadShader();
                    }
                }
                // メッシュ描画素材の再読込対象
                if (auto* mesh =
                        gameObject->GetComponent<
                            MeshRendererComponent>();
                    mesh != nullptr)
                {
                    if (matches(
                            mesh->MaterialAssetPath()))
                    {
                        mesh->ReloadMaterialAsset();
                    }
                    if (matches(
                            mesh->AlbedoTexturePath()))
                    {
                        mesh->SetAlbedoTexturePath(
                            mesh->AlbedoTexturePath());
                    }
                    if (matches(
                            mesh->NormalTexturePath()))
                    {
                        mesh->SetNormalTexturePath(
                            mesh->NormalTexturePath());
                    }
                    if (matches(
                            mesh->RoughnessTexturePath()))
                    {
                        mesh->SetRoughnessTexturePath(
                            mesh->RoughnessTexturePath());
                    }
                    if (matches(
                            mesh->MetallicTexturePath()))
                    {
                        mesh->SetMetallicTexturePath(
                            mesh->MetallicTexturePath());
                    }
                    if (matches(
                            mesh->OcclusionTexturePath()))
                    {
                        mesh->SetOcclusionTexturePath(
                            mesh->OcclusionTexturePath());
                    }
                    if (matches(
                            mesh->EmissiveTexturePath()))
                    {
                        mesh->SetEmissiveTexturePath(
                            mesh->EmissiveTexturePath());
                    }
                    if (matches(mesh->ShaderPath()))
                    {
                        mesh->ReloadShader();
                    }
                }
                // 粒子画像・Shaderの再読込対象
                if (auto* particles =
                        gameObject->GetComponent<
                            ParticleSystemComponent>();
                    particles != nullptr)
                {
                    if (matches(particles->TexturePath()))
                    {
                        particles->SetTexturePath(
                            particles->TexturePath());
                    }
                    if (matches(
                            particles->AuxiliaryTexturePath()))
                    {
                        particles->SetAuxiliaryTexturePath(
                            particles->AuxiliaryTexturePath());
                    }
                    if (matches(particles->ShaderPath()))
                    {
                        particles->ReloadShader();
                    }
                }
                // 2D粒子画像の再読込対象
                if (auto* particles2D =
                        gameObject->GetComponent<
                            SpriteParticles2DComponent>();
                    particles2D != nullptr
                    && matches(particles2D->TexturePath()))
                {
                    particles2D->SetTexturePath(
                        particles2D->TexturePath());
                }
                // タイル画像の再読込対象
                if (auto* tilemap =
                        gameObject->GetComponent<
                            TilemapComponent>();
                    tilemap != nullptr
                    && matches(tilemap->TexturePath()))
                {
                    tilemap->SetTexturePath(
                        tilemap->TexturePath());
                }
                // ボタン画像の再読込対象
                if (auto* button =
                        gameObject->GetComponent<
                            UIButtonComponent>();
                    button != nullptr
                    && matches(button->TexturePath()))
                {
                    button->SetTexturePath(
                        button->TexturePath());
                }
                // UI画像の再読込対象
                if (auto* image =
                        gameObject->GetComponent<
                            UIImageComponent>();
                    image != nullptr
                    && matches(image->TexturePath()))
                {
                    image->SetTexturePath(
                        image->TexturePath());
                }
                // 衝突モデルの再読込対象
                if (auto* collider =
                        gameObject->GetComponent<
                            MeshCollider3DComponent>();
                    collider != nullptr
                    && matches(collider->ModelPath()))
                {
                    collider->SetModelPath(
                        collider->ModelPath());
                }
            }
            RefreshAssets(true);
            if (allAssets)
            {
                SetStatus(
                    "すべてのアセットを再インポートしました（"
                    + std::to_string(
                        m_graphics.Assets().Database().
                            Assets().size())
                    + "件）");
            }
            else
            {
                SetStatus(
                    "再インポートしました: "
                    + PathToUtf8(*asset));
            }
        }
        // 作成・コピー・再読込の失敗原因
        catch (const std::exception& exception)
        {
            SetStatus(
                std::string{
                    allAssets
                        ? "すべての再インポートに失敗しました: "
                        : "再インポートに失敗しました: "
                }
                    + exception.what(),
                true);
        }
    }

    // DBのファイル・実フォルダー・データ型一覧を更新して無効な選択を解除します(reuseExistingDatabase: 走査済みDBを再利用するか)。
    void EditorLayer::RefreshAssets(
        const bool reuseExistingDatabase)
    {
        m_assetFiles.clear();
        m_assetDirectories.clear();

        // アセットのルートパス
        const auto& assetRoot = m_graphics.Assets().AssetRoot();
        if (!std::filesystem::exists(assetRoot))
        {
            m_assetDirectory.clear();
            m_selectedAsset.clear();
            return;
        }

        // 一覧の取得元アセットDB
        auto& database = m_graphics.Assets().Database();
        if (!reuseExistingDatabase || !database.HasRefreshed())
        {
            static_cast<void>(database.Refresh(true));
        }
        m_assetFiles.reserve(
            database.Assets().size());
        m_dataAssetTypeByPath.clear();
        // 一覧へ追加するDB登録情報
        for (const auto& asset :
            m_graphics.Assets().Database().Assets())
        {
            m_assetFiles.push_back(asset.path);
            // 参照欄の絞込と一覧表示のため、データアセット本文のtypeをキャッシュします。
            if (IsDataAsset(asset.path))
            {
                // データアセットの型名
                std::string typeName;
                try
                {
                    // 型を調べるアセット本文
                    const auto bytes =
                        m_graphics.Assets().ReadFileBytes(
                            asset.path);
                    if (!bytes.empty())
                    {
                        typeName = DataAsset::FromJson(
                            std::string_view(
                                reinterpret_cast<const char*>(
                                    bytes.data()),
                                bytes.size()))
                            .TypeName();
                    }
                }
                catch (const std::exception&)
                {
                    // 型の取得失敗は空の型名として扱い、一覧の更新を続けます。
                }
                m_dataAssetTypeByPath.insert_or_assign(
                    Lowercase(PathToUtf8(asset.path)),
                    std::move(typeName));
            }
        }

        // フォルダー走査のエラー
        std::error_code error;
        // 権限拒否を飛ばす走査設定
        const auto options =
            std::filesystem::directory_options::skip_permission_denied;

        // アセットフォルダーの再帰走査位置
        for (std::filesystem::recursive_directory_iterator iterator{
                assetRoot,
                options,
                error
            };
            iterator != std::filesystem::recursive_directory_iterator{};
            iterator.increment(error))
        {
            if (error)
            {
                error.clear();
                continue;
            }

            if (iterator->is_directory(error) && !error)
            {
                // 検出フォルダーの相対パス
                const auto directory =
                    iterator->path().lexically_relative(assetRoot);
                if (!directory.empty())
                {
                    m_assetDirectories.push_back(directory);
                }
            }
            error.clear();
        }

        std::ranges::sort(m_assetFiles);
        std::ranges::sort(m_assetDirectories);
        // 重複を除いた後の不要な末尾
        const auto uniqueDirectories = std::ranges::unique(m_assetDirectories);
        m_assetDirectories.erase(
            uniqueDirectories.begin(),
            uniqueDirectories.end());

        if (!m_selectedAsset.empty()
            && std::ranges::find(m_assetFiles, m_selectedAsset) == m_assetFiles.end())
        {
            m_selectedAsset.clear();
        }
        if (!m_assetDirectory.empty()
            && std::ranges::find(
                m_assetDirectories,
                m_assetDirectory) == m_assetDirectories.end())
        {
            m_assetDirectory.clear();
        }
    }

    // 編集中に選択したシーンを開き、タイムラインを閉じて履歴と保存基準を初期化します。
    void EditorLayer::OpenSelectedAsset()
    {
        if (!IsSceneAsset(m_selectedAsset) || m_playing)
        {
            return;
        }

        try
        {
            // 開くシーンの絶対パス
            const auto scenePath = m_graphics.Assets().ResolvePath(m_selectedAsset);
            if (m_animationTimelineOpen)
            {
                CloseAnimationTimeline(true);
            }
            m_scene.LoadFromFile(scenePath);
            m_scenePath = scenePath;
            m_selectedObjectId = 0;
            ResetHistory();
            MarkSceneSaved();
            SetStatus("シーンを開きました: " + PathToUtf8(m_selectedAsset));
        }
        // シーン読込・Prefab操作の失敗原因
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    // 編集中に選択Prefabを主選択対象の子へ配置し、選択と履歴を更新します。
    void EditorLayer::InstantiateSelectedPrefab()
    {
        if (!IsPrefabAsset(m_selectedAsset) || m_playing)
        {
            return;
        }

        try
        {
            // Prefabを配置する親対象
            auto* parent = m_scene.FindGameObject(m_selectedObjectId);
            // 配置したPrefabルート
            auto& instance = m_scene.InstantiatePrefab(
                m_selectedAsset,
                parent);
            m_selectedObjectId = instance.Id();
            RecordHistory();
            SetStatus(
                "Prefabを配置しました: "
                + PathToUtf8(m_selectedAsset));
        }
        // シーン読込・Prefab操作の失敗原因
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    // 選択対象のPrefab変更を元アセットへ保存し、差分キャッシュと一覧を更新します。
    void EditorLayer::ApplySelectedPrefab()
    {
        // 選択中の対象
        auto* selected =
            m_scene.FindGameObject(m_selectedObjectId);
        // 選択対象を含むPrefabルート
        auto* prefabRoot = selected != nullptr
            ? m_scene.FindPrefabInstanceRoot(*selected)
            : nullptr;
        if (prefabRoot == nullptr || m_playing)
        {
            return;
        }

        try
        {
            m_scene.ApplyPrefabInstance(*prefabRoot);
            m_prefabStatusRootId = 0;
            m_prefabOverrides.clear();
            RefreshAssets();
            SetStatus(
                "Prefabへ変更を反映しました: "
                + PathToUtf8(
                    prefabRoot->PrefabAssetPath()));
        }
        // シーン読込・Prefab操作の失敗原因
        catch (const std::exception& exception)
        {
            m_prefabStatusRootId = 0;
            SetStatus(exception.what(), true);
        }
    }

    // 選択対象のPrefab階層を元アセットから置換し、選択と履歴を更新します。
    bool EditorLayer::RevertSelectedPrefab()
    {
        // 選択中の対象
        auto* selected =
            m_scene.FindGameObject(m_selectedObjectId);
        // 選択対象を含むPrefabルート
        auto* prefabRoot = selected != nullptr
            ? m_scene.FindPrefabInstanceRoot(*selected)
            : nullptr;
        if (prefabRoot == nullptr || m_playing)
        {
            return false;
        }

        try
        {
            // 復元元のPrefabパス
            const auto assetPath =
                prefabRoot->PrefabAssetPath();
            // 置換後のPrefabルート
            auto& replacement =
                m_scene.RevertPrefabInstance(
                    *prefabRoot);
            m_selectedObjectId =
                replacement.Id();
            m_prefabStatusRootId = 0;
            m_prefabOverrides.clear();
            RecordHistory();
            SetStatus(
                "Prefabの変更を元へ戻しました: "
                + PathToUtf8(assetPath));
            return true;
        }
        // シーン読込・Prefab操作の失敗原因
        catch (const std::exception& exception)
        {
            m_prefabStatusRootId = 0;
            SetStatus(exception.what(), true);
            return false;
        }
    }

    // 選択Prefabの指定差分を元アセットへ保存し、差分キャッシュを破棄します(path: 差分項目の識別パス)。
    void EditorLayer::ApplySelectedPrefabOverride(
        const std::string_view path)
    {
        // 選択中の対象
        auto* selected =
            m_scene.FindGameObject(m_selectedObjectId);
        // 選択対象を含むPrefabルート
        auto* prefabRoot = selected != nullptr
            ? m_scene.FindPrefabInstanceRoot(*selected)
            : nullptr;
        if (prefabRoot == nullptr || m_playing)
        {
            return;
        }

        try
        {
            // キャッシュ消去前にコピーした項目
            const std::string pathCopy{ path };
            m_scene.ApplyPrefabOverride(
                *prefabRoot,
                pathCopy);
            m_prefabStatusRootId = 0;
            m_prefabOverrides.clear();
            RefreshAssets();
            SetStatus(
                "Prefabの項目をApplyしました: "
                + FormatPrefabOverridePath(
                    pathCopy));
        }
        // シーン読込・Prefab操作の失敗原因
        catch (const std::exception& exception)
        {
            m_prefabStatusRootId = 0;
            m_prefabOverrides.clear();
            SetStatus(exception.what(), true);
        }
    }

    // 選択Prefabの指定差分を戻して階層を置換し、選択と履歴を更新します(path: 差分項目の識別パス)。
    bool EditorLayer::RevertSelectedPrefabOverride(
        const std::string_view path)
    {
        // 選択中の対象
        auto* selected =
            m_scene.FindGameObject(m_selectedObjectId);
        // 選択対象を含むPrefabルート
        auto* prefabRoot = selected != nullptr
            ? m_scene.FindPrefabInstanceRoot(*selected)
            : nullptr;
        if (prefabRoot == nullptr || m_playing)
        {
            return false;
        }

        try
        {
            // キャッシュ消去前にコピーした項目
            const std::string pathCopy{ path };
            // 置換後のPrefabルート
            auto& replacement =
                m_scene.RevertPrefabOverride(
                    *prefabRoot,
                    pathCopy);
            m_selectedObjectId =
                replacement.Id();
            m_prefabStatusRootId = 0;
            m_prefabOverrides.clear();
            RecordHistory();
            SetStatus(
                "Prefabの項目をRevertしました: "
                + FormatPrefabOverridePath(
                    pathCopy));
            return true;
        }
        // シーン読込・Prefab操作の失敗原因
        catch (const std::exception& exception)
        {
            m_prefabStatusRootId = 0;
            m_prefabOverrides.clear();
            SetStatus(exception.what(), true);
            return false;
        }
    }

    // 編集中に選択画像を対象の既存コンポーネントへ割り当て、履歴へ記録します。
    void EditorLayer::AssignSelectedTexture()
    {
        // 画像を割り当てる選択対象
        auto* gameObject = m_scene.FindGameObject(m_selectedObjectId);
        // スプライト画像の割当先
        auto* sprite = gameObject != nullptr
            ? gameObject->GetComponent<SpriteRendererComponent>()
            : nullptr;
        // メッシュのアルベド割当先
        auto* mesh = gameObject != nullptr
            ? gameObject->GetComponent<MeshRendererComponent>()
            : nullptr;
        // モデルのアルベド割当先
        auto* model = gameObject != nullptr
            ? gameObject->GetComponent<ModelRendererComponent>()
            : nullptr;
        // タイルシートの割当先
        auto* tilemap = gameObject != nullptr
            ? gameObject->GetComponent<
                TilemapComponent>()
            : nullptr;
        // 粒子画像の割当先
        auto* particles = gameObject != nullptr
            ? gameObject->GetComponent<
                ParticleSystemComponent>()
            : nullptr;
        // ボタン画像の割当先
        auto* uiButton = gameObject != nullptr
            ? gameObject->GetComponent<
                UIButtonComponent>()
            : nullptr;

        if ((sprite == nullptr
                && mesh == nullptr
                && model == nullptr
                && tilemap == nullptr
                && particles == nullptr
                && uiButton == nullptr)
            || !IsTextureAsset(m_selectedAsset)
            || m_playing)
        {
            return;
        }

        try
        {
            if (sprite != nullptr)
            {
                sprite->SetTexturePath(m_selectedAsset);
            }
            else if (tilemap != nullptr)
            {
                tilemap->SetTexturePath(
                    m_selectedAsset);
            }
            else if (particles != nullptr)
            {
                particles->SetTexturePath(
                    m_selectedAsset);
            }
            else if (uiButton != nullptr)
            {
                uiButton->SetTexturePath(
                    m_selectedAsset);
            }
            else
            {
                if (mesh != nullptr)
                {
                    mesh->SetAlbedoTexturePath(
                        m_selectedAsset);
                }
                else
                {
                    model->SetMaterialOverrideEnabled(true);
                    model->SetAlbedoTexturePath(
                        m_selectedAsset);
                }
            }
            RecordHistory();
            // 画像用途の通知用表示名
            const char* target = sprite != nullptr
                ? "スプライト画像"
                : tilemap != nullptr
                    ? "Tilemapのタイルシート"
                : particles != nullptr
                    ? "パーティクル画像"
                : uiButton != nullptr
                    ? "UI Button画像"
                : mesh != nullptr
                    ? "アルベド画像"
                    : "モデルのアルベド画像";
            SetStatus(
                std::string{ target }
                + "を割り当てました: "
                + PathToUtf8(m_selectedAsset));
        }
        // 素材割当を中断した失敗原因
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    // 種類と既存構成に応じて素材を割り当て、必要なコンポーネントを追加します(gameObject: 割当対象, asset: 相対アセットパス)。
    bool EditorLayer::ApplyDroppedAsset(
        GameObject& gameObject,
        const std::filesystem::path& asset)
    {
        if (m_playing || asset.empty())
        {
            return false;
        }

        try
        {
            // 素材割当結果の表示文
            std::string message;
            if (IsModelAsset(asset))
            {
                // モデル描画・制御器の割当先
                auto* model = gameObject.GetComponent<
                    ModelRendererComponent>();
                // 素材を割り当てる描画先
                auto& renderer = model != nullptr
                    ? *model
                    : gameObject.AddComponent<
                        ModelRendererComponent>();
                renderer.SetModelPath(asset);
                message = "モデルを割り当てました: ";
            }
            else if (IsTextureAsset(asset))
            {
                // タイルシートの割当先
                if (auto* tilemap = gameObject.GetComponent<
                    TilemapComponent>())
                {
                    tilemap->SetTexturePath(asset);
                    message = "タイルシートを割り当てました: ";
                }
                // ボタン画像の割当先
                else if (auto* button = gameObject.GetComponent<
                    UIButtonComponent>())
                {
                    button->SetTexturePath(asset);
                    message = "ボタン画像を割り当てました: ";
                }
                // 粒子画像・Shaderの割当先
                else if (auto* particles =
                    gameObject.GetComponent<
                        ParticleSystemComponent>())
                {
                    particles->SetTexturePath(asset);
                    message = "パーティクル画像を割り当てました: ";
                }
                // メッシュ描画素材の割当先
                else if (auto* mesh = gameObject.GetComponent<
                    MeshRendererComponent>())
                {
                    mesh->SetAlbedoTexturePath(asset);
                    message = "アルベドを割り当てました: ";
                }
                // モデル描画・制御器の割当先
                else if (auto* model = gameObject.GetComponent<
                    ModelRendererComponent>())
                {
                    model->SetAlbedoTexturePath(asset);
                    message = "アルベドを割り当てました: ";
                }
                else
                {
                    // スプライト素材の割当先
                    auto* sprite = gameObject.GetComponent<
                        SpriteRendererComponent>();
                    // 素材を割り当てる描画先
                    auto& renderer = sprite != nullptr
                        ? *sprite
                        : gameObject.AddComponent<
                            SpriteRendererComponent>();
                    renderer.SetTexturePath(asset);
                    message = "スプライトを割り当てました: ";
                }
            }
            else if (IsMaterialAsset(asset))
            {
                // メッシュ描画素材の割当先
                if (auto* mesh = gameObject.GetComponent<
                    MeshRendererComponent>())
                {
                    mesh->SetMaterialAssetPath(asset);
                }
                // モデル描画・制御器の割当先
                else if (auto* model = gameObject.GetComponent<
                    ModelRendererComponent>())
                {
                    model->SetMaterialAssetPath(asset);
                }
                else
                {
                    gameObject.AddComponent<
                        MeshRendererComponent>()
                        .SetMaterialAssetPath(asset);
                }
                message = "Materialを割り当てました: ";
            }
            else if (IsShaderErrorPlaceholder(asset))
            {
                // エラー表示用Shaderは素材として割り当てず、拒否理由を通知します。
                SetStatus(
                    "このShaderはエンジンが「壊れている印」に使うため、"
                    "割り当てられません",
                    true);
                return false;
            }
            else if (IsShaderAsset(asset))
            {
                // メッシュ描画素材の割当先
                if (auto* mesh = gameObject.GetComponent<
                    MeshRendererComponent>())
                {
                    mesh->SetShaderPath(asset);
                }
                // モデル描画・制御器の割当先
                else if (auto* model = gameObject.GetComponent<
                    ModelRendererComponent>())
                {
                    model->SetShaderPath(asset);
                }
                // スプライト素材の割当先
                else if (auto* sprite = gameObject.GetComponent<
                    SpriteRendererComponent>())
                {
                    sprite->SetShaderPath(asset);
                }
                // 粒子画像・Shaderの割当先
                else if (auto* particles =
                    gameObject.GetComponent<
                        ParticleSystemComponent>())
                {
                    particles->SetShaderPath(asset);
                }
                else
                {
                    return false;
                }
                message = "Shaderを割り当てました: ";
            }
            else if (IsAudioAsset(asset))
            {
                // 既存の音声コンポーネント
                auto* audio = gameObject.GetComponent<
                    AudioSourceComponent>();
                // 既存・追加した音声の割当先
                auto& source = audio != nullptr
                    ? *audio
                    : gameObject.AddComponent<
                        AudioSourceComponent>();
                source.SetAudioPath(asset);
                message = "オーディオを割り当てました: ";
            }
            else if (IsAnimationAsset(asset))
            {
                // 既存のTransform制御器
                auto* animator = gameObject.GetComponent<
                    TransformAnimatorComponent>();
                // 既存・追加したTransform制御器
                auto& target = animator != nullptr
                    ? *animator
                    : gameObject.AddComponent<
                        TransformAnimatorComponent>();
                target.SetClipPath(asset);
                message = "Animation Clipを割り当てました: ";
            }
            else if (IsAnimatorControllerAsset(asset))
            {
                // モデル描画・制御器の割当先
                if (auto* model = gameObject.GetComponent<
                    ModelRendererComponent>())
                {
                    model->SetAnimationControllerPath(asset);
                }
                else
                {
                    // 既存のTransform制御器
                    auto* animator = gameObject.GetComponent<
                        TransformAnimatorComponent>();
                    // 既存・追加したTransform制御器
                    auto& target = animator != nullptr
                        ? *animator
                        : gameObject.AddComponent<
                            TransformAnimatorComponent>();
                    target.SetControllerPath(asset);
                }
                message = "Animator Controllerを割り当てました: ";
            }
            else
            {
                return false;
            }

            // 割当失敗時も、追加済みコンポーネントや先に設定した値は元へ戻りません。
            m_selectedObjectId = gameObject.Id();
            RecordHistory();
            SetStatus(message + PathToUtf8(asset));
            return true;
        }
        // 素材割当を中断した失敗原因
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
            return false;
        }
    }

    // 主選択または追加選択に対象IDが含まれるか返します(id: 判定する対象ID・0は未選択)。
    bool EditorLayer::IsObjectSelected(
        const GameObjectId id) const noexcept
    {
        if (id == 0)
        {
            return false;
        }
        return m_selectedObjectId == id
            || std::ranges::find(
                m_additionalSelection,
                id) != m_additionalSelection.end();
    }

    // アセット選択を解除し、単独選択または選択集合の増減を行います(id: 対象ID・0は未選択, additive: 選択への追加・解除か)。
    void EditorLayer::SelectObject(
        const GameObjectId id,
        const bool additive)
    {
        m_selectedAsset.clear();
        if (!additive)
        {
            m_additionalSelection.clear();
            m_selectedObjectId = id;
            return;
        }
        if (id == 0)
        {
            return;
        }
        if (m_selectedObjectId == 0)
        {
            m_selectedObjectId = id;
            return;
        }
        if (m_selectedObjectId == id)
        {

            if (m_additionalSelection.empty())
            {
                m_selectedObjectId = 0;
                return;
            }
            m_selectedObjectId =
                m_additionalSelection.front();
            m_additionalSelection.erase(
                m_additionalSelection.begin());
            return;
        }
        // 追加選択にある対象IDの位置
        if (const auto found = std::ranges::find(
                m_additionalSelection,
                id);
            found != m_additionalSelection.end())
        {
            m_additionalSelection.erase(found);
            return;
        }
        m_additionalSelection.push_back(id);
    }

    // 主選択を維持して追加選択だけを解除します。
    void EditorLayer::ClearMultiSelection()
    {
        m_additionalSelection.clear();
    }

    // 主選択から順に生存する対象の借用一覧を返します。
    std::vector<GameObject*>
        EditorLayer::SelectedObjects() const
    {
        // 生存する選択対象の借用一覧
        std::vector<GameObject*> objects;
        // 生存する主選択対象
        if (auto* primary =
            m_scene.FindGameObject(m_selectedObjectId))
        {
            objects.push_back(primary);
        }
        // 追加選択の対象ID
        for (const auto id : m_additionalSelection)
        {
            // 生存する追加選択対象
            if (auto* object = m_scene.FindGameObject(id))
            {
                objects.push_back(object);
            }
        }
        return objects;
    }

    // 一致するPrefabルートをまとめて選択し、見つからなければ現在の選択を保ちます(prefabAsset: 検索するPrefab参照)。
    void EditorLayer::SelectPrefabInstances(
        const std::filesystem::path& prefabAsset)
    {
        // 一致するPrefabルートのID一覧
        std::vector<GameObjectId> found;
        // インスタンスを調べる対象
        for (const auto& gameObject : m_scene.GameObjects())
        {
            if (gameObject->IsPrefabInstanceRoot()
                && IsSameAssetReference(
                    gameObject->PrefabAssetPath(),
                    prefabAsset))
            {
                found.push_back(gameObject->Id());
            }
        }

        if (found.empty())
        {
            SetStatus(
                "このPrefabのインスタンスは"
                "シーン内にありません: "
                + PathToUtf8(prefabAsset.filename()));
            return;
        }

        ClearMultiSelection();
        m_selectedObjectId = found.front();
        m_additionalSelection.assign(
            found.begin() + 1,
            found.end());
        m_selectedAsset.clear();
        SetStatus(
            std::to_string(found.size())
            + "個のインスタンスを選択しました: "
            + PathToUtf8(prefabAsset.filename()));
    }

    // 編集中に選択モデルを既存のModelRendererへ割り当て、履歴へ記録します。
    void EditorLayer::AssignSelectedModel()
    {
        // モデルを割り当てる選択対象
        auto* gameObject = m_scene.FindGameObject(m_selectedObjectId);
        // 既存のモデル描画先
        auto* model = gameObject != nullptr
            ? gameObject->GetComponent<ModelRendererComponent>()
            : nullptr;

        if (model == nullptr || !IsModelAsset(m_selectedAsset) || m_playing)
        {
            return;
        }

        try
        {
            model->SetModelPath(m_selectedAsset);
            RecordHistory();
            SetStatus("モデルを割り当てました: " + PathToUtf8(m_selectedAsset));
        }
        // 素材割当を中断した失敗原因
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    // 編集中に選択Materialを既存の描画コンポーネントへ割り当て、履歴へ記録します。
    void EditorLayer::AssignSelectedMaterial()
    {
        // Materialを割り当てる選択対象
        auto* gameObject =
            m_scene.FindGameObject(m_selectedObjectId);
        // メッシュのMaterial割当先
        auto* mesh = gameObject != nullptr
            ? gameObject->GetComponent<MeshRendererComponent>()
            : nullptr;
        // モデルのMaterial割当先
        auto* model = gameObject != nullptr
            ? gameObject->GetComponent<ModelRendererComponent>()
            : nullptr;

        if ((mesh == nullptr && model == nullptr)
            || !IsMaterialAsset(m_selectedAsset)
            || m_playing)
        {
            return;
        }

        try
        {
            if (mesh != nullptr)
            {
                mesh->SetMaterialAssetPath(m_selectedAsset);
            }
            else
            {
                model->SetMaterialAssetPath(m_selectedAsset);
            }
            RecordHistory();
            SetStatus(
                "Lit Materialを割り当てました: "
                + PathToUtf8(m_selectedAsset));
        }
        // 素材割当を中断した失敗原因
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    // 編集中に選択クリップを事前読込し、既存のTransform制御器へ割り当てます。
    void EditorLayer::AssignSelectedAnimation()
    {
        // クリップを割り当てる選択対象
        auto* gameObject =
            m_scene.FindGameObject(
                m_selectedObjectId);
        // 既存のTransform制御器
        auto* animator = gameObject != nullptr
            ? gameObject->GetComponent<
                TransformAnimatorComponent>()
            : nullptr;
        if (animator == nullptr
            || !IsAnimationAsset(m_selectedAsset)
            || m_playing)
        {
            return;
        }

        try
        {
            static_cast<void>(
                m_graphics.Assets().
                    LoadAnimationClip(
                        m_selectedAsset));
            animator->SetClipPath(
                m_selectedAsset);
            RecordHistory();
            SetStatus(
                "Animation Clipを割り当てました: "
                + PathToUtf8(m_selectedAsset));
        }
        // 素材割当を中断した失敗原因
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    // 選択制御器を事前読込し、Transform制御器を優先して既存の制御先へ割り当てます。
    void EditorLayer::AssignSelectedAnimatorController()
    {
        // 制御器を割り当てる選択対象
        auto* gameObject =
            m_scene.FindGameObject(
                m_selectedObjectId);
        // Transform制御器の割当先
        auto* animator = gameObject != nullptr
            ? gameObject->GetComponent<
                TransformAnimatorComponent>()
            : nullptr;
        // モデル制御器の割当先
        auto* model = gameObject != nullptr
            ? gameObject->GetComponent<
                ModelRendererComponent>()
            : nullptr;
        if ((animator == nullptr && model == nullptr)
            || !IsAnimatorControllerAsset(
                m_selectedAsset)
            || m_playing)
        {
            return;
        }

        try
        {
            static_cast<void>(
                m_graphics.Assets().
                    LoadAnimatorController(
                        m_selectedAsset));
            if (animator != nullptr)
            {
                animator->SetControllerPath(
                    m_selectedAsset);
            }
            else
            {
                model->SetAnimationControllerPath(
                    m_selectedAsset);
            }
            RecordHistory();
            SetStatus(
                "Animator Controllerを割り当てました: "
                + PathToUtf8(m_selectedAsset));
        }
        // 素材割当を中断した失敗原因
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    // 同じMaterialを参照するメッシュとモデルを再読込します(materialAsset: 更新したMaterial参照)。
    void EditorLayer::ReloadSharedMaterial(
        const std::filesystem::path& materialAsset)
    {
        // 共有Material参照の更新対象
        for (const auto& gameObject : m_scene.GameObjects())
        {
            // 共有Materialを持つメッシュ
            if (auto* mesh =
                gameObject->GetComponent<MeshRendererComponent>();
                mesh != nullptr
                && IsSameAssetReference(
                    mesh->MaterialAssetPath(),
                    materialAsset))
            {
                mesh->ReloadMaterialAsset();
            }
            // 共有Materialを持つモデル
            if (auto* model =
                gameObject->GetComponent<ModelRendererComponent>();
                model != nullptr
                && IsSameAssetReference(
                    model->MaterialAssetPath(),
                    materialAsset))
            {
                model->ReloadMaterialAsset();
            }
        }
    }

    // 同じモデルを参照する描画先を再読込します(modelAsset: 更新したモデル参照)。
    void EditorLayer::ReloadSharedModel(
        const std::filesystem::path& modelAsset)
    {
        // 共有モデル参照の更新対象
        for (const auto& gameObject : m_scene.GameObjects())
        {
            // 共有モデルを持つ描画先
            if (auto* model =
                gameObject->GetComponent<ModelRendererComponent>();
                model != nullptr
                && IsSameAssetReference(
                    model->ModelPath(),
                    modelAsset))
            {
                // SetModelPathは同じパスでも再読込するため、既存パスを渡して更新します。
                model->SetModelPath(model->ModelPath());
            }
        }
    }

    // 未保存確認をせず編集中のシーンを初期カメラと太陽光へ置き換え、保存先と履歴を初期化します。
    void EditorLayer::NewScene()
    {
        if (m_playing)
        {
            return;
        }

        try
        {
            if (m_animationTimelineOpen)
            {
                CloseAnimationTimeline(true);
            }
            m_scene.Clear();

            // 初期メインカメラの対象
            auto& cameraObject = m_scene.CreateGameObject("メインカメラ");
            cameraObject.GetTransform().position = { 0.0f, 1.6f, 7.0f };
            cameraObject.GetTransform().SetEulerAngles(
                { -0.12f, 0.0f, 0.0f });
            // 初期メインカメラ
            auto& camera = cameraObject.AddComponent<CameraComponent>();
            m_scene.SetMainCamera(camera);

            // 初期太陽光の対象
            auto& lightObject =
                m_scene.CreateGameObject("太陽光");
            lightObject.GetTransform().SetEulerAngles({
                DirectX::XMConvertToRadians(-45.0f),
                DirectX::XMConvertToRadians(-35.0f),
                0.0f
            });
            lightObject.AddComponent<
                DirectionalLightComponent>();

            m_scenePath.clear();
            m_scene.Scenes().
                CancelPending();
            m_scene.Scenes().
                SetCurrentScenePath({});
            m_selectedObjectId = cameraObject.Id();
            m_playSnapshot.clear();
            ResetHistory();
            MarkSceneSaved();
            SetStatus("新しいシーンを作成しました");
        }
        // シーンの作成・保存・読込エラー
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    // 編集中のシーンを保存し、保存先未指定なら名前指定ダイアログへ進みます。
    void EditorLayer::SaveScene()
    {
        if (m_playing)
        {
            return;
        }

        if (m_scenePath.empty())
        {
            SaveSceneAs();
            return;
        }

        try
        {
            m_scene.SaveToFile(m_scenePath);
            MarkSceneSaved();
            m_scene.Scenes().
                SetCurrentScenePath(
                    m_scenePath);
            RefreshAssets();
            SetStatus("保存しました: " + PathToUtf8(m_scenePath.filename()));
        }
        // シーンの作成・保存・読込エラー
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    // ファイル選択後に未保存確認をせずシーンを開き、履歴と保存基準を初期化します。
    void EditorLayer::OpenScene()
    {
        if (m_playing)
        {
            return;
        }

        // 選択・入力するファイル名
        std::array<wchar_t, 32768> filename{};
        // シーン保存先の既定パス
        const auto sceneDirectory =
            m_graphics.Assets().AssetRoot()
            / L"scenes";
        // ダイアログの初期表示パス
        const std::wstring initialDirectory =
            sceneDirectory.wstring();

        // 選択できるファイルの一覧
        constexpr wchar_t filter[] =
            L"LamaPon シーン (*.scene.json)\0*.scene.json\0"
            L"JSON (*.json)\0*.json\0\0";

        // Win32のファイルダイアログ
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = m_window;
        dialog.lpstrFilter = filter;
        dialog.nFilterIndex = 1;
        dialog.lpstrFile = filename.data();
        dialog.nMaxFile =
            static_cast<DWORD>(filename.size());
        dialog.lpstrInitialDir =
            initialDirectory.c_str();
        dialog.lpstrTitle = L"シーンを開く";
        dialog.Flags =
            OFN_FILEMUSTEXIST
            | OFN_PATHMUSTEXIST
            | OFN_NOCHANGEDIR;

        if (!GetOpenFileNameW(&dialog))
        {
            if (CommDlgExtendedError() != 0)
            {
                SetStatus(
                    "シーンを開くダイアログを"
                    "表示できませんでした",
                    true);
            }
            return;
        }

        try
        {
            // 開くシーンのパス
            const std::filesystem::path source{
                filename.data()
            };
            if (m_animationTimelineOpen)
            {
                CloseAnimationTimeline(true);
            }
            m_scene.LoadFromFile(source);
            m_scenePath = source;
            m_selectedObjectId = 0;
            m_playSnapshot.clear();
            RefreshAssets();
            ResetHistory();
            MarkSceneSaved();
            SetStatus(
                "シーンを開きました: "
                + PathToUtf8(
                    m_scenePath.filename()));
        }
        // シーンの作成・保存・読込エラー
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    // 指定した名前へ編集中のシーンを保存し、シーンパスと保存基準を更新します。
    void EditorLayer::SaveSceneAs()
    {
        if (m_playing)
        {
            return;
        }

        // 選択・入力するファイル名
        std::array<wchar_t, 32768> filename{};
        // 保存名の初期候補
        const std::wstring suggestedName = m_scenePath.empty()
            ? L"NewScene.scene.json"
            : m_scenePath.filename().wstring();
        wcscpy_s(filename.data(), filename.size(), suggestedName.c_str());

        // シーン保存先の既定パス
        const auto sceneDirectory =
            m_graphics.Assets().AssetRoot() / L"scenes";
        // 既定保存先の作成エラー
        std::error_code directoryError;
        std::filesystem::create_directories(sceneDirectory, directoryError);
        // ダイアログの初期表示パス
        const std::wstring initialDirectory = sceneDirectory.wstring();

        // 選択できるファイルの一覧
        constexpr wchar_t filter[] =
            L"LamaPon シーン (*.scene.json)\0*.scene.json\0"
            L"JSON (*.json)\0*.json\0\0";

        // Win32のファイルダイアログ
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = m_window;
        dialog.lpstrFilter = filter;
        dialog.nFilterIndex = 1;
        dialog.lpstrFile = filename.data();
        dialog.nMaxFile = static_cast<DWORD>(filename.size());
        dialog.lpstrInitialDir = initialDirectory.c_str();
        dialog.lpstrTitle = L"シーンに名前を付けて保存";
        dialog.lpstrDefExt = L"scene.json";
        dialog.Flags =
            OFN_OVERWRITEPROMPT
            | OFN_PATHMUSTEXIST
            | OFN_NOCHANGEDIR;

        if (!GetSaveFileNameW(&dialog))
        {
            if (CommDlgExtendedError() != 0)
            {
                SetStatus("保存ダイアログを開けませんでした", true);
            }
            return;
        }

        try
        {
            // 名前を指定した保存先
            std::filesystem::path destination{ filename.data() };
            if (!HasSceneExtension(destination))
            {
                destination.replace_extension(L".scene.json");
            }

            // 保存完了後の一覧更新に失敗しても、書き込んだファイルは元へ戻りません。
            m_scene.SaveToFile(destination);
            m_scenePath = std::move(destination);
            MarkSceneSaved();
            m_scene.Scenes().
                SetCurrentScenePath(
                    m_scenePath);
            RefreshAssets();
            SetStatus(
                "名前を付けて保存しました: "
                + PathToUtf8(m_scenePath.filename()));
        }
        // シーンの作成・保存・読込エラー
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    // 主選択階層をassets内へPrefabとして保存し、元対象のPrefab参照と履歴を更新します。
    void EditorLayer::SaveSelectedAsPrefab()
    {
        // 保存する主選択対象
        auto* selected =
            m_scene.FindGameObject(m_selectedObjectId);
        if (selected == nullptr || m_playing)
        {
            return;
        }

        // 対象名から作るPrefab基本名
        const std::wstring baseName = SuggestedPrefabFileStem(selected->Name());

        // 選択・入力する保存ファイル名
        std::array<wchar_t, 32768> filename{};
        // Prefab保存名の初期候補
        const std::wstring suggestedName =
            baseName + L".prefab.json";
        wcscpy_s(
            filename.data(),
            filename.size(),
            suggestedName.c_str());

        // 字句正規化したアセットルート
        const auto assetRoot =
            std::filesystem::absolute(
                m_graphics.Assets().AssetRoot()).lexically_normal();
        // Prefab保存先の既定パス
        const auto prefabDirectory = assetRoot / L"prefabs";
        // Prefab保存先の作成エラー
        std::error_code directoryError;
        std::filesystem::create_directories(
            prefabDirectory,
            directoryError);
        if (directoryError)
        {
            SetStatus(
                "Prefabフォルダーを作成できませんでした",
                true);
            return;
        }
        // 保存ダイアログの初期表示先
        const std::wstring initialDirectory =
            prefabDirectory.wstring();

        // 保存できるファイルの一覧
        constexpr wchar_t filter[] =
            L"LamaPon Prefab (*.prefab.json)\0*.prefab.json\0"
            L"JSON (*.json)\0*.json\0\0";

        // Prefab保存ダイアログ
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = m_window;
        dialog.lpstrFilter = filter;
        dialog.nFilterIndex = 1;
        dialog.lpstrFile = filename.data();
        dialog.nMaxFile = static_cast<DWORD>(filename.size());
        dialog.lpstrInitialDir = initialDirectory.c_str();
        dialog.lpstrTitle = L"選択をPrefabとして保存";
        dialog.lpstrDefExt = L"prefab.json";
        dialog.Flags =
            OFN_OVERWRITEPROMPT
            | OFN_PATHMUSTEXIST
            | OFN_NOCHANGEDIR;

        if (!GetSaveFileNameW(&dialog))
        {
            if (CommDlgExtendedError() != 0)
            {
                SetStatus(
                    "Prefab保存ダイアログを開けませんでした",
                    true);
            }
            return;
        }

        try
        {
            // Prefabの保存先絶対パス
            std::filesystem::path destination{ filename.data() };
            if (!HasPrefabExtension(destination))
            {
                destination.replace_extension(L".prefab.json");
            }
            destination =
                std::filesystem::absolute(destination).lexically_normal();
            if (!IsPathWithin(assetRoot, destination))
            {
                throw std::runtime_error(
                    "Prefabはプロジェクトのassetsフォルダー内へ保存してください。");
            }

            m_scene.SavePrefab(*selected, destination);
            // 保存したPrefabの相対パス
            const auto relativePath =
                destination.lexically_relative(assetRoot);
            selected->SetPrefabAssetPath(
                relativePath);
            RecordHistory();
            RefreshAssets();
            m_selectedAsset = relativePath;
            m_assetDirectory = relativePath.parent_path();
            SetStatus(
                "Prefabを保存しました: "
                + PathToUtf8(relativePath));
        }
        // シーンの作成・保存・読込エラー
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    // 編集状態の保存基準と既存シーンの更新時刻を記録し、JSON取得失敗時は基準を破棄します。
    void EditorLayer::MarkSceneSaved()
    {
        try
        {
            m_savedSceneSnapshot = m_scene.SerializeToJson();
        }
        catch (const std::exception&)
        {
            m_savedSceneSnapshot.clear();
        }

        if (!m_scenePath.empty())
        {
            // 保存シーンの時刻取得エラー
            std::error_code error;
            // 保存シーンの最終更新時刻
            const auto writeTime =
                std::filesystem::last_write_time(
                    m_scenePath,
                    error);
            if (!error)
            {
                m_lastSeenSceneWriteTime = writeTime;
                m_sceneWriteTimeInitialized = true;
                m_externalSceneChangeNotified = false;
            }
        }
    }

    // 0.5秒間隔で外部更新を調べ、未保存なら一度通知し、保存済みなら再読込します。
    void EditorLayer::UpdateExternalSceneFile()
    {
        if (m_scenePath.empty()
            || m_playing
            || m_gameModuleBuildProcess != nullptr)
        {
            return;
        }

        // 外部シーン変更の監視時刻・秒
        const double now = ImGui::GetTime();
        if (now - m_lastSceneScanAt < 0.5)
        {
            return;
        }
        m_lastSceneScanAt = now;

        // 外部シーンの時刻取得エラー
        std::error_code error;
        // 外部シーンの最終更新時刻
        const auto writeTime =
            std::filesystem::last_write_time(
                m_scenePath,
                error);
        if (error)
        {
            // 外部ファイルの置換中は基準時刻を変えず、次の走査で再確認します。
            return;
        }

        if (!m_sceneWriteTimeInitialized)
        {
            m_lastSeenSceneWriteTime = writeTime;
            m_sceneWriteTimeInitialized = true;
            return;
        }
        if (writeTime <= m_lastSeenSceneWriteTime)
        {
            return;
        }

        if (HasUnsavedSceneChanges())
        {
            if (!m_externalSceneChangeNotified)
            {
                SetStatus(
                    "外部エディターでシーンが変更されました。保存するか手動で再読み込みしてください。",
                    true);
                m_externalSceneChangeNotified = true;
            }
            return;
        }

        m_externalSceneChangeNotified = false;
        ReloadScene();
    }

    // 編集状態を保存基準と比較し、比較不能なら未保存変更ありとして扱います。
    bool EditorLayer::HasUnsavedSceneChanges() const
    {
        try
        {
            // 比較する編集状態のJSON
            // 再生中は実行状態ではなく、停止時に復元する開始前の編集状態と比較します。
            const std::string current = m_playing
                ? m_playSnapshot
                : m_scene.SerializeToJson();
            return current != m_savedSceneSnapshot;
        }
        catch (const std::exception&)
        {

            return true;
        }
    }

    // 未保存変更の保存・破棄・中止を確認し、終了可能ならtrueを返します。
    bool EditorLayer::ConfirmClose()
    {
        if (!HasUnsavedSceneChanges())
        {
            return true;
        }

        if (m_scenePath.empty())
        {
            // 保存先未指定のシーンは保存ダイアログへ進まず、破棄だけを確認します。
            return MessageBoxW(
                m_window,
                L"保存されていないシーンの変更があります。\n"
                L"閉じると編集内容は失われます。閉じますか？",
                L"LamaPon Editor",
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2)
                == IDYES;
        }

        // 保存・破棄・中止の選択結果
        const int choice = MessageBoxW(
            m_window,
            L"シーンに保存していない変更があります。\n\n"
            L"[はい] 保存して閉じる\n"
            L"[いいえ] 保存せずに閉じる\n"
            L"[キャンセル] 閉じるのをやめる",
            L"LamaPon Editor",
            MB_YESNOCANCEL | MB_ICONWARNING);
        if (choice == IDCANCEL || choice == 0)
        {
            return false;
        }
        if (choice == IDYES)
        {
            try
            {
                // 再生時の変更を保存しないよう、停止して編集状態へ戻してから保存します。
                if (m_playing)
                {
                    StopPlaying();
                }
                m_scene.SaveToFile(m_scenePath);
                MarkSceneSaved();
            }
            // 終了を中止して表示する保存エラー
            catch (const std::exception& exception)
            {
                MessageBoxW(
                    m_window,
                    (L"シーンを保存できませんでした:\n"
                        + Utf8ToWide(exception.what())).c_str(),
                    L"LamaPon Editor",
                    MB_OK | MB_ICONERROR);
                return false;
            }
        }
        return true;
    }

    // 現在のパスからシーンを再読込して再生を停止し、履歴と保存基準を初期化します。
    void EditorLayer::ReloadScene()
    {
        if (m_scenePath.empty())
        {
            return;
        }

        try
        {
            if (m_animationTimelineOpen)
            {
                CloseAnimationTimeline(true);
            }
            m_scene.LoadFromFile(m_scenePath);
            if (m_scene.FindGameObject(m_selectedObjectId) == nullptr)
            {
                m_selectedObjectId = 0;
            }
            m_playing = false;
            ResetHistory();
            MarkSceneSaved();
            SetStatus(
                "再読み込みしました: "
                + PathToUtf8(m_scenePath.filename()));
        }
        // シーンの作成・保存・読込エラー
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

}
