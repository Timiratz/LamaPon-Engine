#pragma once

#include "LamaPon/Editor/UIComponentInspectors.h"

#include "LamaPon/Core/ApplicationLayer.h"
#include "LamaPon/Core/ProjectSettings.h"
#include "LamaPon/Editor/Editor.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Animation/AnimationClip.h"
#include "LamaPon/Editor/PackageManager.h"
#include "LamaPon/Editor/ScriptEditorDetection.h"
#include "LamaPon/Editor/ShaderProperties.h"
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
    class GameExportDialog;
    class GraphicsDevice;
    class PlayerPrefs;
    class SaveDataStore;
    class Scene;
    class TilemapComponent;
    struct TextureAsset;
    struct ModelAsset;

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
        EditorLayer(
            HWND window,
            GraphicsDevice& graphics,
            Scene& scene,
            PlayerPrefs& playerPrefs,
            SaveDataStore& saveData,
            std::filesystem::path scenePath,
            std::filesystem::path engineRoot,
            std::string buildConfiguration);
        ~EditorLayer() override;

        EditorLayer(const EditorLayer&) = delete;
        EditorLayer& operator=(const EditorLayer&) = delete;

        [[nodiscard]] bool HandleMessage(
            HWND window,
            UINT message,
            WPARAM wParam,
            LPARAM lParam) const override;

        void BeginFrame() override;
        void Draw() override;
        void RenderSceneViews() override;
        void Render() override;

        // セーフモード（C++ Game Moduleを読み込まずに起動した状態）。
        // 直前の実行がクラッシュしたときの復旧用で、UIへ警告を出します。
        void SetSafeMode(const bool enabled) noexcept
        {
            m_safeMode = enabled;
        }
        // スクリーンショットモードを予約します（Editor.hの
        // EditorScreenshotOptionsを参照）。撮影後に自動終了します。
        void SetScreenshotRequest(
            EditorScreenshotOptions request)
        {
            m_screenshotRequest = std::move(request);
        }
        // 「通常モードで開き直す」が押されたか。プロセス終了後に
        // 起動し直すため、プロセス単位の状態として保持します。
        [[nodiscard]] static bool
            NormalModeRestartRequested() noexcept
        {
            return s_normalModeRestartRequested;
        }

        [[nodiscard]] bool IsPlaying() const noexcept override { return m_playing; }
        [[nodiscard]] bool IsPaused() const noexcept override
        {
            return m_playing && m_paused;
        }
        [[nodiscard]] bool
            ConsumeSimulationStep() noexcept override
        {
            const bool requested = m_stepRequested;
            m_stepRequested = false;
            return requested;
        }
        [[nodiscard]] bool ConsumeInputSnapshot(
            InputSnapshot& snapshot) noexcept override;
        [[nodiscard]] bool WantsKeyboard() const noexcept override;
        // 閉じる前に未保存のシーン変更を確認します
        // （保存して閉じる／保存せずに閉じる／キャンセル）。
        [[nodiscard]] bool ConfirmClose() override;

    private:
        // 最後に保存（または読み込み）した時点のシーンJSON。
        // 現在の状態との比較で未保存変更を判定します。
        void MarkSceneSaved();
        [[nodiscard]] bool HasUnsavedSceneChanges() const;
        void ToggleFullscreen();
        void DrawToolbar();
        void DrawDockSpace();
        void DrawConsole();
        void DrawPerformancePanel();
        void DrawPersistencePanel();
        // カスタムShaderのパラメーターUI。Shaderに
        // LAMAPON_PROPERTIES宣言があれば名前付きのUIを生成し、
        // 無ければ生のfloat4を8本編集する従来のUIを出します。
        // 変更があればtrueを返します。
        [[nodiscard]] const ShaderProperties&
            ShaderPropertiesFor(
                const std::filesystem::path& shaderPath);
        [[nodiscard]] bool DrawCustomShaderParameters(
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

        // Shaderが#pragma multi_compile / shader_featureで宣言した
        // キーワードをチェックボックスとして表示します。立てるとその
        // キーワード付きでコンパイルされたシェーダーが使われます。
        // 宣言が無いShaderでは何も出しません。
        bool DrawShaderKeywordToggles(
            const std::filesystem::path& shaderPath,
            const char* identifier,
            const ShaderKeywordSet& current,
            const std::function<void(ShaderKeywordSet)>&
                setter);

        // Shaderを割り当てた瞬間に、宣言された既定値を流し込みます。
        // これが無いとCustomParametersは全部0のままで、色や強さを
        // 0で受け取るShaderは真っ黒に描かれます。「割り当てたのに
        // 何も映らない」という、原因の分かりにくい躓きになるので、
        // 割り当てのたびに必ず通します（宣言が無いShaderでは何も
        // しません）。
        void ApplyShaderPropertyDefaults(
            const std::filesystem::path& shaderPath,
            const std::function<void(
                std::size_t,
                const DirectX::XMFLOAT4&)>& setter);

        // Sprite Renderer / UI Image 共通の「Render Texture」指定UI。
        [[nodiscard]] RenderTexturePickerResult
            DrawRenderTexturePicker(
                const char* id,
                const std::string& current);

        void DrawHierarchy();
        // 追加読み込み（Additive）したシーンごとのグループ表示。
        void DrawAdditiveSceneNodes();
        void DrawHierarchyNode(GameObject& gameObject);
        void DrawHierarchyRootContextMenu();
        void ExecuteHierarchyContextAction();
        // 親子関係の変更はヒエラルキーの走査後に適用します。
        void ExecutePendingHierarchyParentChange();
        // 溜めておいた並び替えを適用します（ヒエラルキーの走査後）。
        void ExecutePendingHierarchyReorder();
        void DrawAssetBrowser();
        void OpenImportAssetsDialog();
        void ImportAssets(
            const std::vector<std::filesystem::path>& sources);
        void ProcessPendingAssetImports();
        void ProcessExternalAssetDrops(
            const RECT& assetBrowserBounds);
        void DrawAssetDirectoryTree();
        void DrawAssetDirectoryNode(const std::filesystem::path& directory);
        void DrawAssetFileContextMenu(const std::filesystem::path& asset);
        [[nodiscard]] std::shared_ptr<const TextureAsset> GetFileTypeIcon(
            AssetIconKind kind);
        void DrawAssetDirectoryContextMenu(
            const std::filesystem::path& directory,
            bool isRoot);
        void DrawAssetDirectoryMenuContents(
            const std::filesystem::path& directory,
            bool isRoot);
        void DrawAssetFolderDialogs();
        void OpenCreateAssetFolderDialog(
            const std::filesystem::path& parentDirectory);
        void OpenCreateSceneDialog(
            const std::filesystem::path& parentDirectory);
        void OpenCreateDataAssetDialog(
            const std::filesystem::path& parentDirectory);
        [[nodiscard]] bool CreateDataAsset();
        void OpenCreateMaterialDialog(
            const std::filesystem::path& parentDirectory);
        void OpenCreateShaderDialog(
            const std::filesystem::path& parentDirectory);
        void OpenCreateCppScriptDialog(
            const std::filesystem::path& parentDirectory);
        void OpenRenameAssetFolderDialog(
            const std::filesystem::path& directory);
        void OpenDeleteAssetFolderDialog(
            const std::filesystem::path& directory);
        void OpenRenameAssetFileDialog(
            const std::filesystem::path& asset);
        void OpenDeleteAssetFileDialog(
            const std::filesystem::path& asset);
        void OpenPendingAssetDialog();
        void OpenAssetInExplorer(
            const std::filesystem::path& asset,
            bool selectFile);
        void OpenCodeAsset(const std::filesystem::path& asset);
        [[nodiscard]] bool BuildGameModule();
        void UpdateGameModuleBuild();
        // assets内の.cpp/.hの保存を検知して、Game Moduleを自動
        // ビルドします（プロジェクト設定でオフにできます）。
        void UpdateScriptAutoBuild();
        // 監視対象スクリプトの中で最も新しい更新時刻。
        [[nodiscard]] std::filesystem::file_time_type
            LatestScriptWriteTime() const;
        void DrawGameModuleBuildOverlay();
        void QueueCppScriptAttachment(
            GameObject& gameObject,
            const std::filesystem::path& asset);
        void CompletePendingCppScriptAttachments();
        [[nodiscard]] bool CreateAssetFolder();
        [[nodiscard]] bool CreateSceneAsset();
        [[nodiscard]] bool CreateMaterialAsset();
        [[nodiscard]] bool CreateShaderAsset();
        [[nodiscard]] bool CreateCppScriptAsset();
        [[nodiscard]] bool RenameAssetFolder();
        [[nodiscard]] bool DeleteAssetFolder();
        [[nodiscard]] bool RenameSelectedAsset();
        [[nodiscard]] bool DeleteSelectedAsset();
        void RefreshAssetDeleteReferences();
        [[nodiscard]] bool MoveAssetFile(
            const std::filesystem::path& sourceAsset,
            const std::filesystem::path& targetDirectory);
        // フォルダーを中身ごと別フォルダーへ移動します
        // （targetDirectoryが空ならassetsルート直下へ）。
        [[nodiscard]] bool MoveAssetFolder(
            const std::filesystem::path& sourceDirectory,
            const std::filesystem::path& targetDirectory);
        [[nodiscard]] bool RelocateAssetFile(
            const std::filesystem::path& sourceAsset,
            const std::filesystem::path& destinationAsset);
        // フォルダーをドラッグ元にします（中身ごと移動用）。
        void BeginAssetFolderDragSource(
            const std::filesystem::path& directory);
        void AcceptAssetMoveDrop(
            const std::filesystem::path& targetDirectory);
        void RemapAssetReferences(
            const std::filesystem::path& oldDirectory,
            const std::filesystem::path& newDirectory);
        void RemapAssetFileReferences(
            const std::filesystem::path& oldAsset,
            const std::filesystem::path& newAsset);
        void DrawViewport();
        void DrawSceneViewport();
        void DrawSceneCameraSettings();
        // Inspectorの数値表示に使う書式（"%.1f"など）。桁数は
        // プロジェクト設定のinspectorDecimalsで決まります。
        [[nodiscard]] std::string InspectorFloatFormat() const;
        void DrawGameViewport();
        void DrawGameViewResolutionSettings();
        void DrawTransformGizmo();
        void DrawViewCube();
        void UpdateSceneCamera();
        void FocusSelection();
        void PickSceneObject();
        void DrawCameraGizmos();
        void DrawLightGizmos();
        void DrawSelectionHighlight();
        void DrawInspector();
        void DrawMaterialAssetInspector();
        void LoadMaterialInspectorDraft();
        // データアセット（*.asset.json）の編集。型はGame Moduleが
        // 宣言し、スキーマから入力欄を作ります。
        void DrawDataAssetInspector();
        void LoadDataAssetInspectorDraft();
        void RefreshDataAssetJsonBuffer();
        [[nodiscard]] bool SaveDataAssetInspectorDraft();
        // スキーマのtype="asset"欄。候補一覧とドラッグ＆ドロップの
        // 受け取りをまとめて描きます。
        [[nodiscard]] bool DrawAssetReferenceField(
            const char* controlId,
            const nlohmann::json& field,
            std::string& value);
        // Game Moduleが宣言したデータアセット型のスキーマ。
        // 型が見つからなければnullptrです。
        [[nodiscard]] const std::string* FindDataAssetSchema(
            std::string_view typeName) const noexcept;
        // *.asset.jsonのファイルに書かれている型名を返します
        // （RefreshAssetsで作った表から引きます）。
        [[nodiscard]] std::string DataAssetTypeOfFile(
            const std::filesystem::path& path) const;
        void DrawModelAssetInspector();
        void LoadModelInspectorDraft();
        [[nodiscard]] bool DrawShaderAssetSelector(
            const char* label,
            std::filesystem::path& shaderPath);
        [[nodiscard]] bool DrawTextureAssetSelector(
            const char* label,
            std::filesystem::path& texturePath);
        [[nodiscard]] bool DrawCubemapAssetSelector(
            const char* label,
            std::filesystem::path& cubemapPath);
        void DrawTilePalette();
        void HandleTilemapPainting();
        void DrawAnimationTimeline();
        void DrawAnimatorControllerGraph();
        void DrawAddComponent(GameObject& gameObject);
        void CreateRootGameObject();
        void CreateChildGameObject();
        void CreateUICanvasGameObject();
        void DeleteSelectedGameObject();
        void DuplicateSelectedGameObject();
        void CopySelectedGameObject();
        void CutSelectedGameObject();
        void PasteGameObject();
        // reuseExistingDatabase=true なら、既にRefresh済みのアセット
        // データベースをそのまま使います。起動直後は SetAssetRoot が
        // 走査を終えた直後なので、ここで同じ走査をもう一度やると
        // ネットワークドライブ上のプロジェクトでは待ち時間が倍になります。
        void RefreshAssets(bool reuseExistingDatabase = false);
        void UpdateExternalSceneFile();
        void ReimportSelectedAsset();
        void OpenSelectedAsset();
        void InstantiateSelectedPrefab();
        void ApplySelectedPrefab();
        [[nodiscard]] bool RevertSelectedPrefab();
        void ApplySelectedPrefabOverride(
            std::string_view path);
        [[nodiscard]] bool RevertSelectedPrefabOverride(
            std::string_view path);
        void AssignSelectedTexture();
        // HierarchyのGameObjectへドロップされたアセットを、種類と
        // 相手の構成に応じて適切なコンポーネントへ割り当てます
        // （必要ならコンポーネントを追加）。割り当てたらtrue。
        bool ApplyDroppedAsset(
            GameObject& gameObject,
            const std::filesystem::path& asset);
        // Scene Viewのマウス位置に対応するワールド座標を返します。
        // 既存オブジェクトに当たればその表面、無ければグリッド平面
        // （2DモードはXY、3DモードはXZ）との交点を使います。
        [[nodiscard]] DirectX::XMFLOAT3
            SceneViewDropPosition() const;
        // Scene Viewへのアセットドロップを処理します。
        void HandleSceneViewAssetDrop();
        // 複数選択（m_selectedObjectIdが主選択、こちらは全選択）。
        [[nodiscard]] bool IsObjectSelected(
            GameObjectId id) const noexcept;
        void SelectObject(
            GameObjectId id,
            bool additive);
        void ClearMultiSelection();
        // 主選択を含む、実在する選択オブジェクトの一覧。
        [[nodiscard]] std::vector<GameObject*>
            SelectedObjects() const;

        // 指定Prefabから配置されたインスタンスをまとめて選択します。
        void SelectPrefabInstances(
            const std::filesystem::path& prefabAsset);
        // GameObjectをアセットフォルダーへドロップしてPrefab化します。
        void CreatePrefabFromGameObject(
            GameObjectId id,
            const std::filesystem::path& targetDirectory);

        void AssignSelectedModel();
        void AssignSelectedMaterial();
        void AssignSelectedAnimation();
        void AssignSelectedAnimatorController();
        void OpenAnimationTimeline();
        void OpenAnimatorControllerGraph(
            const std::filesystem::path& controllerPath);
        void LoadAnimatorControllerGraph();
        void SaveAnimatorControllerGraph();
        void CreateAnimationClipForSelected();
        void LoadAnimationTimeline(
            const std::filesystem::path& clipPath);
        void SaveAnimationTimeline();
        void CloseAnimationTimeline(
            bool restoreTransform);
        void PreviewAnimationTimeline();
        void ReloadSharedMaterial(
            const std::filesystem::path& materialAsset);
        void ReloadSharedModel(
            const std::filesystem::path& modelAsset);
        void NewScene();
        void OpenScene();
        void SaveScene();
        void SaveSceneAs();
        void SaveSelectedAsPrefab();
        void ReloadScene();
        // パッケージタブ（一覧取得・インストール・削除）。
        void DrawPackagesPanel();
        // 自作パッケージの書き出しダイアログ。
        void OpenPackageBuildDialog();
        void DrawPackageBuildDialog();
        // 手元のZipからパッケージを読み込みます（自作の配布用）。
        void ImportPackageFromZipDialog();
        // 公式一覧に無いインストール済みパッケージの一覧表示。
        void DrawInstalledPackagesSection();
        void StartPackageIndexFetch();
        void StartPackageInstall(const PackageInfo& package);
        void ConsumePackageWorkerResult();
        void JoinPackageWorker();
        void RefreshInstalledPackageVersions();

        void OpenProjectSettingsDialog();
        void DrawProjectSettingsDialog();
        // プロジェクト設定のカテゴリー別描画（左一覧＋右ペイン）。
        void DrawProjectSettingsGameSection();
        void DrawProjectSettingsGraphicsSection();
        void DrawProjectSettingsViewportSection();
        void DrawProjectSettingsPhysicsSection();
        void DrawProjectSettingsTagsSection();
        void DrawProjectSettingsInputSection();
        void DrawProjectSettingsScriptingSection();
        void BrowseForScriptEditor();
        [[nodiscard]] std::filesystem::path ProjectSettingsPath() const;
        [[nodiscard]] bool LoadProjectConfiguration();
        void SaveProjectConfiguration() const;
        // project.json の外部変更（git pull・手編集）を検知して
        // 入力アクション等を再読み込みします。開き直し不要にするため
        void UpdateExternalProjectSettings();
        // タグをプロジェクト設定へ登録して保存します。
        // 失敗時はステータスへ理由を出してfalseを返します。
        bool AddProjectTag(std::string tag);
        void OpenGameExportDialog();
        void DrawGameExportDialog();
        void StartPlaying();
        void StopPlaying();
        // 再生中の一時停止を切り替えます（再生していないときは無効）。
        void SetPaused(bool paused);
        // 一時停止中に1フレームだけ進めるよう要求します。
        void RequestSimulationStep();
        [[nodiscard]] std::filesystem::path EditorSettingsPath() const;
        [[nodiscard]] bool LoadEditorSettings();
        void SaveEditorSettings() const;
        void ResetEditorSettings();
        void CreateDefaultEditorPresets();
        void ApplyEditorPreset(std::size_t index);
        void SaveCurrentEditorPreset(std::string name);
        void UpdateSelectedEditorPreset();
        void DeleteSelectedEditorPreset();
        void ResetHistory();
        void RecordHistory();
        void Undo();
        void Redo();
        void RestoreHistoryState();
        [[nodiscard]] bool CanUndo() const noexcept;
        [[nodiscard]] bool CanRedo() const noexcept;
        [[nodiscard]] DirectX::XMMATRIX SceneViewMatrix() const noexcept;
        [[nodiscard]] DirectX::XMMATRIX SceneProjectionMatrix() const noexcept;
        void SetScene2DMode(bool enabled);
        void SetStatus(std::string message, bool error = false);

        // .lamapon/editor-menu.json から読み込む、プロジェクト専用の
        // メニューバー項目。ゲームへは書き出さず、任意階層のツール起動だけを
        // エディターへ追加します。
        struct ProjectMenuCommand final
        {
            std::string command;
            std::vector<std::string> arguments;
            std::filesystem::path workingDirectory;
            bool enabledWhilePlaying{};
        };
        enum class ProjectPanelKind
        {
            StageRoad,
            VehicleParameters,
            BgmLoop
        };
        struct ProjectPanelDefinition final
        {
            ProjectPanelKind kind{ ProjectPanelKind::StageRoad };
            std::string title;
            std::filesystem::path dataPath;
            ProjectMenuCommand saveCommand;
            bool open{};
        };
        struct ProjectMenuNode final
        {
            std::string label;
            std::optional<ProjectMenuCommand> action;
            std::optional<std::size_t> panelIndex;
            std::vector<ProjectMenuNode> children;
        };
        void UpdateProjectMenus();
        void DrawProjectMenus();
        void DrawProjectMenuNode(
            ProjectMenuNode& node,
            std::string_view idPath);
        void DrawProjectPanels();
        void DrawProjectStagePanel(
            std::size_t panelIndex,
            ProjectPanelDefinition& panel);
        void DrawProjectVehiclePanel(
            std::size_t panelIndex,
            ProjectPanelDefinition& panel);
        bool LoadProjectStageProfile(std::size_t profileIndex);
        void ClearProjectStageMapPreview();
        void BuildProjectStageMapPreview(const nlohmann::json& profile);
        void RebuildProjectStageMapPreview();
        bool SaveProjectStageProfile(ProjectPanelDefinition& panel);
        [[nodiscard]] DirectX::XMMATRIX
            ProjectStageViewMatrix() const noexcept;
        [[nodiscard]] DirectX::XMMATRIX
            ProjectStageProjectionMatrix() const noexcept;
        bool LoadProjectVehicleData(ProjectPanelDefinition& panel);
        bool SaveProjectVehicleData(ProjectPanelDefinition& panel);
        void DrawProjectBgmPanel(
            std::size_t panelIndex,
            ProjectPanelDefinition& panel);
        void LaunchProjectMenuCommand(
            const ProjectMenuCommand& command);

        struct ProjectRoadSection final
        {
            DirectX::XMFLOAT3 left{};
            DirectX::XMFLOAT3 center{};
            DirectX::XMFLOAT3 right{};
            float baseWidth{};
            float baseLeftWidth{};
            float baseRightWidth{};
        };
        struct ProjectStageEditSnapshot final
        {
            std::vector<float> widths;
            std::vector<float> edgeBiases;
            std::vector<float> heights;
            int startSection{};
            int goalSection{};
        };
        struct ProjectStageEditorState final
        {
            std::size_t panelIndex{ static_cast<std::size_t>(-1) };
            std::size_t profileIndex{};
            bool loaded{};
            bool dirty{};
            bool fitView{ true };
            bool focusSelection{};
            int draggingRoadEdge{};
            int selectedSection{};
            int selectionStart{};
            int selectionEnd{};
            int startSection{};
            int goalSection{};
            float editLeftWidth{};
            float editRightWidth{};
            float editHeight{};
            float brushRadius{ 20.0f };
            float smoothStrength{ 0.55f };
            int smoothIterations{ 3 };
            float zoom{ 1.0f };
            DirectX::XMFLOAT2 pan{};
            bool view3D{};
            bool showVertices{ true };
            bool showColliders{ true };
            // 3Dプレビューの右上へステージのミニマップを重ねます。
            // 参照画像と同じテクスチャを使うため、ステージのモデルや
            // 道路データを複製せずに全体位置を確認できます。
            bool mapOverlayVisible{ true };
            // 編集中の3Dプレビューは変更時だけ再描画します。再生中と
            // カメラ操作中も更新頻度と解像度を抑え、本画面のFPSを
            // 優先します。
            int previewFrameRate{ 15 };
            float previewRenderScale{ 0.5f };
            double previewLastRenderAt{};
            bool previewHasRendered{};
            bool previewNeedsRender{ true };
            bool mapModelVisible{ true };
            GameObjectId mapPreviewRootId{};
            std::size_t mapPreviewModelCount{};
            std::string mapPreviewError;
            bool previewVisible{};
            DirectX::XMFLOAT2 previewSize{};
            float orbitYawDegrees{ -38.0f };
            float orbitPitchDegrees{ 48.0f };
            float verticalScale{ 2.0f };
            bool referenceVisible{ true };
            bool referenceFlipHorizontal{};
            bool referenceFlipVertical{};
            float referenceOpacity{ 0.34f };
            float referenceScale{ 1.0f };
            float referenceRotationDegrees{};
            DirectX::XMFLOAT2 referenceOffset{};
            std::filesystem::path referenceImagePath;
            std::shared_ptr<const TextureAsset> referenceTexture;
            std::vector<ProjectRoadSection> baseSections;
            std::vector<float> widths;
            std::vector<float> edgeBiases;
            std::vector<float> heights;
            std::vector<ProjectStageEditSnapshot> undo;
            std::vector<ProjectStageEditSnapshot> redo;
            std::unique_ptr<nlohmann::json> profiles;
            std::unique_ptr<nlohmann::json> overrideDocument;
        };
        struct ProjectVehicleEditorState final
        {
            std::size_t panelIndex{ static_cast<std::size_t>(-1) };
            int selectedVehicle{};
            bool loaded{};
            bool dirty{};
            std::unique_ptr<nlohmann::json> document;
            int previewVehicle{ -1 };
            std::shared_ptr<const ModelAsset> previewModel;
        };
        // BGMのループ範囲と試聴開始位置を波形の上で決めるパネル。
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
            std::string name;
            float sceneOrthographicSize{ 10.0f };
            float sceneCameraSpeed{ 5.0f };
            float sceneCameraBoostMultiplier{ 3.0f };
            float sceneCameraLookSensitivity{ 1.0f };
            float sceneCameraZoomSensitivity{ 1.0f };
            float gridSpacing{ 1.0f };
            float gridExtent{ 20.0f };
            float translationSnap{ 0.5f };
            float rotationSnap{ 15.0f };
            float scaleSnap{ 0.1f };
            GizmoOperation gizmoOperation{
                GizmoOperation::Translate
            };
            bool sceneOrthographic{};
            bool gridVisible{ true };
            bool colliderDebugVisible{ true };
            bool lightGizmosVisible{ true };
            bool cameraGizmosVisible{ true };
            bool gizmoLocal{};
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
            std::string path;
            std::string label;
            std::string sourceValue;
            std::string instanceValue;
            bool canApplyIndividually{};
        };

        struct PendingScriptAttachment final
        {
            GameObjectId gameObjectId{};
            std::string scriptType;
            std::string displayName;
        };

        struct PendingExternalAssetDrop final
        {
            POINT screenPosition{};
            std::vector<std::filesystem::path> sources;
        };

        HWND m_window{};
        WINDOWPLACEMENT m_windowedPlacement{
            sizeof(WINDOWPLACEMENT)
        };
        LONG_PTR m_windowedStyle{};
        LONG_PTR m_windowedExtendedStyle{};
        bool m_fullscreen{};
        GraphicsDevice& m_graphics;
        Scene& m_scene;
        PlayerPrefs& m_playerPrefs;
        SaveDataStore& m_saveData;
        std::filesystem::path m_scenePath;
        std::filesystem::path m_engineRoot;
        std::string m_buildConfiguration;
        std::string m_playSnapshot;
        std::string m_savedSceneSnapshot;
        // 外部エディターから保存されたシーンを検知するための状態。
        std::filesystem::file_time_type m_lastSeenSceneWriteTime{};
        bool m_sceneWriteTimeInitialized{};
        double m_lastSceneScanAt{};
        bool m_externalSceneChangeNotified{};
        // project.json の外部変更検知（内容ハッシュ。WebDAVのmtimeは
        // 当てにしない）。SaveProjectConfiguration(const)が自分の保存を
        // 誤検知しないよう基準を更新するため mutable
        mutable std::uint64_t m_projectSettingsSeenHash{};
        mutable bool m_projectSettingsHashInitialized{};
        double m_lastProjectSettingsScanAt{};
        std::string m_clipboardSceneJson;
        GameObjectId m_clipboardObjectId{};
        std::vector<std::string> m_history;
        std::vector<std::filesystem::path> m_assetFiles;
        std::vector<std::filesystem::path> m_assetDirectories;
        // ファイルのコピーと一覧更新を先に完了し、GPUリソースを
        // 作る重いロードは1フレームに1件ずつ進めます。
        std::deque<std::filesystem::path> m_pendingAssetImports;
        std::size_t m_pendingAssetImportTotal{};
        std::size_t m_pendingAssetImportCompleted{};
        std::size_t m_pendingAssetImportFailures{};
        std::string m_pendingAssetImportFirstFailure;
        mutable std::vector<PendingExternalAssetDrop>
            m_pendingExternalAssetDrops;
        std::filesystem::path m_assetDirectory;
        std::filesystem::path m_selectedAsset;
        std::filesystem::path m_materialInspectorAsset;
        std::filesystem::path m_dataAssetInspectorAsset;
        std::filesystem::path m_modelInspectorAsset;
        float m_modelImportScaleDraft{ 1.0f };
        bool m_modelInspectorLoaded{};
        bool m_modelInspectorDirty{};
        std::string m_modelInspectorError;
        std::filesystem::path m_assetDialogTarget;
        std::filesystem::path m_gameModuleBuildLogPath;
        std::array<char, 128> m_assetFilter{};
        std::array<char, 128> m_hierarchyFilter{};
        // 主選択以外の追加選択（Ctrl+クリックで増減）。
        std::vector<GameObjectId> m_additionalSelection;
        std::array<char, 128> m_assetFolderNameBuffer{};
        std::array<char, 256> m_assetFileNameBuffer{};
        std::unique_ptr<GameExportDialog> m_gameExportDialog;
        std::array<char, 256> m_projectGameNameBuffer{};
        std::array<char, 512> m_projectStartupSceneBuffer{};
        std::array<char, 512> m_projectGameIconBuffer{};
        std::array<int, 2> m_projectWindowSize{ 1280, 720 };
        bool m_projectSplashScreenDraft{ true };
        // プロジェクト設定で選択中のカテゴリー（0=ゲーム）。
        int m_projectSettingsCategory{};
        // ---- パッケージタブの状態 ----
        enum class PackageListState
        {
            NotLoaded,
            Loading,
            Ready,
            Failed
        };
        bool m_packagesPanelOpen{ false };
        PackageListState m_packageListState{
            PackageListState::NotLoaded
        };
        std::vector<PackageInfo> m_packages;
        std::string m_packagePanelError;
        int m_selectedPackageIndex{ -1 };
        // 取得またはインストールの実行中か（UIスレッド専用）。
        bool m_packageBusy{};
        std::thread m_packageWorker;
        // ワーカー→UIの受け渡し（m_packageResultMutexで保護）。
        struct PackageWorkerResult final
        {
            bool ready{};
            bool wasInstall{};
            std::string error;
            std::vector<PackageInfo> index;
            std::string installedDisplayName;
            bool installedHasScripts{};
        };
        std::mutex m_packageResultMutex;
        PackageWorkerResult m_packageWorkerResult;
        // インストール済みバージョンのキャッシュ（name→version）。
        std::unordered_map<std::string, std::string>
            m_installedPackageVersions;
        // ---- 自作パッケージ書き出しダイアログの状態 ----
        bool m_packageBuildDialogRequested{};
        std::array<char, 96> m_packageBuildNameBuffer{};
        std::array<char, 128>
            m_packageBuildDisplayNameBuffer{};
        std::array<char, 512>
            m_packageBuildDescriptionBuffer{};
        std::array<char, 96> m_packageBuildAuthorBuffer{};
        std::array<char, 32> m_packageBuildVersionBuffer{};
        std::string m_packageBuildError;
        std::string m_packageBuildIndexEntry;
        std::array<char, 96> m_editorPresetNameBuffer{};
        std::array<char, 128> m_playerPrefKeyBuffer{};
        std::array<char, 512> m_playerPrefValueBuffer{};
        std::array<char, 128> m_saveSlotBuffer{};
        std::array<char, 4096> m_saveJsonBuffer{
            '{', '}', '\0'
        };
        std::string m_selectedSaveSlot;
        int m_playerPrefType{};
        bool m_playerPrefBoolean{};
        std::string m_assetFolderDialogError;
        std::string m_assetFileDialogError;
        std::string m_assetDeleteScanError;
        std::string m_projectSettingsError;
        std::string m_materialInspectorError;
        std::string m_dataAssetInspectorError;
        // 編集中のデータアセット。保存するまでファイルへは
        // 書きません（Materialと同じ「下書き」方式です）。
        std::unique_ptr<nlohmann::json> m_dataAssetInspectorDraft;
        std::string m_dataAssetInspectorTypeName;
        // 「詳細設定 (JSON)」の編集欄。毎フレーム作り直すと入力中の
        // カーソルと競合するため、読み込み直したときだけ入れ直します。
        std::array<char, 8192> m_dataAssetJsonBuffer{};
        bool m_dataAssetInspectorDirty{};
        // 「新規データアセット」で選んでいる型名。
        std::string m_createDataAssetTypeName;
        // *.asset.jsonの相対パス（小文字）から型名を引く表です。
        // RefreshAssetsのときにまとめて読みます。
        std::unordered_map<std::string, std::string>
            m_dataAssetTypeByPath;
        std::vector<std::string> m_assetDeleteReferences;
        AssetDialogRequest m_assetDialogRequest{
            AssetDialogRequest::None
        };
        // ドロップ中にChildren()を変更すると、現在走査中のツリーと
        // ImGuiのTreeNode/TreePopの対応を壊すため、描画後まで保留します。
        struct PendingHierarchyParentChange final
        {
            GameObjectId moved{};
            // 0はシーンルートを表します。
            GameObjectId parent{};
            bool requested{};
        };
        // ヒエラルキーの並び替え要求。ドロップを受けた場所で即座に
        // 並べ替えると、ヒエラルキーを走査しているfor文の対象
        // （Scene::GameObjects()）を反復中に壊してしまうため、
        // 走査が終わってからまとめて適用します。
        struct PendingHierarchyReorder final
        {
            GameObjectId moved{};
            GameObjectId reference{};
            bool insertAfter{};
            // 参照と同じ親へ移してから並べるか（階層をまたぐ移動）。
            bool reparentToReferenceLevel{};
            bool requested{};
        };
        // 回転入力欄の編集中の値。回転の正本はクォータニオンで、
        // 同じ回転を表すオイラー角が複数あるため、変換した値を
        // そのまま表示すると編集中に数値が飛びます。編集中だけは
        // 入力された値を見せるために覚えておきます。
        GameObjectId m_rotationEditObjectId{};
        DirectX::XMFLOAT3 m_rotationEditDegrees{};
        bool m_rotationEditActive{};
        PendingHierarchyParentChange m_pendingHierarchyParentChange{};
        PendingHierarchyReorder m_pendingHierarchyReorder{};
        HierarchyContextAction m_hierarchyContextAction{
            HierarchyContextAction::None
        };
        ProjectSettings m_projectSettings;
        std::vector<ProjectMenuNode> m_projectMenus;
        std::vector<ProjectPanelDefinition> m_projectPanels;
        ProjectStageEditorState m_projectStageEditor;
        ProjectVehicleEditorState m_projectVehicleEditor;
        std::unique_ptr<BgmLoopPanel> m_bgmPanel;
        std::uint64_t m_projectMenuManifestHash{};
        bool m_projectMenuManifestSeen{};
        double m_lastProjectMenuScanAt{ -2.0 };
        GraphicsSettings m_projectGraphicsDraft;
        ViewportSettings m_projectViewportDraft;
        PhysicsSettings m_projectPhysicsDraft;
        LitMaterial m_materialInspectorDraft;
        std::vector<InputActionDefinition>
            m_projectInputActionsDraft;
        std::vector<std::string> m_projectTagsDraft;
        // プロジェクト設定「スクリプト」カテゴリー
        // （外部エディター選択）の状態。
        std::filesystem::path m_projectScriptEditorDraft;
        bool m_projectAutoBuildDraft{ true };
        bool m_projectStripShaderSourceDraft{};
        int m_projectInspectorDecimalsDraft{ 1 };
        std::vector<ScriptEditorOption>
            m_projectScriptEditorOptions;
        std::array<char, 64> m_newTagBuffer{};
        std::array<char, 64> m_projectNewTagBuffer{};
        RenderTarget m_sceneRenderTarget;
        RenderTarget m_gameRenderTarget;
        RenderTarget m_cameraPreviewRenderTarget;
        RenderTarget m_projectStagePreview;
        RenderTarget m_projectVehicleTopPreview;
        RenderTarget m_projectVehicleSidePreview;
        bool m_gameViewFixedResolution{};
        int m_gameViewResolutionWidth{ 1920 };
        int m_gameViewResolutionHeight{ 1080 };
        // 固定解像度の描画スケール（0.5/0.75/1.0）。固定解像度は
        // パネルより大きいことが多く、原寸のまま描くとFPSが解像度
        // なりに落ちます（実測: 1080pはパネル描画の約3倍）。縮めて
        // 描いてもアスペクトと「シーンの見え方」は同じなので、
        // レイアウトの原寸確認が要らない間は下げておけます。
        float m_gameViewResolutionScale{ 1.0f };
        DirectX::XMFLOAT3 m_sceneCameraPosition{ 0.0f, 1.8f, 7.0f };
        DirectX::XMFLOAT3 m_sceneCameraRotation{ -0.12f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 m_scene3DCameraPosition{ 0.0f, 1.8f, 7.0f };
        DirectX::XMFLOAT3 m_scene3DCameraRotation{ -0.12f, 0.0f, 0.0f };
        float m_sceneCameraFocusDistance{ 7.0f };
        float m_scene3DCameraFocusDistance{ 7.0f };
        float m_sceneOrthographicSize{ 10.0f };
        float m_sceneCameraSpeed{ 5.0f };
        float m_sceneCameraBoostMultiplier{ 3.0f };
        float m_sceneCameraLookSensitivity{ 1.0f };
        float m_sceneCameraZoomSensitivity{ 1.0f };
        DirectX::XMFLOAT2 m_viewportPosition{};
        DirectX::XMFLOAT2 m_viewportSize{};
        ViewportMode m_activeViewport{ ViewportMode::None };
        GizmoOperation m_gizmoOperation{ GizmoOperation::Translate };
        float m_gridSpacing{ 1.0f };
        float m_gridExtent{ 20.0f };
        float m_translationSnap{ 0.5f };
        float m_rotationSnap{ 15.0f };
        float m_scaleSnap{ 0.1f };
        std::vector<EditorSettingsPreset> m_editorPresets;
        std::size_t m_selectedEditorPreset{};
        std::string m_editorPresetError;
        bool m_gizmoLocal{};
        bool m_scene2DMode{};
        bool m_scene3DViewStored{};
        bool m_sceneOrthographic{};
        bool m_gridVisible{ true };
        // Scene Viewのデバッグ線を種類ごとに出し入れします。
        // どれも既定は表示（従来の見た目）です。
        bool m_colliderDebugVisible{ true };
        bool m_lightGizmosVisible{ true };
        bool m_cameraGizmosVisible{ true };
        bool m_viewCubeVisible{ true };
        bool m_snapEnabled{};
        bool m_assetGridView{ true };
        std::array<
            std::shared_ptr<const TextureAsset>,
            static_cast<std::size_t>(AssetIconKind::Count)>
            m_fileTypeIcons{};
        std::array<
            bool,
            static_cast<std::size_t>(AssetIconKind::Count)>
            m_fileTypeIconAttempted{};
        bool m_assetDirectoryTreeVisible{};
        bool m_assetDeleteAcknowledged{};
        bool m_materialInspectorLoaded{};
        bool m_materialInspectorDirty{};
        bool m_sceneViewportHovered{};
        bool m_viewCubeHovered{};
        bool m_transformGizmoHovered{};
        bool m_transformGizmoUsing{};
        bool m_transformGizmoMouseCaptured{};
        bool m_gizmoWasUsing{};
        TilemapTool m_tilemapTool{
            TilemapTool::Paint
        };
        std::uint32_t m_tilePaletteSelectedTile{};
        bool m_tilemapStrokeChanged{};
        std::size_t m_historyIndex{};
        GameObjectId m_selectedObjectId{};
        GameObjectId m_gizmoObjectId{};
        // Shaderのプロパティ宣言のキャッシュ（更新時刻で無効化）。
        struct CachedShaderProperties final
        {
            ShaderProperties properties;
            std::filesystem::file_time_type writeTime{};
        };
        std::unordered_map<
            std::wstring,
            CachedShaderProperties>
            m_shaderPropertiesCache;

        HANDLE m_gameModuleBuildProcess{};
        double m_gameModuleBuildStartedAt{};
        // スクリプト保存の自動ビルド用。最後に見たスクリプトの
        // 更新時刻と、変更を検知した時刻（0なら待機なし）です。
        // 変更が続いている間は待ち、静かになってからビルドします。
        std::filesystem::file_time_type
            m_lastSeenScriptWriteTime{};
        bool m_scriptWriteTimeInitialized{};
        double m_scriptChangeDetectedAt{};
        double m_lastScriptScanAt{};
        // 走査の実測コストから次回までの間隔を決めます。ローカルなら
        // 数msなので既定の0.5秒のまま、ネットワークドライブ上の大きな
        // プロジェクトでは走査自体が1秒近くかかり、固定間隔だと走査が
        // 終わる前に次が始まってエディターが常時ディスクを舐め続けます。
        double m_scriptScanIntervalSeconds{ 0.5 };
        bool m_scriptRebuildQueued{};
        std::vector<PendingScriptAttachment>
            m_pendingScriptAttachments;
        GameObjectId m_modelAnimationPreviewObjectId{};
        GameObjectId m_prefabStatusRootId{};
        double m_nextPrefabStatusRefresh{};
        bool m_prefabHasOverrides{};
        std::string m_prefabStatusError;
        std::vector<PrefabOverrideDisplay>
            m_prefabOverrides;
        std::filesystem::path
            m_animationTimelineClipPath;
        std::vector<TransformKeyframe>
            m_animationTimelineKeyframes;
        std::array<char, 128>
            m_animationTimelineName{};
        std::string m_animationTimelineError;
        Transform m_animationTimelineOriginalTransform;
        GameObjectId m_animationTimelineTargetId{};
        std::size_t m_animationTimelineSelectedKey{
            static_cast<std::size_t>(-1)
        };
        float m_animationTimelineDuration{ 1.0f };
        float m_animationTimelineTime{};
        bool m_animationTimelineLoop{ true };
        bool m_animationTimelineOpen{};
        bool m_animationTimelineDirty{};
        std::filesystem::path
            m_animatorGraphControllerPath;
        std::unique_ptr<nlohmann::json>
            m_animatorGraphDocument;
        std::string m_animatorGraphError;
        DirectX::XMFLOAT2 m_animatorGraphScrolling{};
        std::size_t m_animatorGraphSelectedState{
            static_cast<std::size_t>(-1)
        };
        bool m_animatorGraphOpen{};
        bool m_animatorGraphDirty{};
        std::string m_statusMessage;
        std::vector<LogEntry> m_consoleEntries;
        std::array<char, 256> m_consoleFilter{};
        std::uint64_t m_consoleLastSequence{};
        bool m_consoleShowInfo{ true };
        bool m_consoleShowWarning{ true };
        bool m_consoleShowError{ true };
        bool m_consolePaused{};
        bool m_consoleAutoScroll{ true };
        std::array<float, 120>
            m_performanceFrameTimes{};
        std::array<float, 120>
            m_performanceCpuTimes{};
        std::size_t m_performanceSampleIndex{};
        bool m_sceneEnvironmentOpen{};
        std::filesystem::path m_skyboxValidationPath;
        std::string m_skyboxValidationError;
        bool m_performancePanelOpen{ true };
        bool m_consolePanelOpen{ true };
        bool m_persistencePanelOpen{ true };
        bool m_assetBrowserPanelOpen{ true };
        bool m_tilePalettePanelOpen{ true };
        bool m_statusIsError{};
        bool m_playing{};
        // 再生中の一時停止。描画は続け、ゲームプレイの更新だけ止めます。
        bool m_paused{};
        // 「次のフレーム」の要求。Applicationが1回消費します。
        bool m_stepRequested{};
        bool m_safeMode{};
        // スクリーンショットモード（imagePathが空なら無効）。
        EditorScreenshotOptions m_screenshotRequest;
        // 撮影までのフレーム数え。intentの適用は1フレーム目
        // （ImGuiのレイアウトが立ち上がってから）に行います。
        std::uint32_t m_screenshotFrame{};
        // --show の「:bottom」指定。対象（設定の内容ペイン、
        // Inspector）を撮影まで毎フレーム末尾へスクロールします。
        bool m_screenshotScrollToBottom{};
        void ApplyScreenshotIntent();
        void CaptureScreenshotAndQuit();
        // リモート操作モード（--remote）。command.jsonの連番。
        std::uint64_t m_remoteLastSequence{};
        // このフレームで撮る依頼（空なら無し）。Renderの末尾で撮る。
        std::filesystem::path m_remotePendingShot;
        // 実行済みseqの結果をstate.jsonへ書くための控え。
        std::uint64_t m_remoteReportSequence{};
        bool m_remoteReportPending{};
        std::string m_remoteReportError;
        // dumpコマンド: state.jsonへUIの一覧を含める。
        bool m_remoteDumpPending{};
        // dumpの"all"指定（ラベル無しウィジェットも含める）。
        bool m_remoteDumpAll{};
        // ランタイム操作: InputSystemへ渡す一時的な入力スナップショット。
        std::optional<InputSnapshot> m_remoteInputSnapshot;
        std::uint32_t m_remoteInputFrames{};
        bool m_remoteRuntimePending{};
        // 複数フレームにまたがる入力手順（set-value / drag）。
        // frameはマクロ開始からの相対フレーム番号です。
        struct RemoteStep final
        {
            std::uint32_t frame{};
            std::function<void()> action;
        };
        std::vector<RemoteStep> m_remoteMacro;
        std::uint32_t m_remoteMacroFrame{};
        // 最後に注入したマウス位置（クライアント座標）。
        // ImGui_ImplWin32は毎フレーム実カーソル位置を報告するので、
        // 保持した位置を毎フレーム入れ直して上書きします。これにより
        // 実カーソルを動かさずに済み、操作中も人がそのPCを使えます。
        bool m_remoteMouseHeld{};
        float m_remoteMouseX{};
        float m_remoteMouseY{};
        void PollRemoteCommands();
        void WriteRemoteState();
        void RunRemoteMacro();
        [[nodiscard]] nlohmann::json
            BuildRemoteRuntimeState() const;
        // ImGuiのマウス位置をクライアント座標へ動かします
        // （実カーソルは動かしません）。
        void InjectMousePosition(float x, float y);
        // 保持中の注入位置をこのフレームのイベントキューへ入れ直します。
        void ReapplyRemoteMousePosition();
        inline static bool s_normalModeRestartRequested{};
        bool m_win32Initialized{};
        bool m_dx11Initialized{};
        bool m_projectSettingsDialogRequested{};
        std::string m_imguiIniPath;
        bool m_resetDockLayout{};
    };
}
