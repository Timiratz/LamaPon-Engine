#pragma once

#include "LamaPon/Editor/OnlineDiagnosticsPanel.h"
#include "LamaPon/Editor/ServiceDiagnosticsPanel.h"

#include "LamaPon/Editor/EditorExtensionRegistry.h"
#include "LamaPon/Editor/UIComponentInspectors.h"

#include "LamaPon/Core/ApplicationLayer.h"
#include "LamaPon/Core/ProjectSettings.h"
#include "LamaPon/Editor/Editor.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Animation/AnimationClip.h"
#include "LamaPon/Editor/PackageManager.h"
#include "LamaPon/Editor/PersistencePanelState.h"
#include "LamaPon/Editor/ScriptEditorDetection.h"
#include "LamaPon/Editor/ShaderProperties.h"
#include "LamaPon/Editor/SimpleMaterialShaderGenerator.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/LitMaterial.h"
#include "LamaPon/Input/InputSystem.h"
#include "LamaPon/Scene/GameObject.h"

#include <Windows.h>
#include <DirectXMath.h>
#include <nlohmann/json_fwd.hpp>

#include <array>
#include <deque>
#include <filesystem>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace LamaPon
{
    class BgmLoopPanel;
    class EditorGuiRenderer;
    class EditorModelPreviewRenderer;
    class FrameDebuggerPanel;
    class GameExportDialog;
    class GraphicsDevice;
    class MemoryProfilerPanel;
    class MeshRendererComponent;
    class PhysicsDebuggerPanel;
    class ProfileAnalyzerPanel;
    class ProfilerPanel;
    class OnlineServices;
    class PlayerPrefs;
    class SaveDataStore;
    class Scene;
    class TilemapComponent;
    class VehicleParametersPanel;
    struct TextureAsset;
    enum class GameExportTarget;

    enum class AssetIconKind
    {
        Folder,
        Scene,
        Prefab,
        Model,
        Material,
        Shader,
        Animation,
        AnimatorController,
        CppScript,
        Generic,
        Count
    };

    class EditorLayer final : public ApplicationLayer
    {
    public:
        // 編集UIを初期化します(window: 対象ウィンドウ, graphics: 描画管理, scene: 編集シーン, playerPrefs: 設定保存, saveData: セーブ管理, onlineServices: オンライン機能, scenePath: シーンパス, engineRoot: エンジンルート, buildConfiguration: ビルド構成)。
        // 借用するウィンドウと各サービスは、このEditorLayerより長く存続させてください。
        EditorLayer(
            HWND window,
            GraphicsDevice& graphics,
            Scene& scene,
            PlayerPrefs& playerPrefs,
            SaveDataStore& saveData,
            OnlineServices& onlineServices,
            std::filesystem::path scenePath,
            std::filesystem::path engineRoot,
            std::string buildConfiguration);
        // パッケージ作業の終了を待ち、拡張機能と編集UIを破棄します。
        ~EditorLayer() override;

        // UI資源を共有するコピーを禁止します。
        EditorLayer(const EditorLayer&) = delete;
        // UI資源を共有するコピー代入を禁止します。
        EditorLayer& operator=(const EditorLayer&) = delete;

        // 外部ドロップを予約し、その他の入力をImGuiへ渡します(window: 対象ウィンドウ, message: メッセージ種別, wParam: 主パラメーター, lParam: 補助パラメーター)。
        [[nodiscard]] bool HandleMessage(
            HWND window,
            UINT message,
            WPARAM wParam,
            LPARAM lParam) const override;

        // リモート入力を注入してImGuiフレームを開始し、段階読込と初回の撮影指示を処理します。
        void BeginFrame() override;
        // 更新と編集画面を描画し、GameModuleのビルド中は入力とUI操作を止めます。
        void Draw() override;
        // Camera描画先を先に更新し、有効な選択ビューと各プレビュー画像を描画します。
        void RenderSceneViews() override;
        // 編集UIを描画し、Present前の撮影と遠隔操作の完了結果を出力します。
        void Render() override;

        // GameModuleを読み込まない復旧状態の警告表示を切り替えます(enabled: セーフモードか)。
        void SetSafeMode(const bool enabled) noexcept
        {
            m_safeMode = enabled;
        }
        // 撮影後に終了する自動撮影要求を保持します(request: 撮影対象と保存先)。
        void SetScreenshotRequest(
            EditorScreenshotOptions request)
        {
            m_screenshotRequest = std::move(request);
        }
        // 終了後に通常モードで再起動する要求があるかを返します。
        [[nodiscard]] static bool
            NormalModeRestartRequested() noexcept
        {
            return s_normalModeRestartRequested;
        }

        // ゲーム再生中かを返します。
        [[nodiscard]] bool IsPlaying() const noexcept override { return m_playing; }
        // ゲーム再生中で、更新と音声を停止しているかを返します。
        [[nodiscard]] bool IsPaused() const noexcept override
        {
            return m_playing && m_paused;
        }
        // 次の1フレーム更新の要求を返し、同時に消費します。
        [[nodiscard]] bool
            ConsumeSimulationStep() noexcept override
        {
            // 消費する1フレーム更新の要求
            const bool requested = m_stepRequested;
            m_stepRequested = false;
            return requested;
        }
        // 再生中のリモート入力を出力して残りフレーム数を消費します(snapshot: 消費した入力の出力先)。
        [[nodiscard]] bool ConsumeInputSnapshot(
            InputSnapshot& snapshot) noexcept override;
        // 文字入力中または編集時のUI入力取得中に、ゲーム側のキー入力を遮断します。
        [[nodiscard]] bool WantsKeyboard() const noexcept override;
        // 再生中に16～8192pxの固定解像度を設定し、初回の旧設定を終了時の復元用に保存します(width: 幅px, height: 高さpx)。
        [[nodiscard]] bool SetGameViewSize(
            std::uint32_t width, std::uint32_t height) override;
        // 固定時は指定サイズ、それ以外は実際の描画ターゲットのサイズを返します。
        [[nodiscard]] std::pair<std::uint32_t, std::uint32_t>
            GameViewSize() const noexcept override;
        // 未保存変更の保存・破棄・中止を確認し、終了可能ならtrueを返します。
        [[nodiscard]] bool ConfirmClose() override;

        // パネル・メニュー・ライフサイクルの拡張登録口を返します。
        [[nodiscard]] EditorExtensionRegistry& ExtensionRegistry() noexcept
        {
            return m_editorExtensions;
        }
        // パネル・メニュー・ライフサイクルの拡張登録を参照します。
        [[nodiscard]] const EditorExtensionRegistry& ExtensionRegistry()
            const noexcept
        {
            return m_editorExtensions;
        }

    private:
        // インスペクターのパネルID
        static constexpr std::string_view InspectorPanelId{ "inspector" };
        // コンソールのパネルID
        static constexpr std::string_view ConsolePanelId{ "console" };
        // 性能表示のパネルID
        static constexpr std::string_view PerformancePanelId{
            "performance" };
        // 永続化設定のパネルID
        static constexpr std::string_view PersistencePanelId{
            "persistence" };
        // アセット一覧のパネルID
        static constexpr std::string_view AssetBrowserPanelId{
            "assetBrowser" };
        // タイルパレットのパネルID
        static constexpr std::string_view TilePalettePanelId{
            "tilePalette" };
        // オンライン診断のパネルID
        static constexpr std::string_view OnlineDiagnosticsPanelId{ "onlineDiagnostics" };
        // サービス診断のパネルID
        static constexpr std::string_view ServiceDiagnosticsPanelId{ "serviceDiagnostics" };
        // パッケージ一覧のパネルID
        static constexpr std::string_view PackagesPanelId{ "packages" };
        // フレーム記録のパネルID
        static constexpr std::string_view ProfilerPanelId{ "profiler" };
        // 記録比較のパネルID
        static constexpr std::string_view ProfileAnalyzerPanelId{
            "profileAnalyzer" };
        // メモリ記録のパネルID
        static constexpr std::string_view MemoryProfilerPanelId{
            "memoryProfiler" };
        // 描画解析のパネルID
        static constexpr std::string_view FrameDebuggerPanelId{
            "frameDebugger" };
        // 物理解析のパネルID
        static constexpr std::string_view PhysicsDebuggerPanelId{
            "physicsDebugger" };
        // ImGui解析のパネルID
        static constexpr std::string_view ImGuiDebuggerPanelId{
            "imguiDebugger" };

        // 編集状態の保存基準と既存シーンの更新時刻を記録し、JSON取得失敗時は基準を破棄します。
        void MarkSceneSaved();
        // 編集状態を保存基準と比較し、比較不能なら未保存変更ありとして扱います。
        [[nodiscard]] bool HasUnsavedSceneChanges() const;
        // ウィンドウの位置とスタイルを保存して全画面へ切り替え、次回は保存した表示へ戻します。
        void ToggleFullscreen();
        // メニュー・再生操作・統計を描画し、編集用ショートカットを処理します。
        void DrawToolbar();
        // ツールバー下にドックスペースを作り、要求時と初回に既定のパネル配置を組み立てます。
        void DrawDockSpace();
        // 標準パネルとパッケージ管理を登録して解析拡張を追加します。
        void RegisterBuiltInEditorExtensions();
        // 解析パネルを生成し、描画・記録同期・終了処理を登録します。
        void RegisterAnalysisExtension();
        // プロジェクト内の解析記録先を返し、資産の基準パスが無ければ空を返します(name: 記録用途のサブフォルダー名)。
        [[nodiscard]] std::filesystem::path DebugCaptureDirectory(
            std::wstring_view name) const;
        // JSON記録を選ぶダイアログを開き、取消や失敗ならnulloptを返します(initialDirectory: 初期表示するフォルダー)。
        [[nodiscard]] std::optional<std::filesystem::path>
            OpenDebugCaptureDialog(
                const std::filesystem::path& initialDirectory);
        // 物理デバッガーがある場合にScene Viewへ補助線を描画します。
        void DrawAnalysisSceneOverlay();
        // グループ無しのパネルを先に、グループ付きは登録順のサブメニューに並べます。
        void DrawRegisteredPanelMenuItems();
        // 拡張ごとのインラインまたはサブメニューを登録順に描画します。
        void DrawRegisteredExtensionMenuItems();
        // 拡張レジストリに登録した表示中のパネルを描画します。
        void DrawRegisteredPanels();
        // ログの一時停止・検索・レベル別表示・コピーと関連対象の選択を描画します(open: ウィンドウの開閉状態)。
        void DrawConsole(bool& open);
        // ヘルプとサポート画面の表示を予約します。
        void OpenHelpCenter();
        // ヘルプの表示予約を消費し、文書・ログ閲覧とサポート情報のコピーを描画します。
        void DrawHelpCenter();
        // 既定ブラウザーでオンラインマニュアルを開き、起動成否を通知します。
        void OpenOnlineManual();
        // 存在するローカル文書を関連付けで開き、起動成否を通知します。
        void OpenLocalDocumentation();
        // 存在するログまたは保存先を関連付けで開きます(openFolder: 保存先フォルダーを開くか)。
        void OpenEditorLog(bool openFolder);
        // ビルド・動作モード・プロジェクト・シーン・ログの情報をクリップボードへコピーします。
        void CopySupportInformation();
        // ローカル文書の候補を順に調べ、見つからなければ空パスを返します。
        [[nodiscard]] std::filesystem::path
            LocalDocumentationIndexPath() const;
        // 描画時間・GPU・メモリ・物理・可視性と最新CPU計測を描画します(open: ウィンドウの開閉状態)。
        void DrawPerformancePanel(bool& open);
        // 保存先と確認対象の変化を反映し、個別設定・セーブ・同期・保護データの操作を描画します(open: ウィンドウの開閉状態)。
        void DrawPersistencePanel(bool& open);
        // Shaderの宣言済みpropertiesを返します(shaderPath: Shader)。
        [[nodiscard]] const ShaderProperties&
            ShaderPropertiesFor(
                const std::filesystem::path& shaderPath);
        // 宣言済みShader欄を編集します(shaderPath: Shader, identifier: UI識別子, getter/setter: 値操作, textureGetter/textureSetter: テクスチャ操作)。
        [[nodiscard]] ShaderPropertyEditResult
            DrawCustomShaderParameters(
            const std::filesystem::path& shaderPath,
            const char* identifier,
            const std::function<
                DirectX::XMFLOAT4(std::size_t)>& getter,
            const std::function<void(
                std::size_t,
                const DirectX::XMFLOAT4&)>& setter,
            const std::function<
                std::filesystem::path(std::size_t)>&
                textureGetter,
            const std::function<void(
                std::size_t,
                std::filesystem::path)>& textureSetter);

        // 宣言済みShaderキーワードを表示し、変更有無を返します(shaderPath: Shader, identifier: UI識別子, current: 現在値, setter: 保存処理)。
        bool DrawShaderKeywordToggles(
            const std::filesystem::path& shaderPath,
            const char* identifier,
            const ShaderKeywordSet& current,
            const std::function<void(ShaderKeywordSet)>&
                setter);

        // Shaderの割り当て時に宣言済み既定値を適用します(shaderPath: Shader, setter: 保存処理)。
        void ApplyShaderPropertyDefaults(
            const std::filesystem::path& shaderPath,
            const std::function<void(
                std::size_t,
                const DirectX::XMFLOAT4&)>& setter);

        // Render Texture選択欄を描画します(id: UI識別子, current: 現在の名前)。
        [[nodiscard]] RenderTexturePickerResult
            DrawRenderTexturePicker(
                const char* id,
                const std::string& current);

        // 階層・名前検索・選択・ドロップを描画し、走査完了後に予約した変更を実行します。
        void DrawHierarchy();
        // 追加シーンごとの階層と破棄操作を描画し、走査後に破棄要求を実行します。
        void DrawAdditiveSceneNodes();
        // 対象行の選択・操作・移動予約を描画し、開いた子ノードへ再帰します(gameObject: 描画する対象)。
        void DrawHierarchyNode(GameObject& gameObject);
        // シーンルートの操作メニューを描画し、作成と貼り付けを予約します。
        void DrawHierarchyRootContextMenu();
        // 描画中に予約した階層操作を実行し、予約を解除します。
        void ExecuteHierarchyContextAction();
        // 親変更の予約を消費し、存在する対象の親を変更して履歴へ記録します。
        void ExecutePendingHierarchyParentChange();
        // 並び替え予約を消費し、必要なら基準の親へ移してから挿入位置を変更します。
        void ExecutePendingHierarchyReorder();
        // アセットの検索・一覧・操作とドロップ受付を描画します(open: ウィンドウの開閉状態)。
        void DrawAssetBrowser(bool& open);
        // 複数ファイル選択の結果を単一・複数形式から読み、インポートを開始します。
        void OpenImportAssetsDialog();
        // 編集中にファイルを取り込み、素材の段階読込を予約します(sources: インポート元のパス一覧)。
        void ImportAssets(
            const std::vector<std::filesystem::path>& sources);
        // 先頭の素材を段階読込し、モデルの非同期準備中は次へ進めません。
        void ProcessPendingAssetImports();
        // 矩形内の外部ドロップをまとめて取り込みます(assetBrowserBounds: 画面座標の受付矩形)。
        void ProcessExternalAssetDrops(
            const RECT& assetBrowserBounds);
        // assetsルートの操作と直下のフォルダーツリーを描画します。
        void DrawAssetDirectoryTree();
        // フォルダーの選択・移動・メニューと子ツリーを描画します(directory: アセット相対フォルダー)。
        void DrawAssetDirectoryNode(const std::filesystem::path& directory);
        // アセットの種類と選択対象に応じた操作メニューを描画します(asset: 操作する相対ファイル)。
        void DrawAssetFileContextMenu(const std::filesystem::path& asset);
        // 種別アイコンを初回だけ読み、失敗も記憶します(kind: 配列範囲内のアセット種別)。
        [[nodiscard]] std::shared_ptr<const TextureAsset> GetFileTypeIcon(
            AssetIconKind kind);
        // 表示フォルダーを変更せず操作メニューを開きます(directory: 操作する相対フォルダー, isRoot: assetsルートか)。
        void DrawAssetDirectoryContextMenu(
            const std::filesystem::path& directory,
            bool isRoot);
        // 作成・取込・移動先表示とフォルダー操作を提供します(directory: 操作する相対フォルダー, isRoot: assetsルートか)。
        void DrawAssetDirectoryMenuContents(
            const std::filesystem::path& directory,
            bool isRoot);
        // 編集可能な間だけ新規アセット作成の予約を受け付けます(directory: 作成先の相対フォルダー)。
        void DrawCreateAssetMenuContents(
            const std::filesystem::path& directory);
        // アセットの作成・改名・削除Popupを描画し、参照確認と操作成功後に閉じます。
        void DrawAssetFolderDialogs();
        // フォルダーの作成ダイアログを予約します(parentDirectory: 作成先の相対フォルダー)。
        void OpenCreateAssetFolderDialog(
            const std::filesystem::path& parentDirectory);
        // シーンの作成ダイアログを予約します(parentDirectory: 作成先の相対フォルダー)。
        void OpenCreateSceneDialog(
            const std::filesystem::path& parentDirectory);
        // データアセットの作成ダイアログを予約します(parentDirectory: 作成先の相対フォルダー)。
        void OpenCreateDataAssetDialog(
            const std::filesystem::path& parentDirectory);
        // 選択型と作成先を検証し、型のスキーマからデータアセットを保存します。
        [[nodiscard]] bool CreateDataAsset();
        // マテリアルの作成ダイアログを予約します(parentDirectory: 作成先の相対フォルダー)。
        void OpenCreateMaterialDialog(
            const std::filesystem::path& parentDirectory);
        // Shaderの作成ダイアログを予約します(parentDirectory: 作成先の相対フォルダー)。
        void OpenCreateShaderDialog(
            const std::filesystem::path& parentDirectory);
        // C++Scriptの作成ダイアログを予約します(parentDirectory: 作成先の相対フォルダー)。
        void OpenCreateCppScriptDialog(
            const std::filesystem::path& parentDirectory);
        // フォルダー名変更のダイアログを予約します(directory: 操作対象の相対パス)。
        void OpenRenameAssetFolderDialog(
            const std::filesystem::path& directory);
        // フォルダー削除のダイアログを予約します(directory: 操作対象の相対パス)。
        void OpenDeleteAssetFolderDialog(
            const std::filesystem::path& directory);
        // ファイル名変更のダイアログを予約します(asset: 操作対象の相対パス)。
        void OpenRenameAssetFileDialog(
            const std::filesystem::path& asset);
        // ファイル削除のダイアログを予約します(asset: 操作対象の相対パス)。
        void OpenDeleteAssetFileDialog(
            const std::filesystem::path& asset);
        // 予約した操作の編集欄を初期化してPopupを開き、予約を消費します。
        void OpenPendingAssetDialog();
        // assets内の存在する対象をExplorerで開き、失敗を通知します(asset: 相対パス, selectFile: ファイルを選択表示するか)。
        void OpenAssetInExplorer(
            const std::filesystem::path& asset,
            bool selectFile);
        // 設定したエディターまたは既定の関連付けでコードを開きます(asset: 相対コードパス, line: 行番号・0は未指定, column: 列番号・0は未指定)。
        void OpenCodeAsset(
            const std::filesystem::path& asset,
            std::uint32_t line = 0,
            std::uint32_t column = 0);
        // GameModuleの非表示ビルドを開始し、開始成功または既に実行中ならtrueを返します。
        [[nodiscard]] bool BuildGameModule();
        // 終了済みビルドを回収し、再読込成功後に予約したScriptを追加します。
        void UpdateGameModuleBuild();
        // 開発リポジトリまたは保存済みの元リポジトリから再インストールScriptを探します。
        [[nodiscard]] std::optional<std::filesystem::path>
            DesktopReinstallScript() const;
        // API不一致を一度だけ確認し、了承と保存確認後に再インストールを起動して終了します(diagnostic: 不一致の診断文)。
        [[nodiscard]] bool OfferDesktopReinstallForGameModuleMismatch(
            const std::string& diagnostic);
        // Script更新を監視し、保存の連続が1.5秒静まった後に自動ビルドを要求します。
        void UpdateScriptAutoBuild();
        // assets内のcpp・h・hppの最新更新時刻を返し、取得できなければ既定値を返します。
        [[nodiscard]] std::filesystem::file_time_type
            LatestScriptWriteTime() const;
        // ビルド中に操作を覆う画面へ回転表示と経過時間を描画します。
        void DrawGameModuleBuildOverlay();
        // 重複を避けてScript追加を予約し、ビルド開始失敗時はその予約を取り消します(gameObject: 追加先, asset: 相対Scriptパス)。
        void QueueCppScriptAttachment(
            GameObject& gameObject,
            const std::filesystem::path& asset);
        // 存在と登録型を確認して予約Scriptを追加し、成功分の履歴と最初のエラーを通知します。
        void CompletePendingCppScriptAttachments();
        // 名前と作成先を検証して空フォルダーを作成し、表示先を切り替えます。
        [[nodiscard]] bool CreateAssetFolder();
        // 作成先を検証し、メインカメラと太陽光を持つシーンを保存します。
        [[nodiscard]] bool CreateSceneAsset();
        // 作成先を検証し、既定色のLit Materialを保存します。
        [[nodiscard]] bool CreateMaterialAsset();
        // 作成先を検証し、グラフから生成したShaderまたは雛形を保存します。
        [[nodiscard]] bool CreateShaderAsset();
        // クラス名と作成先を検証し、登録用ヘッダーを使うScript雛形を保存して開きます。
        [[nodiscard]] bool CreateCppScriptAsset();
        // フォルダー名を変更してJSONと編集中の参照を更新し、履歴を初期化します。
        [[nodiscard]] bool RenameAssetFolder();
        // 管理対象の空フォルダーだけを削除し、表示先を親へ切り替えます。
        [[nodiscard]] bool DeleteAssetFolder();
        // 元の拡張子を保つ名前を検証し、選択アセットを改名します。
        [[nodiscard]] bool RenameSelectedAsset();
        // 参照確認後に選択アセットを退避・再読込検査し、削除と編集中の参照解除を行います。
        [[nodiscard]] bool DeleteSelectedAsset();
        // 編集中のシーン・保存済みアセット・DBから削除対象の参照を集め、検査不能なら削除を止めます。
        void RefreshAssetDeleteReferences();
        // 同じファイル名でアセットを移動します(sourceAsset: 移動元の相対パス, targetDirectory: 移動先の相対パス)。
        [[nodiscard]] bool MoveAssetFile(
            const std::filesystem::path& sourceAsset,
            const std::filesystem::path& targetDirectory);
        // 編集中にフォルダーと参照を移動し、履歴を初期化します(sourceDirectory: 移動元の相対パス, targetDirectory: 移動先の相対パス)。
        [[nodiscard]] bool MoveAssetFolder(
            const std::filesystem::path& sourceDirectory,
            const std::filesystem::path& targetDirectory);
        // アセットとGUIDメタ情報を移動し、再読込検査と参照更新を行います(sourceAsset: 移動元の相対パス, destinationAsset: 移動先の相対パス)。
        [[nodiscard]] bool RelocateAssetFile(
            const std::filesystem::path& sourceAsset,
            const std::filesystem::path& destinationAsset);
        // フォルダーの相対パスを左ドラッグで渡します(directory: 対象フォルダーの相対パス)。
        void BeginAssetFolderDragSource(
            const std::filesystem::path& directory);
        // ファイル・フォルダー移動とGameObjectのPrefab作成を受け付けます(targetDirectory: ドロップ先の相対パス)。
        void AcceptAssetMoveDrop(
            const std::filesystem::path& targetDirectory);
        // 編集中の参照と起動シーンのパス接頭辞を置換します(oldDirectory: 移動元の相対パス, newDirectory: 移動先の相対パス)。
        void RemapAssetReferences(
            const std::filesystem::path& oldDirectory,
            const std::filesystem::path& newDirectory);
        // 編集中の完全一致する参照と起動シーンを置換します(oldAsset: 移動元の相対パス, newAsset: 移動先の相対パス)。
        void RemapAssetFileReferences(
            const std::filesystem::path& oldAsset,
            const std::filesystem::path& newAsset);
        // タブを登録してから選択中のビューを描画し、ビュー切替時にギズモ操作を履歴へ確定します。
        void DrawViewport();
        // シーン画像と操作UIを描画し、カメラ・ギズモ・タイル編集・選択入力を処理します。
        void DrawSceneViewport();
        // シーンカメラ・ギズモ・表示・プリセットと設定保存のPopupを描画します。
        void DrawSceneCameraSettings();
        // Inspector数値の表示書式を返します。
        [[nodiscard]] std::string InspectorFloatFormat() const;
        // ゲーム画像の解像度と配置を調整し、画像座標へ変換したポインター入力を渡します。
        void DrawGameViewport();
        // 固定解像度と描画倍率のPopupを描画します。
        void DrawGameViewResolutionSettings();
        // 選択対象の変換を操作し、複数選択へ差分を適用して操作終了時に履歴を確定します。
        void DrawTransformGizmo();
        // ビューキューブで視点を変更し、操作時は2D表示を解除してカメラ姿勢へ反映します。
        void DrawViewCube();
        // ギズモ操作中を除き、マウスのOrbit・パン・ズームと右ドラッグ中の視点移動を適用します。
        void UpdateSceneCamera();
        // 選択境界の中心と大きさに合わせて視点距離と正投影サイズを調整します。
        void FocusSelection();
        // マウスのレイに最も近い有効対象を選択し、Ctrl時は選択を追加・解除します。
        void PickSceneObject();
        // 有効カメラの視錐台を最大5mまで描画し、主カメラの色を分けます。
        void DrawCameraGizmos();
        // 有効な3Dライトの方向・範囲・円錐を描画します。
        void DrawLightGizmos();
        // 有効な選択対象の境界を描画し、主選択と追加選択の色を分けます。
        void DrawSelectionHighlight();
        // 選択中のGameObjectまたはアセットのInspectorを描画します。
        void DrawInspector(bool& open);
        // シーン環境と物理設定の補助ウィンドウを描画します。
        void DrawSceneSettingsPanels();
        // Material設定とShaderパラメーターを編集します。
        void DrawMaterialAssetInspector();
        // 選択中Materialの編集バッファを読み込みます。
        void LoadMaterialInspectorDraft();
        // 編集中のMaterialを回転する球体へ適用し、確認画像を描画します。
        void RenderMaterialPreview();
        // 初回だけMaterial確認用の球体と太陽光を持つシーンを作成します。
        void EnsureMaterialPreviewScene();
        // Shaderの診断を表示します(shaderPath: Shader, error: 診断文, identifier: UI識別子)。
        void DrawShaderError(
            const std::filesystem::path& shaderPath,
            std::string_view error,
            const char* identifier);
        // Data Assetのスキーマに従って編集欄を描画します。
        void DrawDataAssetInspector();
        // 選択中Data Assetの編集内容を読み込みます。
        void LoadDataAssetInspectorDraft();
        // 編集用JSONバッファを下書きから更新します。
        void RefreshDataAssetJsonBuffer();
        // Data Assetの下書きを保存し、成否を返します。
        [[nodiscard]] bool SaveDataAssetInspectorDraft();
        // アセット参照欄を描画し、変更有無を返します(controlId: UI識別子, field: 定義, value: 参照値)。
        [[nodiscard]] bool DrawAssetReferenceField(
            const char* controlId,
            const nlohmann::json& field,
            std::string& value);
        // 宣言済みData Assetのスキーマを返します(typeName: 型名)。
        [[nodiscard]] const std::string* FindDataAssetSchema(
            std::string_view typeName) const noexcept;
        // Data Assetファイルの型名を返します(path: アセットパス)。
        [[nodiscard]] std::string DataAssetTypeOfFile(
            const std::filesystem::path& path) const;
        // Model設定と階層を編集します。
        void DrawModelAssetInspector();
        // 選択中Modelの編集バッファを読み込みます。
        void LoadModelInspectorDraft();
        // Shaderアセット選択欄を描画します(label: 表示名, shaderPath: 選択値, allowMaterialManifests: Material Manifestを許可するか)。
        [[nodiscard]] bool DrawShaderAssetSelector(
            const char* label,
            std::filesystem::path& shaderPath,
            bool allowMaterialManifests = false);
        // Textureアセット選択欄を描画します(label: 表示名, texturePath: 選択値)。
        [[nodiscard]] bool DrawTextureAssetSelector(
            const char* label,
            std::filesystem::path& texturePath);
        // Cubemapアセット選択欄を描画します(label: 表示名, cubemapPath: 選択値)。
        [[nodiscard]] bool DrawCubemapAssetSelector(
            const char* label,
            std::filesystem::path& cubemapPath);
        // 選択タイルマップの設定と先頭1024タイルを表示し、編集と選択を処理します(open: パレットの表示状態)。
        void DrawTilePalette(bool& open);
        // 選択タイルマップのローカル座標で描画・消去し、ドラッグ終了時に履歴を確定します。
        void HandleTilemapPainting();
        // キーの時刻と変換を編集・プレビューし、閉じる際は元の変換へ戻します。
        void DrawAnimationTimeline();
        // 状態・遷移・ブレンド・イベントを編集し、閉じる時の保存失敗では画面を開いたままにします。
        void DrawAnimatorControllerGraph();
        // Component追加欄を描画します(gameObject: 追加先)。
        void DrawAddComponent(GameObject& gameObject);
        enum class BuiltInGameObjectKind
        {
            Camera,
            Sprite,
            Cube,
            Sphere,
            Cylinder,
            Plane,
            DirectionalLight,
            PointLight,
            SpotLight,
            Light2D,
            AudioSource,
            UICanvas
        };
        // 編集中に種別に応じた対象とコンポーネントを作成します(kind: 作成する組込種別)。
        void CreateBuiltInGameObject(BuiltInGameObjectKind kind);
        // 空のルート対象を作成して選択し、履歴へ記録します。
        void CreateRootGameObject();
        // 選択対象の空の子を作成して選択し、履歴へ記録します。
        void CreateChildGameObject();
        // Canvasコンポーネントを持つ対象を作成します。
        void CreateUICanvasGameObject();
        // 選択対象をIDで再確認しながら階層ごと削除し、選択解除と履歴記録を行います。
        void DeleteSelectedGameObject();
        // 編集中の選択階層を同じ親へ複製し、複製した対象を選択して履歴へ記録します。
        void DuplicateSelectedGameObject();
        // シーン全体のJSONと主選択対象のIDを、貼り付け用に保持します。
        void CopySelectedGameObject();
        // 編集中にシーンJSONと主選択対象IDを保持し、その階層を削除して履歴へ記録します。
        void CutSelectedGameObject();
        // コピー時のシーンJSONから対象を復元し、現在の選択対象と同じ親へ複製します。
        void PasteGameObject();
        // DBのファイル・実フォルダー・データ型一覧を更新して無効な選択を解除します(reuseExistingDatabase: 走査済みDBを再利用するか)。
        void RefreshAssets(bool reuseExistingDatabase = false);
        // 0.5秒間隔で外部更新を調べ、未保存なら一度通知し、保存済みなら再読込します。
        void UpdateExternalSceneFile();
        // 編集中かつビルド停止中に、選択アセットを再インポートします。
        void ReimportSelectedAsset();
        // 編集中かつビルド停止中に、全アセットを再インポートします。
        void ReimportAllAssets();
        // キャッシュを無効化し、使用中の該当参照を再読込します(asset: 対象相対パス・nulloptは全件)。
        void ReimportAssets(
            const std::optional<std::filesystem::path>& asset);
        // 編集中に選択したシーンを開き、タイムラインを閉じて履歴と保存基準を初期化します。
        void OpenSelectedAsset();
        // 編集中に選択Prefabを主選択対象の子へ配置し、選択と履歴を更新します。
        void InstantiateSelectedPrefab();
        // 選択対象のPrefab変更を元アセットへ保存し、差分キャッシュと一覧を更新します。
        void ApplySelectedPrefab();
        // 選択対象のPrefab階層を元アセットから置換し、選択と履歴を更新します。
        [[nodiscard]] bool RevertSelectedPrefab();
        // 選択Prefabの指定差分を元アセットへ保存し、差分キャッシュを破棄します(path: 差分項目の識別パス)。
        void ApplySelectedPrefabOverride(
            std::string_view path);
        // 選択Prefabの指定差分を戻して階層を置換し、選択と履歴を更新します(path: 差分項目の識別パス)。
        [[nodiscard]] bool RevertSelectedPrefabOverride(
            std::string_view path);
        // 編集中に選択画像を対象の既存コンポーネントへ割り当て、履歴へ記録します。
        void AssignSelectedTexture();
        // 種類と既存構成に応じて素材を割り当て、必要なコンポーネントを追加します(gameObject: 割当対象, asset: 相対アセットパス)。
        bool ApplyDroppedAsset(
            GameObject& gameObject,
            const std::filesystem::path& asset);
        // マウスのレイと対象境界・グリッドの交点を配置位置にし、交点がなければ注視距離を使います。
        [[nodiscard]] DirectX::XMFLOAT3
            SceneViewDropPosition() const;
        // 画像へのドロップでPrefabまたはアセットの対象を配置し、SceneとScriptは配置しません。
        void HandleSceneViewAssetDrop();
        // 主選択または追加選択に対象IDが含まれるか返します(id: 判定する対象ID・0は未選択)。
        [[nodiscard]] bool IsObjectSelected(
            GameObjectId id) const noexcept;
        // アセット選択を解除し、単独選択または選択集合の増減を行います(id: 対象ID・0は未選択, additive: 選択への追加・解除か)。
        void SelectObject(
            GameObjectId id,
            bool additive);
        // 主選択を維持して追加選択だけを解除します。
        void ClearMultiSelection();
        // 主選択から順に生存する対象の借用一覧を返します。
        [[nodiscard]] std::vector<GameObject*>
            SelectedObjects() const;

        // 一致するPrefabルートをまとめて選択し、見つからなければ現在の選択を保ちます(prefabAsset: 検索するPrefab参照)。
        void SelectPrefabInstances(
            const std::filesystem::path& prefabAsset);
        // 対象をPrefabとして保存し、選択と履歴を更新します(id: 対象オブジェクトID, targetDirectory: 保存先の相対パス)。
        void CreatePrefabFromGameObject(
            GameObjectId id,
            const std::filesystem::path& targetDirectory);

        // 編集中に選択モデルを既存のModelRendererへ割り当て、履歴へ記録します。
        void AssignSelectedModel();
        // 編集中に選択Materialを既存の描画コンポーネントへ割り当て、履歴へ記録します。
        void AssignSelectedMaterial();
        // 編集中に選択クリップを事前読込し、既存のTransform制御器へ割り当てます。
        void AssignSelectedAnimation();
        // 選択制御器を事前読込し、Transform制御器を優先して既存の制御先へ割り当てます。
        void AssignSelectedAnimatorController();
        // 選択対象のClipを読み込み、元の変換を保存してタイムラインを開きます。
        void OpenAnimationTimeline();
        // 編集中のグラフを保存して指定ファイルを開き、失敗時はエラーを通知します(controllerPath: 制御グラフのパス)。
        void OpenAnimatorControllerGraph(
            const std::filesystem::path& controllerPath);
        // 制御グラフを検証し、欠けた配置座標を補って編集状態を置き換えます。
        void LoadAnimatorControllerGraph();
        // グラフを検証して一時書込後に置換し、利用中のコンポーネントへ再読み込みします。
        void SaveAnimatorControllerGraph();
        // assets内に現在の変換のClipを作り、選択対象へ割り当ててタイムラインを開きます。
        void CreateAnimationClipForSelected();
        // Clipを読み込み、最初のキーを選択した編集状態に置き換えます(clipPath: 相対または絶対Clipパス)。
        void LoadAnimationTimeline(
            const std::filesystem::path& clipPath);
        // 編集Clipを保存して対象に再読込し、失敗は編集エラーとして通知します。
        void SaveAnimationTimeline();
        // 未保存変更を破棄してタイムラインを閉じます(restoreTransform: 元の変換を復元するか)。
        void CloseAnimationTimeline(
            bool restoreTransform);
        // 編集キーから現在時刻の変換を求め、対象の位置・回転・拡縮に適用します。
        void PreviewAnimationTimeline();
        // 同じMaterialを参照するメッシュとモデルを再読込します(materialAsset: 更新したMaterial参照)。
        void ReloadSharedMaterial(
            const std::filesystem::path& materialAsset);
        // 同じモデルを参照する描画先を再読込します(modelAsset: 更新したモデル参照)。
        void ReloadSharedModel(
            const std::filesystem::path& modelAsset);
        // 未保存確認をせず編集中のシーンを初期カメラと太陽光へ置き換え、保存先と履歴を初期化します。
        void NewScene();
        // ファイル選択後に未保存確認をせずシーンを開き、履歴と保存基準を初期化します。
        void OpenScene();
        // 編集中のシーンを保存し、保存先未指定なら名前指定ダイアログへ進みます。
        void SaveScene();
        // 指定した名前へ編集中のシーンを保存し、シーンパスと保存基準を更新します。
        void SaveSceneAs();
        // 主選択階層をassets内へPrefabとして保存し、元対象のPrefab参照と履歴を更新します。
        void SaveSelectedAsPrefab();
        // 現在のパスからシーンを再読込して再生を停止し、履歴と保存基準を初期化します。
        void ReloadScene();
        // 配布一覧と導入状態を表示し、エンジン互換性を確認して操作を提供します(open: パネルの表示状態)。
        void DrawPackagesPanel(bool& open);
        // 作成用入力と前回結果を初期化してダイアログ表示を要求します。
        void OpenPackageBuildDialog();
        // 配布情報を入力してZIPを作り、配布一覧用のJSONを表示します。
        void DrawPackageBuildDialog();
        // 再生中以外に手元のZIPを選んで配置し、cppを含む場合はビルドを要求します。
        void ImportPackageFromZipDialog();
        // 配置済みパッケージから公式一覧に無いものを表示して削除操作を提供します。
        void DrawInstalledPackagesSection();
        // 前のworkerを待ち、配布一覧の取得をバックグラウンドで開始します。
        void StartPackageIndexFetch();
        // 前のworkerを待ち、ZIP取得と検証・配置をバックグラウンドで開始します(package: コピーして保持する配布情報)。
        void StartPackageInstall(const PackageInfo& package);
        // UIスレッドで完了結果を回収し、表示・資産一覧・必要なビルドを更新します。
        void ConsumePackageWorkerResult();
        // 動作中または未回収のパッケージworkerの完了を待ちます。
        void JoinPackageWorker();
        // 配布一覧にあるパッケージの配置済み版数を更新します。
        void RefreshInstalledPackageVersions();

        // 保存済み設定を編集用のdraftへ複製して設定画面を開きます。
        void OpenProjectSettingsDialog();
        // カテゴリー別にdraftを編集し、保存成功またはキャンセルで設定画面を閉じます。
        void DrawProjectSettingsDialog();
        // ゲーム名・起動シーン・ウィンドウ・アイコンのdraftを編集します。
        void DrawProjectSettingsGameSection();
        // 描画設定のdraftを編集し、個別の品質変更をCustomとして扱います。
        void DrawProjectSettingsGraphicsSection();
        // ビューポート操作と感度のdraftを編集します。
        void DrawProjectSettingsViewportSection();
        // 物理設定と衝突レイヤー・対称マトリクスのdraftを編集します。
        void DrawProjectSettingsPhysicsSection();
        // タグ候補のdraftを編集し、列挙後に削除を適用します。
        void DrawProjectSettingsTagsSection();
        // 入力アクションのdraftを編集し、列挙後に削除を適用します。
        void DrawProjectSettingsInputSection();
        // 外部エディター・保存時ビルド・出力設定のdraftを編集します。
        void DrawProjectSettingsScriptingSection();
        // draftの保存成功後に対象別の出力ダイアログを予約します。
        void DrawProjectSettingsBuildSection();
        // オンライン設定の編集を通信設定セクションへ委譲します。
        void DrawProjectSettingsOnlineSection();
        // アカウント・クラウド・Presenceの設定draftを編集します。
        void DrawProjectSettingsServicesSection();
        // オンライン接続設定を編集します。
        void DrawProjectSettingsNetworkSection();
        // オンライン診断を表示します(open: ウィンドウの開閉状態)。
        void DrawOnlineDiagnosticsPanel(bool& open);
        // サービス診断を表示します(open: ウィンドウの開閉状態)。
        void DrawServiceDiagnosticsPanel(bool& open);
        // ログインやクラウド保存と独立したDiscord Presenceのdraftを編集します。
        void DrawProjectSettingsDiscordPresenceSection();
        // シーン遷移中の読み込み画面のdraftを編集します。
        void DrawProjectSettingsLoadingScreenSection();
        // draftを検証して保存・反映し、例外時はエラーを表示してfalseを返します。
        [[nodiscard]] bool SaveProjectSettingsDraft();
        // 実行ファイルの選択結果を外部エディターのdraftへ設定し、キャンセル時は維持します。
        void BrowseForScriptEditor();
        // プロジェクトの管理ディレクトリ内にある設定JSONの保存パスを返します。
        [[nodiscard]] std::filesystem::path ProjectSettingsPath() const;
        // プロジェクト設定を物理も含めて反映し、外部変更検出の基準を更新します。
        [[nodiscard]] bool LoadProjectConfiguration();
        // 設定を保存し、自身の保存を外部変更として扱わないよう基準を更新します。
        void SaveProjectConfiguration() const;
        // 編集可能な間に二秒ごとに内容変更を検出し、入力・描画・タグへ反映します。
        void UpdateExternalProjectSettings();
        // タグを正規化して保存し、重複なら成功、保存失敗なら登録を取り消します(tag: 登録する64byte以下のタグ名)。
        bool AddProjectTag(std::string tag);
        // プロジェクトの親フォルダーを初期位置として出力画面を開きます。
        void OpenGameExportDialog();
        // 設定画面が閉じた後に出力画面を開く対象を予約します(target: 出力対象)。
        void RequestGameExportDialog(GameExportTarget target);
        // 設定画面の終了後に予約を回収し、出力画面を描画します。
        void DrawGameExportDialog();
        // 編集状態を保存し、遷移・通信・粒子・音声・時計を初期化して再生を開始します。
        void StartPlaying();
        // 再生前のシーンと表示状態を復元し、遷移・音声・時計を編集状態へ戻します。
        void StopPlaying();
        // 再生中の更新と音声を一緒に停止・再開し、ステップ要求を消去します(paused: 一時停止するか)。
        void SetPaused(bool paused);
        // 一時停止中の再生に限り、次回更新で1フレーム進めるよう予約します。
        void RequestSimulationStep();
        // プロジェクトの管理ディレクトリ内にあるエディター設定の保存パスを返します。
        [[nodiscard]] std::filesystem::path EditorSettingsPath() const;
        // 保存された設定を順に適用し、ファイルが無ければfalseを返します。
        [[nodiscard]] bool LoadEditorSettings();
        // 現在の表示設定とプリセットを保存し、失敗は例外で通知します。
        void SaveEditorSettings() const;
        // カメラ・補助表示・資産表示とパネル配置を既定状態へ戻します。
        void ResetEditorSettings();
        // 標準・レベルデザイン・精密配置の既定プリセットを生成します。
        void CreateDefaultEditorPresets();
        // 選択プリセットを反映して設定を保存します(index: プリセットの添字・範囲外は無視)。
        void ApplyEditorPreset(std::size_t index);
        // 現在の操作設定を新しいプリセットとして保存します(name: 空白のみを除く64byte以下の名前)。
        void SaveCurrentEditorPreset(std::string name);
        // 選択中プリセットを現在の操作設定で更新して保存します。
        void UpdateSelectedEditorPreset();
        // 選択中プリセットを削除して保存し、最後の一件は残します。
        void DeleteSelectedEditorPreset();
        // 追加選択を解除し、現在の主シーンだけを持つ履歴に置き換えます。
        void ResetHistory();
        // 編集時の主シーンの変更だけを記録し、やり直し分と件数超過分を破棄します。
        void RecordHistory();
        // 再生中以外に履歴の位置を戻して主シーンを復元します。
        void Undo();
        // 再生中以外に履歴の位置を進めて主シーンを復元します。
        void Redo();
        // 主シーンの履歴を復元して追加シーンを読み直し、消えた選択対象を解除します。
        void RestoreHistoryState();
        // 現在の履歴位置より前に保存状態があるか返します。
        [[nodiscard]] bool CanUndo() const noexcept;
        // 現在の履歴位置より後に保存状態があるか返します。
        [[nodiscard]] bool CanRedo() const noexcept;
        // エディター視点の位置と姿勢から右手系ビュー行列を返します。
        [[nodiscard]] DirectX::XMMATRIX SceneViewMatrix() const noexcept;
        // 描画ターゲットの比率で正投影または画角60度の右手系投影行列を返します。
        [[nodiscard]] DirectX::XMMATRIX SceneProjectionMatrix() const noexcept;
        // 2D切替時に3D視点を退避し、復帰時は保存視点または注視点から3D視点を再構成します(enabled: 2D表示にするか)。
        void SetScene2DMode(bool enabled);
        // 状態をログへ通知して画面表示を更新します(message: 通知内容, error: エラー通知か)。
        void SetStatus(std::string message, bool error = false);

        // .lamapon/editor-menu.json から読み込む、プロジェクト専用のメニューバー項目。
        // ゲームへは書き出さず、任意階層のツール起動だけをエディターへ追加します。
        struct ProjectMenuCommand final
        {
            // 起動するコマンド
            std::string command;
            // コマンドの引数一覧
            std::vector<std::string> arguments;
            // コマンドの作業フォルダー
            std::filesystem::path workingDirectory;
            // 再生中も起動を許可するか
            bool enabledWhilePlaying{};
        };
        enum class ProjectPanelKind
        {
            BgmLoop,
            VehicleParameters
        };
        struct ProjectPanelDefinition final
        {
            // パネルの種類
            ProjectPanelKind kind{ ProjectPanelKind::BgmLoop };
            // パネルの表示名
            std::string title;
            // パネルの編集データのパス
            std::filesystem::path dataPath;
            // 保存後に起動するコマンド
            ProjectMenuCommand saveCommand;
            // パネルが表示中か
            bool open{};
        };
        struct ProjectMenuNode final
        {
            // メニューの表示名
            std::string label;
            // 選択時に起動するコマンド
            std::optional<ProjectMenuCommand> action;
            // 選択時に開くパネル番号
            std::optional<std::size_t> panelIndex;
            // 子メニューの一覧
            std::vector<ProjectMenuNode> children;
        };
        // 2秒間隔で定義の内容変更を検出し、検証成功時に専用メニューとパネルを置き換えます。
        void UpdateProjectMenus();
        // 登録されたプロジェクト専用メニューを描画します。
        void DrawProjectMenus();
        // ノードの再生可否とパネル・コマンド操作を描画し、子へ再帰します(node: 描画するノード, idPath: 親階層の識別パス)。
        void DrawProjectMenuNode(
            ProjectMenuNode& node,
            std::string_view idPath);
        // 編集中の専用パネルを描画し、閉じたBGMパネルと再生中の試聴を止めます。
        void DrawProjectPanels();
        // BGMカタログの編集パネルを描画します(panelIndex: 互換用未使用番号, panel: 表示する定義)。
        void DrawProjectBgmPanel(
            std::size_t panelIndex,
            ProjectPanelDefinition& panel);
        // 車両データの編集パネルを描画します(panelIndex: 互換用未使用番号, panel: 表示する定義)。
        void DrawProjectVehiclePanel(
            std::size_t panelIndex,
            ProjectPanelDefinition& panel);
        // 定義の引数と作業先でツールを起動し、起動成否を通知します(command: 実行するツール定義)。
        void LaunchProjectMenuCommand(
            const ProjectMenuCommand& command);

        enum class ViewportMode
        {
            None,
            Scene,
            Game
        };

        enum class GizmoOperation
        {
            Translate,
            Rotate,
            Scale
        };

        enum class TilemapTool
        {
            Paint,
            Erase
        };

        struct EditorSettingsPreset final
        {
            // プリセットの名前
            std::string name;
            // 正投影の表示高さ
            float sceneOrthographicSize{ 10.0f };
            // 視点の移動速度
            float sceneCameraSpeed{ 5.0f };
            // Shiftによる移動速度倍率
            float sceneCameraBoostMultiplier{ 3.0f };
            // 視点の回転感度
            float sceneCameraLookSensitivity{ 1.0f };
            // 視点のズーム感度
            float sceneCameraZoomSensitivity{ 1.0f };
            // グリッドの間隔
            float gridSpacing{ 1.0f };
            // グリッドの表示範囲
            float gridExtent{ 20.0f };
            // 移動のスナップ単位
            float translationSnap{ 0.5f };
            // 回転のスナップ角度
            float rotationSnap{ 15.0f };
            // 拡縮のスナップ単位
            float scaleSnap{ 0.1f };
            // ギズモの操作種別
            GizmoOperation gizmoOperation{
                GizmoOperation::Translate
            };
            // 正投影で表示するか
            bool sceneOrthographic{};
            // グリッドを表示するか
            bool gridVisible{ true };
            // コライダーの範囲を表示するか
            bool colliderDebugVisible{ true };
            // ライトギズモを表示するか
            bool lightGizmosVisible{ true };
            // カメラギズモを表示するか
            bool cameraGizmosVisible{ true };
            // ローカル座標で操作するか
            bool gizmoLocal{};
            // スナップを有効にするか
            bool snapEnabled{};
        };

        enum class AssetDialogRequest
        {
            None,
            CreateFolder,
            CreateScene,
            CreateMaterial,
            CreateDataAsset,
            CreateShader,
            CreateCppScript,
            RenameFolder,
            DeleteFolder,
            RenameFile,
            DeleteFile
        };

        enum class HierarchyContextAction
        {
            None,
            CreateRoot,
            CreateChild,
            CreateUICanvas,
            Cut,
            Copy,
            Paste,
            Duplicate,
            SaveAsPrefab,
            Delete
        };

        struct PrefabOverrideDisplay final
        {
            // 差分のプロパティパス
            std::string path;
            // 差分の表示名
            std::string label;
            // Prefab側の値の表示
            std::string sourceValue;
            // 配置した実体の値の表示
            std::string instanceValue;
            // 差分を個別に適用できるか
            bool canApplyIndividually{};
        };

        struct PendingScriptAttachment final
        {
            // Scriptを追加する対象ID
            GameObjectId gameObjectId{};
            // 追加するScriptの型名
            std::string scriptType;
            // 追加するScriptの表示名
            std::string displayName;
        };

        struct PendingExternalAssetDrop final
        {
            // ドロップした画面座標
            POINT screenPosition{};
            // インポートする元ファイル一覧
            std::vector<std::filesystem::path> sources;
        };

        // エディターのウィンドウ
        HWND m_window{};
        // 全画面解除時の元の配置
        WINDOWPLACEMENT m_windowedPlacement{
            sizeof(WINDOWPLACEMENT)
        };
        // 全画面解除時の元のスタイル
        LONG_PTR m_windowedStyle{};
        // 全画面解除時の拡張スタイル
        LONG_PTR m_windowedExtendedStyle{};
        // 全画面表示中か
        bool m_fullscreen{};
        // 借用する描画デバイス
        GraphicsDevice& m_graphics;
        // 借用する編集シーン
        Scene& m_scene;
        // 借用するユーザー設定
        PlayerPrefs& m_playerPrefs;
        // 借用するセーブデータ
        SaveDataStore& m_saveData;
        // 借用するオンライン機能
        OnlineServices& m_onlineServices;
        // 永続化パネルの編集状態
        Detail::PersistencePanelState m_persistencePanelState;
        // 編集中シーンの保存先
        std::filesystem::path m_scenePath;
        // エンジンのルートフォルダー
        std::filesystem::path m_engineRoot;
        // GameModuleのビルド構成
        std::string m_buildConfiguration;
        // 再生前の編集シーンJSON
        std::string m_playSnapshot;
        // 最後に保存・読込したJSON
        std::string m_savedSceneSnapshot;
        // 最後に確認したシーン更新時刻
        std::filesystem::file_time_type m_lastSeenSceneWriteTime{};
        // シーン監視の基準があるか
        bool m_sceneWriteTimeInitialized{};
        // 最後にシーンを走査した時刻
        double m_lastSceneScanAt{};
        // 外部変更を通知済みか
        bool m_externalSceneChangeNotified{};
        // 保存時も更新する、設定監視用の内容ハッシュ
        mutable std::uint64_t m_projectSettingsSeenHash{};
        // 設定監視の基準があるか
        mutable bool m_projectSettingsHashInitialized{};
        // 最後に設定を走査した時刻
        double m_lastProjectSettingsScanAt{};
        // コピー対象のシーンJSON
        std::string m_clipboardSceneJson;
        // コピー元の対象ID
        GameObjectId m_clipboardObjectId{};
        // Undo・Redo用のシーンJSON
        std::vector<std::string> m_history;
        // アセットの相対ファイル一覧
        std::vector<std::filesystem::path> m_assetFiles;
        // アセットの相対フォルダー一覧
        std::vector<std::filesystem::path> m_assetDirectories;
        // 一覧更新後にGPUへ1件ずつ送るアセット
        std::deque<std::filesystem::path> m_pendingAssetImports;
        // 今回のインポート予定数
        std::size_t m_pendingAssetImportTotal{};
        // インポート済みの件数
        std::size_t m_pendingAssetImportCompleted{};
        // インポート失敗件数
        std::size_t m_pendingAssetImportFailures{};
        // 最初のインポート失敗理由
        std::string m_pendingAssetImportFirstFailure;
        // 処理待ちの外部ドロップ一覧
        mutable std::vector<PendingExternalAssetDrop>
            m_pendingExternalAssetDrops;
        // 表示中のアセットフォルダー
        std::filesystem::path m_assetDirectory;
        // 選択中のアセット
        std::filesystem::path m_selectedAsset;
        // マテリアル編集対象
        std::filesystem::path m_materialInspectorAsset;
        // データアセット編集対象
        std::filesystem::path m_dataAssetInspectorAsset;
        // モデル編集対象
        std::filesystem::path m_modelInspectorAsset;
        // モデル取込倍率の編集値
        float m_modelImportScaleDraft{ 1.0f };
        // モデル設定を読込済みか
        bool m_modelInspectorLoaded{};
        // モデル設定が未保存か
        bool m_modelInspectorDirty{};
        // モデル設定の編集エラー
        std::string m_modelInspectorError;
        // アセット操作の対象パス
        std::filesystem::path m_assetDialogTarget;
        // GameModuleのビルドログ先
        std::filesystem::path m_gameModuleBuildLogPath;
        // アセット一覧の検索欄
        std::array<char, 128> m_assetFilter{};
        // ヒエラルキーの検索欄
        std::array<char, 128> m_hierarchyFilter{};
        // 主選択以外の選択対象ID
        std::vector<GameObjectId> m_additionalSelection;
        // アセットフォルダー名の編集欄
        std::array<char, 128> m_assetFolderNameBuffer{};
        // アセットファイル名の編集欄
        std::array<char, 256> m_assetFileNameBuffer{};
        // グラフからShaderを作るか
        bool m_createShaderFromGraph{ true };
        // 新規Shaderの編集グラフ
        SimpleMaterialShaderGraph m_createShaderGraph;
        // ゲーム出力ダイアログ
        std::unique_ptr<GameExportDialog> m_gameExportDialog;
        // ゲーム名の編集欄
        std::array<char, 256> m_projectGameNameBuffer{};
        // 起動シーンの編集欄
        std::array<char, 512> m_projectStartupSceneBuffer{};
        // ゲームアイコンの編集欄
        std::array<char, 512> m_projectGameIconBuffer{};
        // ウィンドウの幅と高さ
        std::array<int, 2> m_projectWindowSize{ 1280, 720 };
        // 起動演出の編集値
        bool m_projectSplashScreenDraft{ true };
        // 読み込み画面の編集設定
        SceneLoadingScreenSettings m_projectLoadingScreenDraft;
        // オンライン機能の編集設定
        OnlineProjectSettings m_projectOnlineDraft;
        // 通信機能の編集設定
        NetworkConfiguration m_projectNetworkDraft;
        // オンライン診断パネル
        OnlineDiagnosticsPanel m_onlineDiagnosticsPanel;
        // サービス診断パネル
        ServiceDiagnosticsPanel m_serviceDiagnosticsPanel;
        // サービスURLの編集欄
        std::array<char, 2049>
            m_projectOnlineServiceBaseUrlBuffer{};
        // ゲームIDの編集欄
        std::array<char, 129> m_projectOnlineGameIdBuffer{};
        // 環境IDの編集欄
        std::array<char, 65>
            m_projectOnlineEnvironmentIdBuffer{};
        // PresenceのアプリID編集欄
        std::array<char, 33>
            m_projectDiscordPresenceApplicationIdBuffer{};
        // Presenceの画像キー編集欄
        std::array<char, 257>
            m_projectDiscordPresenceImageKeyBuffer{};
        // Presenceの画像説明編集欄
        std::array<char, 129>
            m_projectDiscordPresenceImageTextBuffer{};
        // 設定カテゴリー・0はゲーム
        int m_projectSettingsCategory{};
        // 設定画面終了後の出力対象
        std::optional<GameExportTarget>
            m_requestedGameExportTarget;
        // パッケージタブの状態
        enum class PackageListState
        {
            NotLoaded,
            Loading,
            Ready,
            Failed
        };
        // パッケージ一覧の取得状態
        PackageListState m_packageListState{
            PackageListState::NotLoaded
        };
        // 取得したパッケージ一覧
        std::vector<PackageInfo> m_packages;
        // パッケージ操作のエラー
        std::string m_packagePanelError;
        // 選択中のパッケージ番号
        int m_selectedPackageIndex{ -1 };
        // パッケージの対象フィルター
        PackageTarget m_packageTargetFilter{ PackageTarget::Project };
        // パッケージ操作中・UI専用
        bool m_packageBusy{};
        // 取得・導入処理のワーカー
        std::thread m_packageWorker;
        // ワーカー→UIの受け渡し（m_packageResultMutexで保護）。
        struct PackageWorkerResult final
        {
            // UIで回収できる結果があるか
            bool ready{};
            // インストール操作の結果か
            bool wasInstall{};
            // ワーカー処理のエラー
            std::string error;
            // 取得したパッケージ一覧
            std::vector<PackageInfo> index;
            // 導入したパッケージの表示名
            std::string installedDisplayName;
            // 導入物にC++Scriptがあるか
            bool installedHasScripts{};
            // 導入後の有効化方式
            PackageActivation installedActivation{
                PackageActivation::Immediate };
        };
        // ワーカー結果を保護するmutex
        std::mutex m_packageResultMutex;
        // UIで回収するワーカー結果
        PackageWorkerResult m_packageWorkerResult;
        // 導入済みの名前とバージョン
        std::unordered_map<std::string, std::string>
            m_installedPackageVersions;
        // パッケージ出力の開く要求
        bool m_packageBuildDialogRequested{};
        // パッケージ内部名の編集欄
        std::array<char, 96> m_packageBuildNameBuffer{};
        // パッケージ表示名の編集欄
        std::array<char, 128>
            m_packageBuildDisplayNameBuffer{};
        // パッケージ説明の編集欄
        std::array<char, 512>
            m_packageBuildDescriptionBuffer{};
        // パッケージ作者名の編集欄
        std::array<char, 96> m_packageBuildAuthorBuffer{};
        // パッケージバージョン編集欄
        std::array<char, 32> m_packageBuildVersionBuffer{};
        // パッケージ出力のエラー
        std::string m_packageBuildError;
        // 公開用のパッケージJSON
        std::string m_packageBuildIndexEntry;
        // 表示プリセット名の編集欄
        std::array<char, 96> m_editorPresetNameBuffer{};
        // フォルダー操作のエラー
        std::string m_assetFolderDialogError;
        // ファイル操作のエラー
        std::string m_assetFileDialogError;
        // 削除参照の走査エラー
        std::string m_assetDeleteScanError;
        // プロジェクト設定のエラー
        std::string m_projectSettingsError;
        // マテリアル編集のエラー
        std::string m_materialInspectorError;
        // データアセット編集のエラー
        std::string m_dataAssetInspectorError;
        // 保存前のデータアセットJSON
        std::unique_ptr<nlohmann::json> m_dataAssetInspectorDraft;
        // 編集中データアセットの型名
        std::string m_dataAssetInspectorTypeName;
        // カーソル位置を保つため読込時だけ再構成するJSON欄
        std::array<char, 8192> m_dataAssetJsonBuffer{};
        // データアセットが未保存か
        bool m_dataAssetInspectorDirty{};
        // 作成するデータアセット型名
        std::string m_createDataAssetTypeName;
        // 小文字相対パスから型名の表
        std::unordered_map<std::string, std::string>
            m_dataAssetTypeByPath;
        // 削除対象を参照する一覧
        std::vector<std::string> m_assetDeleteReferences;
        // アセットダイアログの予約
        AssetDialogRequest m_assetDialogRequest{
            AssetDialogRequest::None
        };
        // ドロップ中にChildren()を変更すると、現在走査中のツリーとImGuiのTreeNode/TreePopの対応を壊すため、描画後まで保留します。
        struct PendingHierarchyParentChange final
        {
            // 親を変更する対象ID
            GameObjectId moved{};
            // 変更先の親ID・0はルート
            GameObjectId parent{};
            // 未処理の親変更があるか
            bool requested{};
        };
        // GameObjectsの走査後に適用するHierarchyの並び替え要求
        struct PendingHierarchyReorder final
        {
            // 並べ替える対象ID
            GameObjectId moved{};
            // 配置先の基準となる対象ID
            GameObjectId reference{};
            // 基準の後ろへ配置するか
            bool insertAfter{};
            // 基準と同じ親へ変更するか
            bool reparentToReferenceLevel{};
            // 未処理の並べ替えがあるか
            bool requested{};
        };
        // 回転編集中の入力角を保持し表示飛びを防ぐ対象ID
        GameObjectId m_rotationEditObjectId{};
        // 編集中のオイラー角・度
        DirectX::XMFLOAT3 m_rotationEditDegrees{};
        // 回転入力欄を編集中か
        bool m_rotationEditActive{};
        // 走査後に適用する親変更
        PendingHierarchyParentChange m_pendingHierarchyParentChange{};
        // 走査後に適用する並べ替え
        PendingHierarchyReorder m_pendingHierarchyReorder{};
        // ヒエラルキー操作の予約
        HierarchyContextAction m_hierarchyContextAction{
            HierarchyContextAction::None
        };
        // 読み込み・保存済みの設定
        ProjectSettings m_projectSettings;
        // プロジェクト専用メニュー
        std::vector<ProjectMenuNode> m_projectMenus;
        // プロジェクト専用パネル定義
        std::vector<ProjectPanelDefinition> m_projectPanels;
        // BGMのループ編集パネル
        std::unique_ptr<BgmLoopPanel> m_bgmPanel;
        // 車両パラメータ編集パネル
        std::unique_ptr<VehicleParametersPanel> m_vehicleParametersPanel;
        // 表示状態と記録を持つフレーム解析パネル
        std::unique_ptr<ProfilerPanel> m_profilerPanel;
        // プロファイル比較パネル
        std::unique_ptr<ProfileAnalyzerPanel> m_profileAnalyzerPanel;
        // メモリ記録パネル
        std::unique_ptr<MemoryProfilerPanel> m_memoryProfilerPanel;
        // 描画フレーム解析パネル
        std::unique_ptr<FrameDebuggerPanel> m_frameDebuggerPanel;
        // 物理解析パネル
        std::unique_ptr<PhysicsDebuggerPanel> m_physicsDebuggerPanel;
        // メニュー定義の内容ハッシュ
        std::uint64_t m_projectMenuManifestHash{};
        // メニュー定義を確認済みか
        bool m_projectMenuManifestSeen{};
        // メニュー定義の最終確認時刻
        double m_lastProjectMenuScanAt{ -2.0 };
        // 描画設定の編集値
        GraphicsSettings m_projectGraphicsDraft;
        // 視点操作設定の編集値
        ViewportSettings m_projectViewportDraft;
        // 物理設定の編集値
        PhysicsSettings m_projectPhysicsDraft;
        // マテリアル設定の編集値
        LitMaterial m_materialInspectorDraft;
        // 入力アクションの編集値
        std::vector<InputActionDefinition>
            m_projectInputActionsDraft;
        // タグ候補の編集値
        std::vector<std::string> m_projectTagsDraft;
        // 外部エディターの編集値
        std::filesystem::path m_projectScriptEditorDraft;
        // 保存時自動ビルドの編集値
        bool m_projectAutoBuildDraft{ true };
        // Shaderソース除外の編集値
        bool m_projectStripShaderSourceDraft{};
        // 数値の表示桁数の編集値
        int m_projectInspectorDecimalsDraft{ 1 };
        // 外部エディターの検出候補
        std::vector<ScriptEditorOption>
            m_projectScriptEditorOptions;
        // Inspectorの新規タグ名欄
        std::array<char, 64> m_newTagBuffer{};
        // 設定画面の新規タグ名欄
        std::array<char, 64> m_projectNewTagBuffer{};
        // シーンビューの描画先
        RenderTarget m_sceneRenderTarget;
        // ゲームビューの描画先
        RenderTarget m_gameRenderTarget;
        // 選択カメラのプレビュー先
        RenderTarget m_cameraPreviewRenderTarget;
        // マテリアルのプレビュー先
        RenderTarget m_materialPreviewRenderTarget;
        // マテリアル確認用のシーン
        std::unique_ptr<Scene> m_materialPreviewScene;
        // 確認用シーン内の描画担当
        MeshRendererComponent* m_materialPreviewRenderer{};
        // 固定解像度で表示するか
        bool m_gameViewFixedResolution{};
        // 固定解像度の幅px
        int m_gameViewResolutionWidth{ 1920 };
        // 固定解像度の高さpx
        int m_gameViewResolutionHeight{ 1080 };
        // ゲームビューの描画倍率
        float m_gameViewResolutionScale{ 1.0f };
        // Scriptが表示サイズを変えたか
        bool m_scriptGameViewSizeChanged{};
        // 復元用の固定解像度フラグ
        bool m_savedGameViewFixedResolution{};
        // 復元用の表示幅px
        int m_savedGameViewResolutionWidth{};
        // 復元用の表示高さpx
        int m_savedGameViewResolutionHeight{};
        // 復元用の描画倍率
        float m_savedGameViewResolutionScale{};
        // シーンカメラの位置
        DirectX::XMFLOAT3 m_sceneCameraPosition{ 0.0f, 1.8f, 7.0f };
        // シーンカメラの回転rad
        DirectX::XMFLOAT3 m_sceneCameraRotation{ -0.12f, 0.0f, 0.0f };
        // 2D切替前の3D視点位置
        DirectX::XMFLOAT3 m_scene3DCameraPosition{ 0.0f, 1.8f, 7.0f };
        // 2D切替前の3D視点回転
        DirectX::XMFLOAT3 m_scene3DCameraRotation{ -0.12f, 0.0f, 0.0f };
        // シーンカメラの注視距離
        float m_sceneCameraFocusDistance{ 7.0f };
        // 2D切替前の3D注視距離
        float m_scene3DCameraFocusDistance{ 7.0f };
        // 正投影の表示高さ
        float m_sceneOrthographicSize{ 10.0f };
        // シーンカメラの移動速度
        float m_sceneCameraSpeed{ 5.0f };
        // Shiftによる移動速度倍率
        float m_sceneCameraBoostMultiplier{ 3.0f };
        // シーンカメラの回転感度
        float m_sceneCameraLookSensitivity{ 1.0f };
        // シーンカメラのズーム感度
        float m_sceneCameraZoomSensitivity{ 1.0f };
        // シーン画像の画面上の原点
        DirectX::XMFLOAT2 m_viewportPosition{};
        // シーン画像の表示サイズ
        DirectX::XMFLOAT2 m_viewportSize{};
        // 選択中のビューポート
        ViewportMode m_activeViewport{ ViewportMode::None };
        // ギズモの操作種別
        GizmoOperation m_gizmoOperation{ GizmoOperation::Translate };
        // グリッドの間隔
        float m_gridSpacing{ 1.0f };
        // グリッドの表示範囲
        float m_gridExtent{ 20.0f };
        // 移動のスナップ単位
        float m_translationSnap{ 0.5f };
        // 回転のスナップ角度
        float m_rotationSnap{ 15.0f };
        // 拡縮のスナップ単位
        float m_scaleSnap{ 0.1f };
        // 保存済みの表示プリセット
        std::vector<EditorSettingsPreset> m_editorPresets;
        // 選択中の表示プリセット番号
        std::size_t m_selectedEditorPreset{};
        // 表示プリセット操作のエラー
        std::string m_editorPresetError;
        // ローカル座標で操作するか
        bool m_gizmoLocal{};
        // 2D表示中か
        bool m_scene2DMode{};
        // 復帰用の3D視点があるか
        bool m_scene3DViewStored{};
        // 正投影で表示するか
        bool m_sceneOrthographic{};
        // グリッドを表示するか
        bool m_gridVisible{ true };
        // コライダーの範囲を表示するか
        bool m_colliderDebugVisible{ true };
        // ライトギズモを表示するか
        bool m_lightGizmosVisible{ true };
        // カメラギズモを表示するか
        bool m_cameraGizmosVisible{ true };
        // ビューキューブを表示するか
        bool m_viewCubeVisible{ true };
        // スナップを有効にするか
        bool m_snapEnabled{};
        // アセットをグリッド表示するか
        bool m_assetGridView{ true };
        // ファイル種別ごとのアイコン
        std::array<
            std::shared_ptr<const TextureAsset>,
            static_cast<std::size_t>(AssetIconKind::Count)>
            m_fileTypeIcons{};
        // 種別アイコンを読込済みか
        std::array<
            bool,
            static_cast<std::size_t>(AssetIconKind::Count)>
            m_fileTypeIconAttempted{};
        // フォルダーツリーを表示するか
        bool m_assetDirectoryTreeVisible{};
        // 削除の参照確認を了承済みか
        bool m_assetDeleteAcknowledged{};
        // マテリアルを読込済みか
        bool m_materialInspectorLoaded{};
        // マテリアルが未保存か
        bool m_materialInspectorDirty{};
        // マウスがシーン操作領域内か
        bool m_sceneViewportHovered{};
        // マウスがビューキューブ内か
        bool m_viewCubeHovered{};
        // マウスが変換ギズモ上か
        bool m_transformGizmoHovered{};
        // 変換ギズモを操作中か
        bool m_transformGizmoUsing{};
        // ギズモがマウスを保持中か
        bool m_transformGizmoMouseCaptured{};
        // 前フレームはギズモ操作中か
        bool m_gizmoWasUsing{};
        // タイル編集ツールの種類
        TilemapTool m_tilemapTool{
            TilemapTool::Paint
        };
        // 選択中のタイル番号
        std::uint32_t m_tilePaletteSelectedTile{};
        // 現在のストロークに変更があるか
        bool m_tilemapStrokeChanged{};
        // 現在のUndo履歴の位置
        std::size_t m_historyIndex{};
        // 主選択のオブジェクトID
        GameObjectId m_selectedObjectId{};
        // ギズモ操作中の対象ID
        GameObjectId m_gizmoObjectId{};
        // Shaderのプロパティ宣言のキャッシュ（更新時刻で無効化）。
        struct CachedShaderProperties final
        {
            // 解析したShaderプロパティ
            ShaderProperties properties;
            // 解析時のファイル更新時刻
            std::filesystem::file_time_type writeTime{};
        };
        // Shaderプロパティのキャッシュ
        std::unordered_map<
            std::wstring,
            CachedShaderProperties>
            m_shaderPropertiesCache;

        // 実行中のビルドプロセス
        HANDLE m_gameModuleBuildProcess{};
        // ビルド開始時刻
        double m_gameModuleBuildStartedAt{};
        // 再インストールを確認済みか
        bool m_desktopReinstallPrompted{};
        // 最後に見たScript更新時刻
        std::filesystem::file_time_type
            m_lastSeenScriptWriteTime{};
        // Script監視の基準があるか
        bool m_scriptWriteTimeInitialized{};
        // 連続変更が収まったか判定する検知時刻
        double m_scriptChangeDetectedAt{};
        // Scriptの最終走査時刻
        double m_lastScriptScanAt{};
        // 次回Script走査までの調整済み間隔
        double m_scriptScanIntervalSeconds{ 0.5 };
        // ビルド中に次の要求があるか
        bool m_scriptRebuildQueued{};
        // ビルド後に追加するScript
        std::vector<PendingScriptAttachment>
            m_pendingScriptAttachments;
        // モデルアニメーション確認対象ID
        GameObjectId m_modelAnimationPreviewObjectId{};
        // Prefab差分確認の対象ID
        GameObjectId m_prefabStatusRootId{};
        // 次のPrefab差分確認時刻
        double m_nextPrefabStatusRefresh{};
        // Prefabに差分があるか
        bool m_prefabHasOverrides{};
        // Prefab差分確認のエラー
        std::string m_prefabStatusError;
        // 表示するPrefab差分一覧
        std::vector<PrefabOverrideDisplay>
            m_prefabOverrides;
        // 編集中アニメーションのパス
        std::filesystem::path
            m_animationTimelineClipPath;
        // 編集中のキーフレーム一覧
        std::vector<TransformKeyframe>
            m_animationTimelineKeyframes;
        // アニメーション名の編集欄
        std::array<char, 128>
            m_animationTimelineName{};
        // アニメーション編集のエラー
        std::string m_animationTimelineError;
        // プレビュー前の元の変換
        Transform m_animationTimelineOriginalTransform;
        // アニメーション編集対象ID
        GameObjectId m_animationTimelineTargetId{};
        // 選択キー番号・最大値は未選択
        std::size_t m_animationTimelineSelectedKey{
            static_cast<std::size_t>(-1)
        };
        // アニメーションの長さ・秒
        float m_animationTimelineDuration{ 1.0f };
        // プレビュー位置・秒
        float m_animationTimelineTime{};
        // プレビューをループするか
        bool m_animationTimelineLoop{ true };
        // タイムラインが表示中か
        bool m_animationTimelineOpen{};
        // アニメーションが未保存か
        bool m_animationTimelineDirty{};
        // 編集中の制御グラフのパス
        std::filesystem::path
            m_animatorGraphControllerPath;
        // 編集中の制御グラフJSON
        std::unique_ptr<nlohmann::json>
            m_animatorGraphDocument;
        // 制御グラフ編集のエラー
        std::string m_animatorGraphError;
        // 制御グラフの表示オフセット
        DirectX::XMFLOAT2 m_animatorGraphScrolling{};
        // 選択状態番号・最大値は未選択
        std::size_t m_animatorGraphSelectedState{
            static_cast<std::size_t>(-1)
        };
        // 制御グラフが表示中か
        bool m_animatorGraphOpen{};
        // 制御グラフが未保存か
        bool m_animatorGraphDirty{};
        // 状態表示のメッセージ
        std::string m_statusMessage;
        // コンソールに表示するログ
        std::vector<LogEntry> m_consoleEntries;
        // コンソールの検索欄
        std::array<char, 256> m_consoleFilter{};
        // 最後に取得したログの連番
        std::uint64_t m_consoleLastSequence{};
        // 情報ログを表示するか
        bool m_consoleShowInfo{ true };
        // 警告ログを表示するか
        bool m_consoleShowWarning{ true };
        // エラーログを表示するか
        bool m_consoleShowError{ true };
        // コンソールの更新を止めるか
        bool m_consolePaused{};
        // コンソール末尾へ追従するか
        bool m_consoleAutoScroll{ true };
        // フレーム時間の記録・ms
        std::array<float, 120>
            m_performanceFrameTimes{};
        // CPU時間の記録・ms
        std::array<float, 120>
            m_performanceCpuTimes{};
        // 次の性能記録の書込位置
        std::size_t m_performanceSampleIndex{};
        // シーン環境設定が表示中か
        bool m_sceneEnvironmentOpen{};
        // 検証中Skyboxのパス
        std::filesystem::path m_skyboxValidationPath;
        // Skybox検証のエラー
        std::string m_skyboxValidationError;
        // エディター拡張の登録一覧
        EditorExtensionRegistry m_editorExtensions;
        // コンソールへフォーカスする要求
        bool m_focusConsoleRequested{};
        // ToolbarからInspectorへPopup表示を依頼するフラグ
        bool m_addComponentPickerRequested{};
        // 状態表示がエラーか
        bool m_statusIsError{};
        // ゲーム再生中か
        bool m_playing{};
        // 描画を続けたまま更新と音声を止める状態
        bool m_paused{};
        // Applicationが1回だけ消費します。
        // 次の1フレーム更新の要求
        bool m_stepRequested{};
        // GameModuleを使わない状態か
        bool m_safeMode{};
        // 自動撮影と終了の要求
        EditorScreenshotOptions m_screenshotRequest;
        // レイアウト初期化後の1フレーム目に撮影intentを適用します。
        // 撮影モード開始からのフレーム
        std::uint32_t m_screenshotFrame{};
        // :bottom指定時は設定ペインとInspectorを撮影まで毎フレーム末尾へ移します。
        // 撮影対象を末尾へ追従するか
        bool m_screenshotScrollToBottom{};
        // 撮影指定から画面・設定カテゴリー・選択対象を開き、必要なら末尾スクロールを予約します。
        void ApplyScreenshotIntent();
        // バックバッファをPNGへ保存し、撮影結果の報告後に終了を要求します。
        void CaptureScreenshotAndQuit();
        // 最後に処理した操作指示の連番
        std::uint64_t m_remoteLastSequence{};
        // Render末尾で撮影する保存先
        std::filesystem::path m_remotePendingShot;
        // 結果を報告する指示の連番
        std::uint64_t m_remoteReportSequence{};
        // 未報告の操作結果があるか
        bool m_remoteReportPending{};
        // 報告する操作エラー
        std::string m_remoteReportError;
        // UI一覧の報告要求があるか
        bool m_remoteDumpPending{};
        // ラベルなしのUIも報告するか
        bool m_remoteDumpAll{};
        // ゲームへ注入する入力状態
        std::optional<InputSnapshot> m_remoteInputSnapshot;
        // 注入入力を維持する残りフレーム
        std::uint32_t m_remoteInputFrames{};
        // ゲーム状態の報告要求があるか
        bool m_remoteRuntimePending{};
        // 複数フレームにまたがる入力手順（set-value / drag）。
        // frameはマクロ開始からの相対フレーム番号です。
        struct RemoteStep final
        {
            // マクロ開始からの相対フレーム
            std::uint32_t frame{};
            // 指定フレームに実行する操作
            std::function<void()> action;
        };
        // 複数フレームの操作手順
        std::vector<RemoteStep> m_remoteMacro;
        // 操作手順の現在の相対フレーム
        std::uint32_t m_remoteMacroFrame{};
        // Win32の実カーソル報告を上書きするため、保持するクライアント座標を毎フレーム再注入します。
        // マウス位置を注入し続けるか
        bool m_remoteMouseHeld{};
        // 注入マウスのクライアントX
        float m_remoteMouseX{};
        // 注入マウスのクライアントY
        float m_remoteMouseY{};
        // 未処理の指示JSONを順に実行し、UI入力マクロと描画後の結果出力を予約します。
        void PollRemoteCommands();
        // 応答予約を消費し、指示の結果・要求されたUI一覧・実行状態をstate.jsonへ書きます。
        void WriteRemoteState();
        // 該当フレームの手順を実行し、最終フレーム経過後にマクロを消費します。
        void RunRemoteMacro();
        // 統計・オブジェクト・入力・最新計測と最後の64件中の警告以上をJSONで返します。
        [[nodiscard]] nlohmann::json
            BuildRemoteRuntimeState() const;
        // リモートのマウス位置を保持してImGuiへ注入します(x: 画面座標X, y: 画面座標Y)。
        void InjectMousePosition(float x, float y);
        // Win32の実カーソル報告より後に保持した位置を注入し、リモート位置を優先します。
        void ReapplyRemoteMousePosition();
        // 通常モードの再起動要求
        inline static bool s_normalModeRestartRequested{};
        // ImGuiのWin32初期化済みか
        bool m_win32Initialized{};
        // モデルプレビューの描画担当
        std::unique_ptr<EditorModelPreviewRenderer>
            m_editorModelPreviewRenderer;
        // エディターUIの描画担当
        std::unique_ptr<EditorGuiRenderer> m_editorGuiRenderer;
        // プロジェクト設定を開く要求
        bool m_projectSettingsDialogRequested{};
        // ヘルプセンターを開く要求
        bool m_helpCenterRequested{};
        // ImGuiレイアウトの保存先
        std::string m_imguiIniPath;
        // ドック配置のリセット要求
        bool m_resetDockLayout{};
        // 配置復元後にアセットを選ぶか
        bool m_selectAssetTabAfterLayoutReset{};
    };
}
