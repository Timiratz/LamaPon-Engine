#include "LamaPon/Editor/EditorLayer.h"

#include "LamaPon/Editor/EditorGuiRenderer.h"
#include "LamaPon/Editor/EditorLayerShared.h"
#include "LamaPon/Editor/DataAssetSchema.h"
#include "LamaPon/Editor/GameModuleBuilder.h"

#include "LamaPon/Animation/AnimatorController.h"
#include "LamaPon/Assets/AssetImporter.h"
#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Audio/AudioSystem.h"
#include "LamaPon/Components/AudioSourceComponent.h"
#include "LamaPon/Components/CameraComponent.h"
#include "LamaPon/Components/DirectionalLightComponent.h"
#include "LamaPon/Components/MeshRendererComponent.h"
#include "LamaPon/Components/ModelRendererComponent.h"
#include "LamaPon/Components/NativeScriptComponent.h"
#include "LamaPon/Components/ParticleSystemComponent.h"
#include "LamaPon/Components/SpriteRendererComponent.h"
#include "LamaPon/Components/TilemapComponent.h"
#include "LamaPon/Components/TransformAnimatorComponent.h"
#include "LamaPon/Components/UIButtonComponent.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/LitMaterialAsset.h"
#include "LamaPon/Scene/Scene.h"
#include "LamaPon/Scene/SceneManager.h"
#include "LamaPon/Scripting/GameModuleHost.h"

#include <commdlg.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

using namespace LamaPon::EditorDetail;

namespace
{
    // SDKとランタイムのAPI不一致を示す診断文か調べます(diagnostic: ビルド・読込の診断文)。
    [[nodiscard]] bool IsSdkRuntimeMismatch(
        const std::string_view diagnostic) noexcept
    {
        return diagnostic.find(
                   "The installed Game Module SDK is API ")
                != std::string_view::npos
            || diagnostic.find(
                   "Game Module was built for API version ")
                != std::string_view::npos;
    }

    // ファイルの全内容を読み、開けない場合は空文字列を返します(path: 読込先)。
    [[nodiscard]] std::string ReadTextFile(
        const std::filesystem::path& path)
    {
        // 全内容を読む入力ストリーム
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            return {};
        }
        return {
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{}
        };
    }

    // 通常ファイルかを調べ、ファイルシステムのエラー時はfalseを返します(path: 確認先)。
    [[nodiscard]] bool IsRegularFile(
        const std::filesystem::path& path) noexcept
    {
        // ファイル種別の確認エラー
        std::error_code error;
        return std::filesystem::is_regular_file(path, error) && !error;
    }

    // 削除警告と移動・改名に使うPBRテクスチャの借用参照です。
    struct PbrMapReference final
    {
        // PBRマップの表示名
        const char* label;
        // 描画コンポーネント内のパス参照
        const std::filesystem::path* path;
    };

    // 粗さ・金属度・遮蔽・発光のパス参照を返します(renderer: 参照元・返却参照より長寿命)。
    template<typename Renderer>
    std::array<PbrMapReference, 4> PbrMapReferences(
        const Renderer& renderer) noexcept
    {
        return {
            PbrMapReference{
                "粗さマップ",
                &renderer.RoughnessTexturePath()
            },
            PbrMapReference{
                "金属度マップ",
                &renderer.MetallicTexturePath()
            },
            PbrMapReference{
                "遮蔽マップ",
                &renderer.OcclusionTexturePath()
            },
            PbrMapReference{
                "発光マップ",
                &renderer.EmissiveTexturePath()
            }
        };
    }

    // PBRテクスチャの取得・設定用メンバー関数です。
    template<typename Renderer>
    struct PbrMapAccessor final
    {
        // PBRマップの表示名
        const char* label;
        // テクスチャパスを取得する関数
        const std::filesystem::path& (Renderer::*get)()
            const noexcept;
        // テクスチャパスを設定する関数
        void (Renderer::*set)(std::filesystem::path);
    };

    // 粗さ・金属度・遮蔽・発光の取得・設定メンバー関数を返します。
    template<typename Renderer>
    std::array<PbrMapAccessor<Renderer>, 4>
        PbrMapAccessors() noexcept
    {
        return {
            PbrMapAccessor<Renderer>{
                "粗さマップ",
                &Renderer::RoughnessTexturePath,
                &Renderer::SetRoughnessTexturePath
            },
            PbrMapAccessor<Renderer>{
                "金属度マップ",
                &Renderer::MetallicTexturePath,
                &Renderer::SetMetallicTexturePath
            },
            PbrMapAccessor<Renderer>{
                "遮蔽マップ",
                &Renderer::OcclusionTexturePath,
                &Renderer::SetOcclusionTexturePath
            },
            PbrMapAccessor<Renderer>{
                "発光マップ",
                &Renderer::EmissiveTexturePath,
                &Renderer::SetEmissiveTexturePath
            }
        };
    }

    // 先頭が英字か_、以降が英数字か_で構成される名前かを調べます(value: 検証する識別子)。
    bool IsCppIdentifier(const std::string_view value)
    {
        if (value.empty()
            || !(std::isalpha(static_cast<unsigned char>(value.front()))
                || value.front() == '_'))
        {
            return false;
        }
        // 残りの文字を検証します(character: 識別子の構成文字)。
        return std::ranges::all_of(
            value.substr(1),
            [](const unsigned char character)
            {
                return std::isalnum(character) || character == '_';
            });
    }

    // クラス名を埋め込んだ新規ScriptのC++ソースを返します(className: 検証済みのクラス名)。
    std::string CreateCppScriptSource(const std::string_view className)
    {
        // 生成するScriptのクラス名
        const std::string name{ className };
        // クラス名を置換するScriptひな形
        std::string source = R"LAMAPON(#include "LamaPon/LamaPon.h"

class __SCRIPT_NAME__ final : public LamaPon::Script
{
public:
    // 最初のフレーム前に1回だけ初期化処理を行います。
    void Start() override
    {

    }

    // 毎フレームの更新処理を行います(deltaTime: 前フレームからの経過秒)。
    void Update(const float deltaTime) override
    {

        (void)deltaTime;
    }
};

LAMAPON_SCRIPT(__SCRIPT_NAME__);
)LAMAPON";
        // クラス名を置換する目印
        constexpr std::string_view marker = "__SCRIPT_NAME__";
        // 次のクラス名置換の検索位置
        std::size_t position{};
        while ((position = source.find(marker, position))
            != std::string::npos)
        {
            source.replace(position, marker.size(), name);
            position += name.size();
        }
        return source;
    }

    // Windowsの名前制約とUTF-8を検証し、不正時は理由、正常ならnulloptを返します(name: 120byte以下の名前, entryLabel: エラーに使う項目名)。
    std::optional<std::string> ValidateAssetEntryName(
        const std::string_view name,
        const std::string_view entryLabel)
    {
        if (name.empty())
        {
            return std::string{ entryLabel } + "を入力してください";
        }
        if (name == "." || name == "..")
        {
            return "「.」と「..」はフォルダー名に使用できません";
        }
        if (name.size() > 120)
        {
            return std::string{ entryLabel } + "が長すぎます";
        }
        if (name.back() == ' ' || name.back() == '.')
        {
            return "末尾に空白またはピリオドは使用できません";
        }

        // Windows名で禁止する文字
        constexpr std::string_view invalidCharacters = "<>:\"/\\|?*";
        // 名前の構成文字
        for (const unsigned char character : name)
        {
            if (character < 32
                || invalidCharacters.find(
                    static_cast<char>(character)) != std::string_view::npos)
            {
                return std::string{ entryLabel }
                    + "に使用できない文字が含まれています";
            }
        }

        // 最初の拡張子区切り
        const auto dot = name.find('.');
        // 予約名を調べる小文字の幹
        const std::string baseName = Lowercase(
            std::string{ name.substr(0, dot) });
        // Windowsの予約デバイス名
        constexpr std::array reservedNames{
            "con", "prn", "aux", "nul",
            "com1", "com2", "com3", "com4", "com5",
            "com6", "com7", "com8", "com9",
            "lpt1", "lpt2", "lpt3", "lpt4", "lpt5",
            "lpt6", "lpt7", "lpt8", "lpt9"
        };
        if (std::ranges::find(reservedNames, baseName)
            != reservedNames.end())
        {
            return "Windowsの予約名は使用できません";
        }

        try
        {
            if (LamaPon::PathFromUtf8(name).empty())
            {
                return std::string{ entryLabel }
                    + "をUTF-8として解釈できません";
            }
        }
        catch (const std::exception&)
        {
            return std::string{ entryLabel }
                + "をUTF-8として解釈できません";
        }
        return std::nullopt;
    }

    // 字句上で旧パス配下の接頭辞を置換し、対象外ならnulloptを返します(value: 変更候補, oldPrefix: 旧接頭辞, newPrefix: 新接頭辞)。
    std::optional<std::filesystem::path> RemapPathPrefix(
        const std::filesystem::path& value,
        const std::filesystem::path& oldPrefix,
        const std::filesystem::path& newPrefix)
    {
        if (value.empty())
        {
            return std::nullopt;
        }
        if (value == oldPrefix)
        {
            return newPrefix;
        }

        // 旧接頭辞からの相対パス
        const auto suffix = value.lexically_relative(oldPrefix);
        if (suffix.empty()
            || suffix.is_absolute()
            || (*suffix.begin() == ".."))
        {
            return std::nullopt;
        }
        return newPrefix / suffix;
    }
}

namespace LamaPon
{
    // assetsルートの操作と直下のフォルダーツリーを描画します。
    void EditorLayer::DrawAssetDirectoryTree()
    {
        // ルートノードの表示設定
        ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_DefaultOpen
            | ImGuiTreeNodeFlags_OpenOnArrow
            | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (m_assetDirectory.empty())
        {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        if (m_assetDirectories.empty())
        {
            flags |=
                ImGuiTreeNodeFlags_Leaf
                | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }

        // ルートノードが展開中か
        const bool open = ImGui::TreeNodeEx(
            "##AssetRoot",
            flags,
            "assets");
        if (ImGui::IsItemClicked())
        {
            m_assetDirectory.clear();
            m_selectedAsset.clear();
        }
        DrawAssetDirectoryContextMenu({}, true);
        AcceptAssetMoveDrop({});

        if (open && !m_assetDirectories.empty())
        {
            // ルート直下のフォルダー候補
            for (const auto& directory : m_assetDirectories)
            {
                if (directory.parent_path().empty())
                {
                    DrawAssetDirectoryNode(directory);
                }
            }
            ImGui::TreePop();
        }
    }

    // フォルダーの選択・移動・メニューと子ツリーを描画します(directory: アセット相対フォルダー)。
    void EditorLayer::DrawAssetDirectoryNode(
        const std::filesystem::path& directory)
    {
        // 子フォルダーがあるか調べます(candidate: 子フォルダー候補)。
        const bool hasChildren = std::ranges::any_of(
            m_assetDirectories,
            [&directory](const std::filesystem::path& candidate)
            {
                return candidate.parent_path() == directory;
            });

        // フォルダーノードの表示設定
        ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_OpenOnArrow
            | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (!hasChildren)
        {
            flags |=
                ImGuiTreeNodeFlags_Leaf
                | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }
        if (m_assetDirectory == directory)
        {
            flags |= ImGuiTreeNodeFlags_Selected;
        }

        // フォルダーノードのImGui ID
        const std::string id = PathToUtf8(directory);
        // フォルダーの表示名
        const std::string name = PathToUtf8(directory.filename());
        ImGui::PushID(id.c_str());
        // フォルダーノードが展開中か
        const bool open = ImGui::TreeNodeEx(
            "##AssetDirectory",
            flags,
            "%s",
            name.c_str());
        if (ImGui::IsItemClicked())
        {
            m_assetDirectory = directory;
            m_selectedAsset.clear();
        }
        DrawAssetDirectoryContextMenu(directory, false);
        BeginAssetFolderDragSource(directory);
        AcceptAssetMoveDrop(directory);

        if (open && hasChildren)
        {
            // 表示する子フォルダー候補
            for (const auto& child : m_assetDirectories)
            {
                if (child.parent_path() == directory)
                {
                    DrawAssetDirectoryNode(child);
                }
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    // アセットの種類と選択対象に応じた操作メニューを描画します(asset: 操作する相対ファイル)。
    void EditorLayer::DrawAssetFileContextMenu(
        const std::filesystem::path& asset)
    {
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            m_selectedAsset = asset;
        }
        if (!ImGui::BeginPopupContextItem())
        {
            return;
        }

        // アセットを割り当てる選択対象
        const auto* selectedObject =
            m_scene.FindGameObject(m_selectedObjectId);
        // 選択対象へ画像を割当可能か
        const bool canAssignTexture =
            selectedObject != nullptr
            && (selectedObject->GetComponent<
                    SpriteRendererComponent>() != nullptr
                || selectedObject->GetComponent<
                    MeshRendererComponent>() != nullptr
                || selectedObject->GetComponent<
                    ModelRendererComponent>() != nullptr
                || selectedObject->GetComponent<
                    TilemapComponent>() != nullptr
                || selectedObject->GetComponent<
                    ParticleSystemComponent>() != nullptr
                || selectedObject->GetComponent<
                    UIButtonComponent>() != nullptr)
            && IsTextureAsset(asset);
        // 選択対象へモデルを割当可能か
        const bool canAssignModel =
            selectedObject != nullptr
            && selectedObject->GetComponent<ModelRendererComponent>() != nullptr
            && IsModelAsset(asset);
        // 選択対象へ材質を割当可能か
        const bool canAssignMaterial =
            selectedObject != nullptr
            && (selectedObject->GetComponent<
                    MeshRendererComponent>() != nullptr
                || selectedObject->GetComponent<
                    ModelRendererComponent>() != nullptr)
            && IsMaterialAsset(asset);
        // 選択対象へClipを割当可能か
        const bool canAssignAnimation =
            selectedObject != nullptr
            && selectedObject->GetComponent<
                TransformAnimatorComponent>() != nullptr
            && IsAnimationAsset(asset);
        // 選択対象へ制御を割当可能か
        const bool canAssignAnimatorController =
            selectedObject != nullptr
            && (selectedObject->GetComponent<
                    TransformAnimatorComponent>() != nullptr
                || selectedObject->GetComponent<
                    ModelRendererComponent>() != nullptr)
            && IsAnimatorControllerAsset(asset);

        // 種類別の操作を表示したか
        bool hasAssetAction = false;
        if (IsCppScriptAsset(asset))
        {
            hasAssetAction = true;
            if (ImGui::MenuItem(
                "コードエディターで開く",
                nullptr,
                false,
                !m_playing))
            {
                m_selectedAsset = asset;
                OpenCodeAsset(asset);
            }
            if (ImGui::MenuItem(
                "Game Moduleをビルド",
                nullptr,
                false,
                !m_playing))
            {
                static_cast<void>(BuildGameModule());
            }
        }
        if (IsOpenableShaderAsset(asset))
        {
            hasAssetAction = true;
            if (ImGui::MenuItem(
                "Shaderをコードエディターで開く",
                nullptr,
                false,
                !m_playing))
            {
                m_selectedAsset = asset;
                OpenCodeAsset(asset);
            }
        }
        if (IsSceneAsset(asset))
        {
            hasAssetAction = true;
            if (ImGui::MenuItem(
                "シーンを開く",
                nullptr,
                false,
                !m_playing))
            {
                m_selectedAsset = asset;
                OpenSelectedAsset();
            }
            // 対象シーンを追加読込済みか
            const bool alreadyLoaded =
                m_scene.FindAdditiveScene(
                    m_scene.Scenes().
                        ResolveScenePath(asset))
                    != Scene::PrimarySceneHandle();
            if (ImGui::MenuItem(
                "シーンを追加読み込み（Additive）",
                nullptr,
                false,
                !m_playing && !alreadyLoaded))
            {
                if (m_scene.Scenes().
                    RequestLoadAdditive(asset))
                {
                    SetStatus(
                        "追加読み込みを要求しました: "
                        + PathToUtf8(asset));
                }
                else
                {
                    SetStatus(
                        "追加読み込みできませんでした: "
                        + m_scene.Scenes().LastError());
                }
            }
        }
        if (IsPrefabAsset(asset))
        {
            hasAssetAction = true;
            if (ImGui::MenuItem(
                "Prefabを配置",
                nullptr,
                false,
                !m_playing))
            {
                m_selectedAsset = asset;
                InstantiateSelectedPrefab();
            }

            if (ImGui::MenuItem(
                "シーン内のインスタンスを選択"))
            {
                SelectPrefabInstances(asset);
            }
        }
        if (IsTextureAsset(asset))
        {
            hasAssetAction = true;
            if (ImGui::MenuItem(
                "画像を割り当て",
                nullptr,
                false,
                !m_playing && canAssignTexture))
            {
                m_selectedAsset = asset;
                AssignSelectedTexture();
            }
        }
        if (IsModelAsset(asset))
        {
            hasAssetAction = true;
            if (ImGui::MenuItem(
                "モデルを割り当て",
                nullptr,
                false,
                !m_playing && canAssignModel))
            {
                m_selectedAsset = asset;
                AssignSelectedModel();
            }
        }
        if (IsMaterialAsset(asset))
        {
            hasAssetAction = true;
            if (ImGui::MenuItem(
                "Materialを割り当て",
                nullptr,
                false,
                !m_playing && canAssignMaterial))
            {
                m_selectedAsset = asset;
                AssignSelectedMaterial();
            }
        }
        if (IsAnimationAsset(asset))
        {
            hasAssetAction = true;
            if (ImGui::MenuItem(
                "Animationを割り当て",
                nullptr,
                false,
                !m_playing && canAssignAnimation))
            {
                m_selectedAsset = asset;
                AssignSelectedAnimation();
            }
        }
        if (IsAnimatorControllerAsset(asset))
        {
            hasAssetAction = true;
            if (ImGui::MenuItem(
                "Animator Controllerを開く",
                nullptr,
                false,
                !m_playing))
            {
                m_selectedAsset = asset;
                OpenAnimatorControllerGraph(asset);
            }
            if (ImGui::MenuItem(
                "Animator Controllerを割り当て",
                nullptr,
                false,
                !m_playing
                    && canAssignAnimatorController))
            {
                m_selectedAsset = asset;
                AssignSelectedAnimatorController();
            }
        }

        if (hasAssetAction)
        {
            ImGui::Separator();
        }
        if (ImGui::MenuItem(
            "このアセットを再インポート",
            nullptr,
            false,
            !m_playing
                && m_gameModuleBuildProcess == nullptr))
        {
            m_selectedAsset = asset;
            ReimportSelectedAsset();
        }
        if (ImGui::MenuItem(
            "名前変更",
            nullptr,
            false,
            !m_playing))
        {
            OpenRenameAssetFileDialog(asset);
        }
        if (ImGui::MenuItem(
            "削除",
            nullptr,
            false,
            !m_playing))
        {
            OpenDeleteAssetFileDialog(asset);
        }

        ImGui::Separator();
        // 参照するアセットDBの記録
        if (const auto* record =
                m_graphics.Assets().Database().
                    FindByPath(asset);
            record != nullptr
                && ImGui::BeginMenu("アセット情報"))
        {
            ImGui::TextDisabled(
                "GUID: %s",
                record->guid.c_str());
            ImGui::TextDisabled(
                "Importer: %s",
                record->importer.c_str());
            ImGui::TextDisabled(
                "依存: %zu  /  被依存: %zu",
                record->dependencies.size(),
                record->dependents.size());
            if (ImGui::MenuItem("GUIDをコピー"))
            {
                ImGui::SetClipboardText(
                    record->guid.c_str());
                SetStatus("GUIDをコピーしました");
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("相対パスをコピー"))
        {
            // コピーするアセット相対パス
            const std::string path = PathToUtf8(asset);
            ImGui::SetClipboardText(path.c_str());
            SetStatus("相対パスをコピーしました: " + path);
        }
        if (ImGui::MenuItem("Explorerで表示"))
        {
            OpenAssetInExplorer(asset, true);
        }

        ImGui::EndPopup();
    }

    // 表示フォルダーを変更せず操作メニューを開きます(directory: 操作する相対フォルダー, isRoot: assetsルートか)。
    void EditorLayer::DrawAssetDirectoryContextMenu(
        const std::filesystem::path& directory,
        const bool isRoot)
    {
        // 表示先を変える操作は「開く」に限り、メニュー表示だけでは変更しません。
        if (!ImGui::BeginPopupContextItem())
        {
            return;
        }

        DrawAssetDirectoryMenuContents(
            directory,
            isRoot);
        ImGui::EndPopup();
    }

    // 作成・取込・移動先表示とフォルダー操作を提供します(directory: 操作する相対フォルダー, isRoot: assetsルートか)。
    void EditorLayer::DrawAssetDirectoryMenuContents(
        const std::filesystem::path& directory,
        const bool isRoot)
    {
        if (ImGui::MenuItem("開く"))
        {
            m_assetDirectory = directory;
            m_selectedAsset.clear();
        }
        ImGui::Separator();
        if (ImGui::BeginMenu(
            "作成",
            !m_playing
                && m_gameModuleBuildProcess == nullptr))
        {
            DrawCreateAssetMenuContents(directory);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(
            "新しいアセットをインポート...",
            nullptr,
            false,
            !m_playing
                && m_gameModuleBuildProcess == nullptr))
        {
            m_assetDirectory = directory;
            m_selectedAsset.clear();
            OpenImportAssetsDialog();
        }
        if (ImGui::MenuItem(
            "Game Moduleをビルド",
            nullptr,
            false,
            !m_playing))
        {
            static_cast<void>(BuildGameModule());
        }
        ImGui::Separator();

        if (ImGui::MenuItem(
            "名前変更",
            nullptr,
            false,
            !m_playing && !isRoot))
        {
            OpenRenameAssetFolderDialog(directory);
        }
        if (ImGui::MenuItem(
            "削除",
            nullptr,
            false,
            !m_playing && !isRoot))
        {
            OpenDeleteAssetFolderDialog(directory);
        }

        ImGui::Separator();
        if (ImGui::MenuItem("相対パスをコピー"))
        {
            // コピーするフォルダー相対パス
            const std::string path = isRoot
                ? "assets"
                : PathToUtf8(directory);
            ImGui::SetClipboardText(path.c_str());
            SetStatus("相対パスをコピーしました: " + path);
        }
        if (ImGui::MenuItem("Explorerで開く"))
        {
            OpenAssetInExplorer(directory, false);
        }
    }

    // 編集可能な間だけ新規アセット作成の予約を受け付けます(directory: 作成先の相対フォルダー)。
    void EditorLayer::DrawCreateAssetMenuContents(
        const std::filesystem::path& directory)
    {
        if (ImGui::MenuItem(
            "フォルダー",
            nullptr,
            false,
            !m_playing
                && m_gameModuleBuildProcess == nullptr))
        {
            OpenCreateAssetFolderDialog(directory);
        }
        if (ImGui::MenuItem(
            "シーン",
            nullptr,
            false,
            !m_playing
                && m_gameModuleBuildProcess == nullptr))
        {
            OpenCreateSceneDialog(directory);
        }
        if (ImGui::MenuItem(
            "Lit Material",
            nullptr,
            false,
            !m_playing
                && m_gameModuleBuildProcess == nullptr))
        {
            OpenCreateMaterialDialog(directory);
        }
        if (ImGui::MenuItem(
            "データアセット",
            nullptr,
            false,
            !m_playing
                && m_gameModuleBuildProcess == nullptr))
        {
            OpenCreateDataAssetDialog(directory);
        }
        if (ImGui::IsItemHovered(
                ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip(
                "GameObjectへ付けずに持てるデータです"
                "（型はGame ModuleのLAMAPON_DATA_ASSETで宣言します）");
        }
        if (ImGui::MenuItem(
            "カスタムShader",
            nullptr,
            false,
            !m_playing
                && m_gameModuleBuildProcess == nullptr))
        {
            OpenCreateShaderDialog(directory);
        }
        if (ImGui::MenuItem(
            "C++ Script",
            nullptr,
            false,
            !m_playing
                && m_gameModuleBuildProcess == nullptr))
        {
            OpenCreateCppScriptDialog(directory);
        }
    }

    // フォルダーの作成ダイアログを予約します(parentDirectory: 作成先の相対フォルダー)。
    void EditorLayer::OpenCreateAssetFolderDialog(
        const std::filesystem::path& parentDirectory)
    {
        m_assetDialogTarget = parentDirectory;
        m_assetDialogRequest = AssetDialogRequest::CreateFolder;
    }

    // シーンの作成ダイアログを予約します(parentDirectory: 作成先の相対フォルダー)。
    void EditorLayer::OpenCreateSceneDialog(
        const std::filesystem::path& parentDirectory)
    {
        m_assetDialogTarget = parentDirectory;
        m_assetDialogRequest = AssetDialogRequest::CreateScene;
    }

    // マテリアルの作成ダイアログを予約します(parentDirectory: 作成先の相対フォルダー)。
    void EditorLayer::OpenCreateMaterialDialog(
        const std::filesystem::path& parentDirectory)
    {
        m_assetDialogTarget = parentDirectory;
        m_assetDialogRequest = AssetDialogRequest::CreateMaterial;
    }

    // データアセットの作成ダイアログを予約します(parentDirectory: 作成先の相対フォルダー)。
    void EditorLayer::OpenCreateDataAssetDialog(
        const std::filesystem::path& parentDirectory)
    {
        m_assetDialogTarget = parentDirectory;
        m_assetDialogRequest =
            AssetDialogRequest::CreateDataAsset;
    }

    // Shaderの作成ダイアログを予約します(parentDirectory: 作成先の相対フォルダー)。
    void EditorLayer::OpenCreateShaderDialog(
        const std::filesystem::path& parentDirectory)
    {
        m_assetDialogTarget = parentDirectory;
        m_assetDialogRequest = AssetDialogRequest::CreateShader;
    }

    // C++Scriptの作成ダイアログを予約します(parentDirectory: 作成先の相対フォルダー)。
    void EditorLayer::OpenCreateCppScriptDialog(
        const std::filesystem::path& parentDirectory)
    {
        m_assetDialogTarget = parentDirectory;
        m_assetDialogRequest = AssetDialogRequest::CreateCppScript;
    }

    // フォルダー名変更のダイアログを予約します(directory: 操作対象の相対パス)。
    void EditorLayer::OpenRenameAssetFolderDialog(
        const std::filesystem::path& directory)
    {
        m_assetDialogTarget = directory;
        m_assetDialogRequest = AssetDialogRequest::RenameFolder;
    }

    // フォルダー削除のダイアログを予約します(directory: 操作対象の相対パス)。
    void EditorLayer::OpenDeleteAssetFolderDialog(
        const std::filesystem::path& directory)
    {
        m_assetDialogTarget = directory;
        m_assetDialogRequest = AssetDialogRequest::DeleteFolder;
    }

    // ファイル名変更のダイアログを予約します(asset: 操作対象の相対パス)。
    void EditorLayer::OpenRenameAssetFileDialog(
        const std::filesystem::path& asset)
    {
        m_assetDialogTarget = asset;
        m_assetDialogRequest = AssetDialogRequest::RenameFile;
    }

    // ファイル削除のダイアログを予約します(asset: 操作対象の相対パス)。
    void EditorLayer::OpenDeleteAssetFileDialog(
        const std::filesystem::path& asset)
    {
        m_assetDialogTarget = asset;
        m_assetDialogRequest = AssetDialogRequest::DeleteFile;
    }

    // 予約した操作の編集欄を初期化してPopupを開き、予約を消費します。
    void EditorLayer::OpenPendingAssetDialog()
    {
        // 予約したアセット操作のPopupを開きます。
        switch (m_assetDialogRequest)
        {
        case AssetDialogRequest::CreateFolder:
            m_assetDirectory = m_assetDialogTarget;
            m_selectedAsset.clear();
            strncpy_s(
                m_assetFolderNameBuffer.data(),
                m_assetFolderNameBuffer.size(),
                "NewFolder",
                _TRUNCATE);
            m_assetFolderDialogError.clear();
            ImGui::OpenPopup("フォルダーを作成");
            break;
        case AssetDialogRequest::CreateScene:
            m_assetDirectory = m_assetDialogTarget;
            m_selectedAsset.clear();
            strncpy_s(
                m_assetFileNameBuffer.data(),
                m_assetFileNameBuffer.size(),
                "NewScene.scene.json",
                _TRUNCATE);
            m_assetFileDialogError.clear();
            ImGui::OpenPopup("シーンを作成");
            break;
        case AssetDialogRequest::CreateMaterial:
            m_assetDirectory = m_assetDialogTarget;
            m_selectedAsset.clear();
            strncpy_s(
                m_assetFileNameBuffer.data(),
                m_assetFileNameBuffer.size(),
                "NewMaterial.material.json",
                _TRUNCATE);
            m_assetFileDialogError.clear();
            ImGui::OpenPopup("Lit Materialを作成");
            break;
        case AssetDialogRequest::CreateDataAsset:
        {
            m_assetDirectory = m_assetDialogTarget;
            m_selectedAsset.clear();
            strncpy_s(
                m_assetFileNameBuffer.data(),
                m_assetFileNameBuffer.size(),
                "NewData.asset.json",
                _TRUNCATE);
            m_assetFileDialogError.clear();
            // 作成可能なデータ型の登録元
            const auto* host = GameModuleHost::Current();
            if (m_createDataAssetTypeName.empty()
                && host != nullptr
                && host->RegisteredDataAssets().size() == 1)
            {
                m_createDataAssetTypeName =
                    host->RegisteredDataAssets()
                        .front()
                        .typeName;
            }
            ImGui::OpenPopup("データアセットを作成");
            break;
        }
        case AssetDialogRequest::CreateShader:
            m_assetDirectory = m_assetDialogTarget;
            m_selectedAsset.clear();
            m_createShaderFromGraph = true;
            m_createShaderGraph = {};
            strncpy_s(
                m_assetFileNameBuffer.data(),
                m_assetFileNameBuffer.size(),
                "NewCustomShader.hlsl",
                _TRUNCATE);
            m_assetFileDialogError.clear();
            ImGui::OpenPopup("カスタムShaderを作成");
            break;
        case AssetDialogRequest::CreateCppScript:
            m_assetDirectory = m_assetDialogTarget;
            m_selectedAsset.clear();
            strncpy_s(
                m_assetFileNameBuffer.data(),
                m_assetFileNameBuffer.size(),
                "NewScript.cpp",
                _TRUNCATE);
            m_assetFileDialogError.clear();
            ImGui::OpenPopup("C++ Scriptを作成");
            break;
        case AssetDialogRequest::RenameFolder:
        {
            m_assetDirectory = m_assetDialogTarget;
            m_selectedAsset.clear();
            // 名前変更欄へ入れる現在名
            const std::string currentName =
                PathToUtf8(m_assetDirectory.filename());
            strncpy_s(
                m_assetFolderNameBuffer.data(),
                m_assetFolderNameBuffer.size(),
                currentName.c_str(),
                _TRUNCATE);
            m_assetFolderDialogError.clear();
            ImGui::OpenPopup("フォルダー名を変更");
            break;
        }
        case AssetDialogRequest::DeleteFolder:
            m_assetDirectory = m_assetDialogTarget;
            m_selectedAsset.clear();
            m_assetFolderDialogError.clear();
            ImGui::OpenPopup("フォルダーを削除");
            break;
        case AssetDialogRequest::RenameFile:
        {
            m_selectedAsset = m_assetDialogTarget;
            // 名前変更欄へ入れる現在名
            const std::string currentName =
                PathToUtf8(m_selectedAsset.filename());
            strncpy_s(
                m_assetFileNameBuffer.data(),
                m_assetFileNameBuffer.size(),
                currentName.c_str(),
                _TRUNCATE);
            m_assetFileDialogError.clear();
            ImGui::OpenPopup("ファイル名を変更");
            break;
        }
        case AssetDialogRequest::DeleteFile:
            m_selectedAsset = m_assetDialogTarget;
            m_assetFileDialogError.clear();
            m_assetDeleteScanError.clear();
            m_assetDeleteAcknowledged = false;
            RefreshAssetDeleteReferences();
            ImGui::OpenPopup("ファイルを削除");
            break;
        case AssetDialogRequest::None:
            return;
        }

        m_assetDialogRequest = AssetDialogRequest::None;
        m_assetDialogTarget.clear();
    }

    // assets内の存在する対象をExplorerで開き、失敗を通知します(asset: 相対パス, selectFile: ファイルを選択表示するか)。
    void EditorLayer::OpenAssetInExplorer(
        const std::filesystem::path& asset,
        const bool selectFile)
    {
        try
        {
            // 実体を解決したアセットルート
            const auto root = std::filesystem::weakly_canonical(
                m_graphics.Assets().AssetRoot());
            // 実体を解決した表示対象
            const auto resolved = std::filesystem::weakly_canonical(
                m_graphics.Assets().ResolvePath(asset));
            if (!IsPathWithin(root, resolved)
                || !std::filesystem::exists(resolved))
            {
                SetStatus(
                    "Explorerで表示できるアセットが見つかりません",
                    true);
                return;
            }

            // Explorer起動の結果
            HINSTANCE result{};
            if (selectFile)
            {
                // Explorerの選択表示引数
                const std::wstring parameters =
                    L"/select,\"" + resolved.wstring() + L"\"";
                result = ShellExecuteW(
                    m_window,
                    L"open",
                    L"explorer.exe",
                    parameters.c_str(),
                    nullptr,
                    SW_SHOWNORMAL);
            }
            else
            {
                result = ShellExecuteW(
                    m_window,
                    L"open",
                    resolved.c_str(),
                    nullptr,
                    nullptr,
                    SW_SHOWNORMAL);
            }

            if (reinterpret_cast<INT_PTR>(result) <= 32)
            {
                SetStatus("Explorerを開けませんでした", true);
            }
        }
        // 外部アプリを開く処理の例外
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    // 設定したエディターまたは既定の関連付けでコードを開きます(asset: 相対コードパス, line: 行番号・0は未指定, column: 列番号・0は未指定)。
    void EditorLayer::OpenCodeAsset(
        const std::filesystem::path& asset,
        const std::uint32_t line,
        const std::uint32_t column)
    {
        if (!IsCppScriptAsset(asset)
            && !IsOpenableShaderAsset(asset))
        {
            return;
        }

        try
        {
            // 実体を解決したアセットルート
            const auto root = std::filesystem::weakly_canonical(
                m_graphics.Assets().AssetRoot());
            // 実体を解決したコードパス
            const auto resolved = std::filesystem::weakly_canonical(
                m_graphics.Assets().ResolvePath(asset));
            if (!IsPathWithin(root, resolved)
                || !std::filesystem::is_regular_file(resolved))
            {
                SetStatus(
                    "コードエディターで開けるファイルが見つかりません",
                    true);
                return;
            }

            // 設定済みの外部エディター
            const auto& editor =
                m_projectSettings.scriptEditorPath;
            // コードエディター起動の結果
            HINSTANCE result{};
            if (!editor.empty()
                && std::filesystem::is_regular_file(editor))
            {
                // コードを開く位置指定引数
                const std::wstring parameters =
                    BuildScriptEditorArguments(
                        editor,
                        resolved,
                        line,
                        column);
                result = ShellExecuteW(
                    m_window,
                    L"open",
                    editor.c_str(),
                    parameters.c_str(),
                    resolved.parent_path().c_str(),
                    SW_SHOWNORMAL);
            }
            else
            {
                result = ShellExecuteW(
                    m_window,
                    L"open",
                    resolved.c_str(),
                    nullptr,
                    resolved.parent_path().c_str(),
                    SW_SHOWNORMAL);
            }
            if (reinterpret_cast<INT_PTR>(result) <= 32)
            {
                SetStatus(
                    "コードを開けませんでした。プロジェクト設定のスクリプトエディターを確認してください",
                    true);
                return;
            }
            SetStatus(
                "コードを開きました: "
                + PathToUtf8(asset)
                + (line == 0
                    ? std::string{}
                    : " (" + std::to_string(line)
                        + ":" + std::to_string(
                            std::max(column, 1u)) + ")"));
        }
        // 外部アプリを開く処理の例外
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    // 開発リポジトリまたは保存済みの元リポジトリから再インストールScriptを探します。
    std::optional<std::filesystem::path>
        EditorLayer::DesktopReinstallScript() const
    {

        // 開発リポジトリの再導入Script
        const auto directScript = m_engineRoot
            / "tools"
            / "RebuildAndInstallEditor.ps1";
        if (IsRegularFile(directScript)
            && IsRegularFile(m_engineRoot / "CMakeLists.txt"))
        {
            return directScript;
        }

        // 保存済みの開発元情報JSON
        const auto manifest = ExecutableDirectory()
            / "desktop-build-source.json";
        if (!IsRegularFile(manifest))
        {
            return std::nullopt;
        }

        try
        {
            // 形式を検証する開発元情報
            const auto document = nlohmann::json::parse(
                ReadTextFile(manifest));
            if (document.value("format", std::string{})
                    != "LamaPonDesktopBuildSource"
                || document.value("version", 0) != 1)
            {
                return std::nullopt;
            }
            // 保存済みの開発元ルート
            const auto sourceRoot = PathFromUtf8(
                document.value("sourceRoot", std::string{}));
            if (sourceRoot.empty())
            {
                return std::nullopt;
            }
            // 開発元の再導入Script
            const auto script = sourceRoot
                / "tools"
                / "RebuildAndInstallEditor.ps1";
            if (!IsRegularFile(script)
                || !IsRegularFile(sourceRoot / "CMakeLists.txt"))
            {
                return std::nullopt;
            }
            return script;
        }
        catch (const std::exception&)
        {
            return std::nullopt;
        }
    }

    // API不一致を一度だけ確認し、了承と保存確認後に再インストールを起動して終了します(diagnostic: 不一致の診断文)。
    bool EditorLayer::OfferDesktopReinstallForGameModuleMismatch(
        const std::string& diagnostic)
    {
        if (m_desktopReinstallPrompted
            || !IsSdkRuntimeMismatch(diagnostic))
        {
            return false;
        }
        m_desktopReinstallPrompted = true;

        // 起動する再インストールScript
        const auto script = DesktopReinstallScript();
        if (!script)
        {
            SetStatus(
                "Game Module SDKとRuntimeの世代が一致しません。"
                "デスクトップの自動ビルドを一度実行して、"
                "RuntimeとSDKを再インストールしてください。",
                true);
            return false;
        }

        // 再インストールの確認文
        const std::wstring message =
            L"LamaPon RuntimeとGame Module SDKの世代が一致しません。\n\n"
            L"デスクトップ版を再インストールして、RuntimeとSDKを"
            L"同じ版へそろえます。エディターとHubを閉じて、"
            L"クリーンビルドを実行します。\n\n"
            L"保存していない変更がある場合は、先に保存確認を表示します。\n\n"
            L"再インストールしますか？";
        // 再インストール確認の回答
        const int result = MessageBoxW(
            m_window,
            message.c_str(),
            L"LamaPon SDK の再インストール",
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (result != IDYES)
        {
            SetStatus(
                "Game Module SDKとRuntimeの世代が一致しません。"
                "デスクトップの自動ビルドで再インストールしてください。",
                true);
            return false;
        }
        if (!ConfirmClose())
        {
            SetStatus("SDKの再インストールをキャンセルしました", true);
            return false;
        }

        // 再インストールの起動引数
        const std::wstring parameters =
            L"-NoProfile -ExecutionPolicy Bypass -File \""
            + script->wstring()
            + L"\" -NonInteractive -CloseRunningLamaPonProcesses "
              L"-WaitForProcessId "
            + std::to_wstring(GetCurrentProcessId());
        // 再インストールの起動設定
        SHELLEXECUTEINFOW executeInfo{};
        executeInfo.cbSize = sizeof(executeInfo);
        executeInfo.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        executeInfo.hwnd = m_window;
        executeInfo.lpVerb = L"open";
        executeInfo.lpFile = L"powershell.exe";
        executeInfo.lpParameters = parameters.c_str();
        executeInfo.lpDirectory = script->parent_path().c_str();
        executeInfo.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&executeInfo)
            || executeInfo.hProcess == nullptr)
        {
            SetStatus(
                "SDKの再インストールを開始できませんでした: "
                + PathToUtf8(*script),
                true);
            return false;
        }
        CloseHandle(executeInfo.hProcess);

        // スクリプトは上のPIDの終了を待ってから実行ファイルを更新します。
        DestroyWindow(m_window);
        return true;
    }

    // GameModuleの非表示ビルドを開始し、開始成功または既に実行中ならtrueを返します。
    bool EditorLayer::BuildGameModule()
    {
        if (m_gameModuleBuildProcess != nullptr)
        {
            SetStatus(
                "Game Moduleをビルド中です。完了後に自動で反映します");
            return true;
        }

        try
        {
            // 実体を解決したプロジェクト
            const auto projectRoot =
                std::filesystem::weakly_canonical(
                    m_graphics.Assets().AssetRoot().parent_path());
            // エディターの配置フォルダー
            const auto executableDirectory = ExecutableDirectory();

            // ビルドの起動引数と保存先
            const auto buildCommand =
                MakeGameModuleBuildCommand(
                    projectRoot,
                    m_engineRoot,
                    executableDirectory,
                    m_buildConfiguration,
                    true);
            // ネットワーク保存先で時刻が動かない変更だけ内容ハッシュで補います。
            if (buildCommand.usesLocalBuildCache)
            {
                static_cast<void>(RefreshStaleGameModuleSources(
                    projectRoot,
                    buildCommand.buildDirectory));
            }
            // 自分で進めた時刻を変更検知から除き、二重ビルドを防ぎます。
            m_lastSeenScriptWriteTime = LatestScriptWriteTime();
            m_scriptWriteTimeInitialized = true;
            m_scriptChangeDetectedAt = 0.0;
            m_scriptRebuildQueued = false;
            m_gameModuleBuildLogPath =
                buildCommand.logPath;
            // cmd.exeへ渡すビルド引数
            const std::wstring& command =
                buildCommand.parameters;

            // 非表示ビルドの起動設定
            SHELLEXECUTEINFOW executeInfo{};
            executeInfo.cbSize = sizeof(executeInfo);
            executeInfo.fMask =
                SEE_MASK_NOCLOSEPROCESS
                | SEE_MASK_FLAG_NO_UI;
            executeInfo.hwnd = m_window;
            executeInfo.lpVerb = L"open";
            executeInfo.lpFile = L"cmd.exe";
            executeInfo.lpParameters = command.c_str();
            executeInfo.lpDirectory = projectRoot.c_str();
            executeInfo.nShow = SW_HIDE;
            if (!ShellExecuteExW(&executeInfo)
                || executeInfo.hProcess == nullptr)
            {
                SetStatus(
                    "Game Moduleの自動ビルドを開始できませんでした",
                    true);
                return false;
            }

            m_gameModuleBuildProcess =
                executeInfo.hProcess;
            m_gameModuleBuildStartedAt =
                ImGui::GetTime();
            SetStatus(
                "Game Moduleをビルドしています（最大"
                + std::to_string(buildCommand.parallelJobs)
                + "並列）");
            return true;
        }
        // ビルド開始処理の例外
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
            return false;
        }
    }

    // assets内のcpp・h・hppの最新更新時刻を返し、取得できなければ既定値を返します。
    std::filesystem::file_time_type
        EditorLayer::LatestScriptWriteTime() const
    {

        // 確認できた最新Script更新時刻
        std::filesystem::file_time_type latest{};
        // 走査するアセットルート
        const auto& assetRoot =
            m_graphics.Assets().AssetRoot();
        // Script走査・時刻取得エラー
        std::error_code error;
        if (!std::filesystem::is_directory(assetRoot, error))
        {
            return latest;
        }

        // 権限不足を飛ばす走査設定
        const auto options =
            std::filesystem::directory_options::
                skip_permission_denied;
        // assets内の再帰走査位置
        for (std::filesystem::recursive_directory_iterator
                iterator{ assetRoot, options, error };
            iterator
                != std::filesystem::
                    recursive_directory_iterator{};
            iterator.increment(error))
        {
            if (error)
            {
                error.clear();
                continue;
            }
            if (!iterator->is_regular_file(error) || error)
            {
                error.clear();
                continue;
            }
            // 判定する小文字の拡張子
            const auto extension = Lowercase(
                PathToUtf8(iterator->path().extension()));
            if (extension != ".cpp"
                && extension != ".h"
                && extension != ".hpp")
            {
                continue;
            }
            // Script候補の更新時刻
            const auto writeTime =
                iterator->last_write_time(error);
            if (error)
            {
                error.clear();
                continue;
            }
            latest = std::max(latest, writeTime);
        }
        return latest;
    }

    // Script更新を監視し、保存の連続が1.5秒静まった後に自動ビルドを要求します。
    void EditorLayer::UpdateScriptAutoBuild()
    {
        if (!m_projectSettings.autoBuildGameModuleOnSave
            || m_playing)
        {
            return;
        }

        // 自動ビルド監視の現在時刻
        const double now = ImGui::GetTime();
        // 走査時間の20倍を次の間隔にし、0.5～30秒へ制限します。
        if (now - m_lastScriptScanAt >= m_scriptScanIntervalSeconds)
        {
            m_lastScriptScanAt = now;
            // Script走査の開始時刻
            const auto scanStartedAt =
                std::chrono::steady_clock::now();
            // 今回の最新Script更新時刻
            const auto latest = LatestScriptWriteTime();
            // 今回の走査所要時間・秒
            const double scanSeconds =
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now()
                    - scanStartedAt).count();
            m_scriptScanIntervalSeconds =
                std::clamp(scanSeconds * 20.0, 0.5, 30.0);
            if (!m_scriptWriteTimeInitialized)
            {
                m_lastSeenScriptWriteTime = latest;
                m_scriptWriteTimeInitialized = true;
                // 初回のビルド要否と出力状態
                const auto state = InspectGameModuleBuildState(
                    m_graphics.Assets().AssetRoot().parent_path());
                if (state.buildRequired)
                {
                    // 初回は待ちを省き、0を未検出として扱うため検知時刻を正にします。
                    m_scriptChangeDetectedAt = std::max(now - 1.5, 0.000001);
                    // 自動ビルド開始の理由
                    const char* reason =
                        "C++ Scriptが更新されているためGame Moduleをビルドします";
                    if (!state.outputExists)
                    {
                        reason = "C++ Scriptの初回Game Moduleをビルドします";
                    }
                    else if (state.staleAgainstRuntime)
                    {
                        reason = "エンジンが更新されたためGame Moduleを"
                                 "再ビルドします";
                    }
                    SetStatus(reason);
                }
            }
            else if (latest > m_lastSeenScriptWriteTime)
            {
                m_lastSeenScriptWriteTime = latest;
                m_scriptChangeDetectedAt = now;
                if (m_gameModuleBuildProcess != nullptr)
                {
                    // ビルド中の変更は完了後の再ビルドとして予約します。
                    m_scriptRebuildQueued = true;
                }
                else
                {
                    SetStatus(
                        "スクリプトの変更を検知しました。"
                        "まもなくGame Moduleをビルドします");
                }
            }
        }

        if (m_gameModuleBuildProcess != nullptr)
        {
            return;
        }
        if (m_scriptRebuildQueued)
        {
            m_scriptRebuildQueued = false;
            m_scriptChangeDetectedAt = now;
        }
        if (m_scriptChangeDetectedAt <= 0.0)
        {
            return;
        }
        // 保存の連続が1.5秒静まるまで待ちます。
        if (now - m_scriptChangeDetectedAt < 1.5)
        {
            return;
        }
        // 開始が失敗しても予約は消費し、次の変更検知まで再試行しません。
        m_scriptChangeDetectedAt = 0.0;
        static_cast<void>(BuildGameModule());
    }

    // 終了済みビルドを回収し、再読込成功後に予約したScriptを追加します。
    void EditorLayer::UpdateGameModuleBuild()
    {
        if (m_gameModuleBuildProcess == nullptr)
        {
            return;
        }

        // ビルドプロセスの終了コード
        DWORD exitCode{};
        if (!GetExitCodeProcess(
                m_gameModuleBuildProcess,
                &exitCode))
        {
            CloseHandle(m_gameModuleBuildProcess);
            m_gameModuleBuildProcess = nullptr;
            m_gameModuleBuildStartedAt = 0.0;
            m_pendingScriptAttachments.clear();
            SetStatus(
                "Game Moduleのビルド結果を確認できませんでした",
                true);
            return;
        }
        if (exitCode == STILL_ACTIVE)
        {
            return;
        }

        CloseHandle(m_gameModuleBuildProcess);
        m_gameModuleBuildProcess = nullptr;
        m_gameModuleBuildStartedAt = 0.0;
        if (exitCode != 0)
        {
            m_pendingScriptAttachments.clear();
            if (OfferDesktopReinstallForGameModuleMismatch(
                    ReadTextFile(m_gameModuleBuildLogPath)))
            {
                return;
            }
            SetStatus(
                "C++ Scriptのビルドに失敗しました。ログ: "
                + PathToUtf8(m_gameModuleBuildLogPath),
                true);
            return;
        }

        // 再読込するGameModule
        auto* module = GameModuleHost::Current();
        if (module == nullptr || !module->Reload())
        {
            m_pendingScriptAttachments.clear();
            // ビルド後の再読込エラー
            const std::string diagnostic = module != nullptr
                ? module->LastError()
                : "Game Moduleを再読み込みできませんでした";
            if (OfferDesktopReinstallForGameModuleMismatch(diagnostic))
            {
                return;
            }
            SetStatus(
                diagnostic,
                true);
            return;
        }

        if (m_pendingScriptAttachments.empty())
        {
            SetStatus(
                "Game Moduleをビルドして再読み込みしました");
            return;
        }
        CompletePendingCppScriptAttachments();
    }

    // 重複を避けてScript追加を予約し、ビルド開始失敗時はその予約を取り消します(gameObject: 追加先, asset: 相対Scriptパス)。
    void EditorLayer::QueueCppScriptAttachment(
        GameObject& gameObject,
        const std::filesystem::path& asset)
    {
        try
        {
            // 実体を解決したアセットルート
            const auto assetRoot =
                std::filesystem::weakly_canonical(
                    m_graphics.Assets().AssetRoot());
            // 実体を解決したScriptパス
            const auto resolved =
                std::filesystem::weakly_canonical(
                    m_graphics.Assets().ResolvePath(asset));
            if (!IsCppScriptAsset(resolved)
                || !IsPathWithin(assetRoot, resolved)
                || !std::filesystem::is_regular_file(resolved))
            {
                SetStatus(
                    "ドロップされたC++ Scriptが見つかりません",
                    true);
                return;
            }

            // ファイル名から得るクラス名
            const std::string className =
                PathToUtf8(resolved.stem());
            if (!IsCppIdentifier(className))
            {
                SetStatus(
                    "C++ Script名には半角英字・数字・アンダースコアを使用してください",
                    true);
                return;
            }
            // 追加するScriptの登録型名
            const std::string scriptType =
                "Game." + className;
            // 同じ型を追加済みか調べます(component: 対象のコンポーネント)。
            const bool alreadyAttached =
                std::ranges::any_of(
                    gameObject.Components(),
                    [&scriptType](
                        const std::unique_ptr<Component>& component)
                    {
                        // 同じ型かを調べる既存Script
                        const auto* script =
                            dynamic_cast<
                                const NativeScriptComponent*>(
                                    component.get());
                        return script != nullptr
                            && script->ScriptType()
                                == scriptType;
                    });
            if (!alreadyAttached)
            {
                // 同じ対象と型の予約を調べます(pending: 追加待ちのScript)。
                const bool alreadyQueued =
                    std::ranges::any_of(
                        m_pendingScriptAttachments,
                        [&gameObject, &scriptType](
                            const PendingScriptAttachment& pending)
                        {
                            return pending.gameObjectId
                                    == gameObject.Id()
                                && pending.scriptType
                                    == scriptType;
                        });
                if (!alreadyQueued)
                {
                    m_pendingScriptAttachments.push_back({
                        gameObject.Id(),
                        scriptType,
                        className
                    });
                }
            }

            m_selectedObjectId = gameObject.Id();
            if (!BuildGameModule())
            {
                // 開始失敗時の予約を消します(pending: 取り消す対象と型の候補)。
                std::erase_if(
                    m_pendingScriptAttachments,
                    [&gameObject, &scriptType](
                        const PendingScriptAttachment& pending)
                    {
                        return pending.gameObjectId
                                == gameObject.Id()
                            && pending.scriptType
                                == scriptType;
                    });
                return;
            }

            SetStatus(
                alreadyAttached
                    ? className
                        + "を更新しています。ビルド後に自動で再読み込みします"
                    : className
                        + "をビルドしています。完了後に自動でアタッチします");
        }
        // Script追加の予約処理の例外
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    // 存在と登録型を確認して予約Scriptを追加し、成功分の履歴と最初のエラーを通知します。
    void EditorLayer::CompletePendingCppScriptAttachments()
    {
        // 型登録を確認するGameModule
        auto* module = GameModuleHost::Current();
        if (module == nullptr)
        {
            m_pendingScriptAttachments.clear();
            SetStatus(
                "Game Moduleが読み込まれていません",
                true);
            return;
        }

        // 今回追加できたScript数
        std::size_t attachedCount{};
        // 最初のScript追加エラー
        std::string firstError;
        // 処理するScript追加の予約
        for (const auto& pending :
            m_pendingScriptAttachments)
        {
            // 予約Scriptの追加先
            auto* gameObject =
                m_scene.FindGameObject(
                    pending.gameObjectId);
            if (gameObject == nullptr)
            {
                if (firstError.empty())
                {
                    firstError =
                        "アタッチ先のGameObjectが見つかりません";
                }
                continue;
            }

            // 同じ型を追加済みか調べます(component: 対象のコンポーネント)。
            const bool alreadyAttached =
                std::ranges::any_of(
                    gameObject->Components(),
                    [&pending](
                        const std::unique_ptr<Component>& component)
                    {
                        // 同じ型かを調べる既存Script
                        const auto* script =
                            dynamic_cast<
                                const NativeScriptComponent*>(
                                    component.get());
                        return script != nullptr
                            && script->ScriptType()
                                == pending.scriptType;
                    });
            if (alreadyAttached)
            {
                continue;
            }
            if (module->FindComponent(
                    pending.scriptType) == nullptr)
            {
                if (firstError.empty())
                {
                    firstError =
                        pending.displayName
                        + "が登録されていません。ファイル名とクラス名、LAMAPON_SCRIPTを確認してください";
                }
                continue;
            }

            gameObject->AddComponent<
                NativeScriptComponent>(
                    pending.scriptType);
            ++attachedCount;
        }
        m_pendingScriptAttachments.clear();

        if (attachedCount != 0)
        {
            RecordHistory();
        }
        if (!firstError.empty())
        {
            SetStatus(firstError, true);
            return;
        }
        SetStatus(
            attachedCount == 1
                ? "C++ Scriptをビルドしてアタッチしました"
                : std::to_string(attachedCount)
                    + "件のC++ Scriptをビルドしてアタッチしました");
    }

    // アセットの作成・改名・削除Popupを描画し、参照確認と操作成功後に閉じます。
    void EditorLayer::DrawAssetFolderDialogs()
    {
        if (ImGui::BeginPopupModal(
            "フォルダーを作成",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (ImGui::IsWindowAppearing())
            {
                ImGui::SetKeyboardFocusHere();
            }
            // Enterで名前入力を確定したか
            const bool submit = ImGui::InputText(
                "名前",
                m_assetFolderNameBuffer.data(),
                m_assetFolderNameBuffer.size(),
                ImGuiInputTextFlags_EnterReturnsTrue);
            if (!m_assetFolderDialogError.empty())
            {
                ImGui::TextColored(
                    ImVec4{ 1.0f, 0.35f, 0.30f, 1.0f },
                    "%s",
                    m_assetFolderDialogError.c_str());
            }
            if ((ImGui::Button("作成") || submit)
                && CreateAssetFolder())
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("キャンセル"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal(
            "シーンを作成",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (ImGui::IsWindowAppearing())
            {
                ImGui::SetKeyboardFocusHere();
            }
            // Enterで名前入力を確定したか
            const bool submit = ImGui::InputText(
                "名前",
                m_assetFileNameBuffer.data(),
                m_assetFileNameBuffer.size(),
                ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::TextDisabled(
                "拡張子は .scene.json を使用します。");
            if (!m_assetFileDialogError.empty())
            {
                ImGui::TextColored(
                    ImVec4{ 1.0f, 0.35f, 0.30f, 1.0f },
                    "%s",
                    m_assetFileDialogError.c_str());
            }
            if ((ImGui::Button("作成") || submit)
                && CreateSceneAsset())
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("キャンセル"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal(
            "Lit Materialを作成",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (ImGui::IsWindowAppearing())
            {
                ImGui::SetKeyboardFocusHere();
            }
            // Enterで名前入力を確定したか
            const bool submit = ImGui::InputText(
                "名前",
                m_assetFileNameBuffer.data(),
                m_assetFileNameBuffer.size(),
                ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::TextDisabled(
                "拡張子は .material.json を使用します。");
            if (!m_assetFileDialogError.empty())
            {
                ImGui::TextColored(
                    ImVec4{ 1.0f, 0.35f, 0.30f, 1.0f },
                    "%s",
                    m_assetFileDialogError.c_str());
            }
            if ((ImGui::Button("作成") || submit)
                && CreateMaterialAsset())
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("キャンセル"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal(
            "データアセットを作成",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize))
        {
            // データアセット型の登録元
            const auto* host = GameModuleHost::Current();
            // 作成できるデータ型があるか
            const bool hasTypes = host != nullptr
                && !host->RegisteredDataAssets().empty();
            if (!hasTypes)
            {
                ImGui::TextWrapped(
                    "データアセットの型がまだ宣言されていません。");
                ImGui::TextWrapped(
                    "C++ Scriptの中で LAMAPON_DATA_ASSET("
                    "\"Game.型名\", \"表示名\", スキーマ) と書いて"
                    "Game Moduleをビルドすると、ここへ現れます。");
                if (ImGui::Button("閉じる"))
                {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            else
            {
            // 選択中のデータ型の表示
            const std::string preview =
                m_createDataAssetTypeName.empty()
                    ? std::string{ "選択してください" }
                    : m_createDataAssetTypeName;
            if (ImGui::BeginCombo("型", preview.c_str()))
            {
                // 作成するデータ型の候補
                for (const auto& type :
                    host->RegisteredDataAssets())
                {
                    // 選択中のデータ型か
                    const bool selected =
                        type.typeName
                            == m_createDataAssetTypeName;
                    // データ型候補の表示名
                    const std::string label =
                        type.displayName
                        + "  ("
                        + type.typeName
                        + ")";
                    if (ImGui::Selectable(
                            label.c_str(),
                            selected))
                    {
                        m_createDataAssetTypeName =
                            type.typeName;
                    }
                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            // Enterで名前入力を確定したか
            const bool submit = ImGui::InputText(
                "名前",
                m_assetFileNameBuffer.data(),
                m_assetFileNameBuffer.size(),
                ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::TextDisabled(
                "拡張子は .asset.json を使用します。");
            if (!m_assetFileDialogError.empty())
            {
                ImGui::TextColored(
                    ImVec4{ 1.0f, 0.35f, 0.30f, 1.0f },
                    "%s",
                    m_assetFileDialogError.c_str());
            }
            if ((ImGui::Button("作成") || submit)
                && CreateDataAsset())
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("キャンセル"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
            }
        }

        if (ImGui::BeginPopupModal(
            "C++ Scriptを作成",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (ImGui::IsWindowAppearing())
            {
                ImGui::SetKeyboardFocusHere();
            }
            // Enterで名前入力を確定したか
            const bool submit = ImGui::InputText(
                "クラス名 / ファイル名",
                m_assetFileNameBuffer.data(),
                m_assetFileNameBuffer.size(),
                ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::TextDisabled(
                "例: PlayerController.cpp（英数字とアンダースコア）");
            ImGui::TextWrapped(
                "Scriptを継承した初心者向けの雛形を作成し、"
                "Game Moduleへ自動登録します。");
            if (!m_assetFileDialogError.empty())
            {
                ImGui::TextColored(
                    ImVec4{ 1.0f, 0.35f, 0.30f, 1.0f },
                    "%s",
                    m_assetFileDialogError.c_str());
            }
            if ((ImGui::Button("作成して開く") || submit)
                && CreateCppScriptAsset())
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("キャンセル"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal(
            "カスタムShaderを作成",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (ImGui::IsWindowAppearing())
            {
                ImGui::SetKeyboardFocusHere();
            }
            // Enterで名前入力を確定したか
            const bool submit = ImGui::InputText(
                "名前",
                m_assetFileNameBuffer.data(),
                m_assetFileNameBuffer.size(),
                ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SeparatorText("作成方法");
            if (ImGui::RadioButton(
                    "簡易ノード生成",
                    m_createShaderFromGraph))
            {
                m_createShaderFromGraph = true;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton(
                    "コード雛形",
                    !m_createShaderFromGraph))
            {
                m_createShaderFromGraph = false;
            }
            if (m_createShaderFromGraph)
            {
                ImGui::TextDisabled(
                    "Albedo Texture → 選択ノード → Material Output");
                ImGui::Checkbox("Tint", &m_createShaderGraph.tint);
                ImGui::SameLine();
                ImGui::Checkbox(
                    "Emission",
                    &m_createShaderGraph.emission);
                ImGui::Checkbox(
                    "Rim Light",
                    &m_createShaderGraph.rimLight);
                ImGui::SameLine();
                ImGui::Checkbox(
                    "UV Scroll",
                    &m_createShaderGraph.uvScroll);
                ImGui::Checkbox(
                    "Mask Texture",
                    &m_createShaderGraph.maskTexture);
                ImGui::SameLine();
                ImGui::Checkbox(
                    "Alpha Clip",
                    &m_createShaderGraph.alphaClip);
                ImGui::TextWrapped(
                    "定数とTextureの空きスロットは自動で割り当てます。"
                    "register番号を指定する必要はありません。");
            }
            else
            {
                ImGui::TextDisabled(
                    "VSMain / PSMainを持つ編集用HLSL雛形です。");
            }
            if (!m_assetFileDialogError.empty())
            {
                ImGui::TextColored(
                    ImVec4{ 1.0f, 0.35f, 0.30f, 1.0f },
                    "%s",
                    m_assetFileDialogError.c_str());
            }
            if ((ImGui::Button("作成") || submit)
                && CreateShaderAsset())
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("キャンセル"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal(
            "フォルダー名を変更",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (ImGui::IsWindowAppearing())
            {
                ImGui::SetKeyboardFocusHere();
            }
            // Enterで名前入力を確定したか
            const bool submit = ImGui::InputText(
                "新しい名前",
                m_assetFolderNameBuffer.data(),
                m_assetFolderNameBuffer.size(),
                ImGuiInputTextFlags_EnterReturnsTrue);
            if (!m_assetFolderDialogError.empty())
            {
                ImGui::TextColored(
                    ImVec4{ 1.0f, 0.35f, 0.30f, 1.0f },
                    "%s",
                    m_assetFolderDialogError.c_str());
            }
            if ((ImGui::Button("変更") || submit)
                && RenameAssetFolder())
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("キャンセル"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal(
            "フォルダーを削除",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text(
                "空フォルダー「%s」を削除しますか？",
                PathToUtf8(m_assetDirectory.filename()).c_str());
            ImGui::TextDisabled("ファイルや子フォルダーがある場合は削除できません。");
            if (!m_assetFolderDialogError.empty())
            {
                ImGui::TextColored(
                    ImVec4{ 1.0f, 0.35f, 0.30f, 1.0f },
                    "%s",
                    m_assetFolderDialogError.c_str());
            }

            ImGui::PushStyleColor(
                ImGuiCol_Button,
                ImVec4{ 0.70f, 0.16f, 0.14f, 1.0f });
            if (ImGui::Button("削除する")
                && DeleteAssetFolder())
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Button("キャンセル"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal(
            "ファイル名を変更",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text(
                "現在: %s",
                PathToUtf8(m_selectedAsset).c_str());
            if (ImGui::IsWindowAppearing())
            {
                ImGui::SetKeyboardFocusHere();
            }
            // Enterで名前入力を確定したか
            const bool submit = ImGui::InputText(
                "新しい名前",
                m_assetFileNameBuffer.data(),
                m_assetFileNameBuffer.size(),
                ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::TextDisabled("ファイル形式を維持するため、拡張子は変更できません。");
            if (!m_assetFileDialogError.empty())
            {
                ImGui::TextColored(
                    ImVec4{ 1.0f, 0.35f, 0.30f, 1.0f },
                    "%s",
                    m_assetFileDialogError.c_str());
            }
            if ((ImGui::Button("変更") || submit)
                && RenameSelectedAsset())
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("キャンセル"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal(
            "ファイルを削除",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text(
                "「%s」を完全に削除しますか？",
                PathToUtf8(m_selectedAsset).c_str());
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.58f, 0.18f, 1.0f },
                "この操作は元に戻せません。");

            if (!m_assetDeleteScanError.empty())
            {
                ImGui::Separator();
                ImGui::TextColored(
                    ImVec4{ 1.0f, 0.35f, 0.30f, 1.0f },
                    "%s",
                    m_assetDeleteScanError.c_str());
                if (ImGui::SmallButton("参照を再確認"))
                {
                    RefreshAssetDeleteReferences();
                }
            }
            else if (m_assetDeleteReferences.empty())
            {
                ImGui::TextDisabled(
                    "シーン内の直接参照は見つかりませんでした。");
            }
            else
            {
                ImGui::Separator();
                ImGui::TextColored(
                    ImVec4{ 1.0f, 0.58f, 0.18f, 1.0f },
                    "%zu件のシーン参照があります:",
                    m_assetDeleteReferences.size());

                // 表示する削除参照数の上限
                constexpr std::size_t MaximumVisibleReferences = 8;
                // 削除確認に表示する参照数
                const std::size_t visibleReferences = std::min(
                    MaximumVisibleReferences,
                    m_assetDeleteReferences.size());
                ImGui::PushTextWrapPos(
                    ImGui::GetCursorPosX() + 560.0f);
                // 削除確認の参照番号
                for (std::size_t index = 0;
                    // 削除確認に表示する参照数
                    index < visibleReferences;
                    ++index)
                {
                    ImGui::BulletText(
                        "%s",
                        m_assetDeleteReferences[index].c_str());
                }
                if (visibleReferences < m_assetDeleteReferences.size())
                {
                    ImGui::TextDisabled(
                        "ほか %zu件",
                        m_assetDeleteReferences.size()
                            - visibleReferences);
                }
                ImGui::TextWrapped(
                    "現在のシーンにある直接参照は自動解除します。"
                    "別のシーンファイルは変更しません。");
                ImGui::PopTextWrapPos();
                ImGui::Checkbox(
                    "参照切れを理解して削除する",
                    &m_assetDeleteAcknowledged);
            }

            if (!m_assetFileDialogError.empty())
            {
                ImGui::TextColored(
                    ImVec4{ 1.0f, 0.35f, 0.30f, 1.0f },
                    "%s",
                    m_assetFileDialogError.c_str());
            }

            // 参照確認で削除を止めるか
            const bool deleteBlocked =
                !m_assetDeleteScanError.empty()
                || (!m_assetDeleteReferences.empty()
                    && !m_assetDeleteAcknowledged);
            ImGui::BeginDisabled(deleteBlocked);
            ImGui::PushStyleColor(
                ImGuiCol_Button,
                ImVec4{ 0.70f, 0.16f, 0.14f, 1.0f });
            if (ImGui::Button("完全に削除")
                && DeleteSelectedAsset())
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor();
            ImGui::EndDisabled();

            ImGui::SameLine();
            if (ImGui::Button("キャンセル"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // 名前と作成先を検証して空フォルダーを作成し、表示先を切り替えます。
    bool EditorLayer::CreateAssetFolder()
    {
        // 入力された作成・変更名
        const std::string name = m_assetFolderNameBuffer.data();
        // 名前の検証エラー
        if (const auto validationError =
            ValidateAssetEntryName(name, "フォルダー名"))
        {
            m_assetFolderDialogError = *validationError;
            return false;
        }

        try
        {
            // 正規化したアセットルート
            const auto root = std::filesystem::weakly_canonical(
                m_graphics.Assets().AssetRoot());
            // 指定された親フォルダー
            const auto parentPath =
                (root / m_assetDirectory).lexically_normal();
            // 実体の作成先フォルダー
            const auto resolvedParent =
                std::filesystem::weakly_canonical(parentPath);
            if (!IsPathWithin(root, resolvedParent)
                || !std::filesystem::is_directory(parentPath)
                || std::filesystem::is_symlink(parentPath))
            {
                m_assetFolderDialogError =
                    "アセットルート外にはフォルダーを作成できません";
                return false;
            }

            // 新フォルダーの相対パス
            const auto newRelativeDirectory =
                m_assetDirectory / PathFromUtf8(name);
            // 作成先の絶対パス
            const auto destination =
                (parentPath / PathFromUtf8(name)).lexically_normal();
            if (!IsPathWithin(root, destination))
            {
                m_assetFolderDialogError =
                    "アセットルート外にはフォルダーを作成できません";
                return false;
            }
            if (std::filesystem::exists(destination))
            {
                m_assetFolderDialogError =
                    "同じ名前のファイルまたはフォルダーが存在します";
                return false;
            }

            // フォルダー作成エラー
            std::error_code error;
            if (!std::filesystem::create_directory(destination, error)
                || error)
            {
                m_assetFolderDialogError =
                    "フォルダーを作成できませんでした";
                return false;
            }

            m_assetDirectory = newRelativeDirectory;
            m_selectedAsset.clear();
            RefreshAssets();
            SetStatus(
                "フォルダーを作成しました: "
                + PathToUtf8(m_assetDirectory));
            return true;
        }
        // 作成・変更処理の失敗原因
        catch (const std::exception& exception)
        {
            m_assetFolderDialogError = exception.what();
            return false;
        }
    }

    // 作成先を検証し、メインカメラと太陽光を持つシーンを保存します。
    bool EditorLayer::CreateSceneAsset()
    {
        // 入力された作成・変更名
        const std::string name = m_assetFileNameBuffer.data();
        // 名前の検証エラー
        if (const auto validationError =
            ValidateAssetEntryName(name, "シーン名"))
        {
            m_assetFileDialogError = *validationError;
            return false;
        }
        if (!Lowercase(name).ends_with(".scene.json")
            || name.size() <= std::string_view{
                ".scene.json"
            }.size())
        {
            m_assetFileDialogError =
                "ファイル名を .scene.json で終えてください";
            return false;
        }

        try
        {
            // 正規化したアセットルート
            const auto root = std::filesystem::weakly_canonical(
                m_graphics.Assets().AssetRoot());
            // 指定された親フォルダー
            const auto parent =
                (root / m_assetDirectory).lexically_normal();
            // 実体の作成先フォルダー
            const auto resolvedParent =
                std::filesystem::weakly_canonical(parent);
            if (!IsPathWithin(root, resolvedParent)
                || !std::filesystem::is_directory(parent)
                || std::filesystem::is_symlink(parent))
            {
                m_assetFileDialogError =
                    "アセットルート外には作成できません";
                return false;
            }

            // 新アセットの相対パス
            const auto relativePath =
                m_assetDirectory / PathFromUtf8(name);
            // 作成先の絶対パス
            const auto destination =
                (root / relativePath).lexically_normal();
            if (!IsPathWithin(root, destination)
                || std::filesystem::exists(destination))
            {
                m_assetFileDialogError =
                    "同じ名前のファイルが存在します";
                return false;
            }

            // 保存する初期シーン
            Scene newScene(m_graphics);
            // 初期メインカメラの対象
            auto& cameraObject =
                newScene.CreateGameObject("メインカメラ");
            cameraObject.GetTransform().position =
                { 0.0f, 1.6f, 7.0f };
            cameraObject.GetTransform().SetEulerAngles(
                { -0.12f, 0.0f, 0.0f });
            // 初期メインカメラ
            auto& camera =
                cameraObject.AddComponent<CameraComponent>();
            newScene.SetMainCamera(camera);

            // 初期太陽光の対象
            auto& lightObject =
                newScene.CreateGameObject("太陽光");
            lightObject.GetTransform().SetEulerAngles({
                DirectX::XMConvertToRadians(-45.0f),
                DirectX::XMConvertToRadians(-35.0f),
                0.0f
            });
            lightObject.AddComponent<
                DirectionalLightComponent>();

            newScene.SaveToFile(destination);

            m_selectedAsset = relativePath;
            RefreshAssets();
            SetStatus(
                "シーンを作成しました: "
                + PathToUtf8(relativePath));
            return true;
        }
        // 作成・変更処理の失敗原因
        catch (const std::exception& exception)
        {
            m_assetFileDialogError = exception.what();
            return false;
        }
    }

    // 作成先を検証し、既定色のLit Materialを保存します。
    bool EditorLayer::CreateMaterialAsset()
    {
        // 入力された作成・変更名
        const std::string name = m_assetFileNameBuffer.data();
        // 名前の検証エラー
        if (const auto validationError =
            ValidateAssetEntryName(name, "Material名"))
        {
            m_assetFileDialogError = *validationError;
            return false;
        }
        if (!Lowercase(name).ends_with(".material.json")
            || name.size() <= std::string_view{
                ".material.json"
            }.size())
        {
            m_assetFileDialogError =
                "ファイル名を .material.json で終えてください";
            return false;
        }

        try
        {
            // 正規化したアセットルート
            const auto root = std::filesystem::weakly_canonical(
                m_graphics.Assets().AssetRoot());
            // 指定された親フォルダー
            const auto parent =
                (root / m_assetDirectory).lexically_normal();
            // 実体の作成先フォルダー
            const auto resolvedParent =
                std::filesystem::weakly_canonical(parent);
            if (!IsPathWithin(root, resolvedParent)
                || !std::filesystem::is_directory(parent)
                || std::filesystem::is_symlink(parent))
            {
                m_assetFileDialogError =
                    "アセットルート外には作成できません";
                return false;
            }

            // 新アセットの相対パス
            const auto relativePath =
                m_assetDirectory / PathFromUtf8(name);
            // 作成先の絶対パス
            const auto destination =
                (root / relativePath).lexically_normal();
            if (!IsPathWithin(root, destination)
                || std::filesystem::exists(destination))
            {
                m_assetFileDialogError =
                    "同じ名前のファイルが存在します";
                return false;
            }

            SaveLitMaterialAsset(
                destination,
                LitMaterial{
                    DirectX::XMFLOAT4{
                        0.75f,
                        0.75f,
                        0.75f,
                        1.0f
                    }
                });
            m_selectedAsset = relativePath;
            RefreshAssets();
            SetStatus(
                "Lit Materialを作成しました: "
                + PathToUtf8(relativePath));
            return true;
        }
        // 作成・変更処理の失敗原因
        catch (const std::exception& exception)
        {
            m_assetFileDialogError = exception.what();
            return false;
        }
    }

    // 選択型と作成先を検証し、型のスキーマからデータアセットを保存します。
    bool EditorLayer::CreateDataAsset()
    {
        // 入力された作成・変更名
        const std::string name = m_assetFileNameBuffer.data();
        if (m_createDataAssetTypeName.empty())
        {
            m_assetFileDialogError =
                "データアセットの型を選んでください";
            return false;
        }
        // 名前の検証エラー
        if (const auto validationError =
            ValidateAssetEntryName(name, "データアセット名"))
        {
            m_assetFileDialogError = *validationError;
            return false;
        }
        if (!Lowercase(name).ends_with(".asset.json")
            || name.size() <= std::string_view{
                ".asset.json"
            }.size())
        {
            m_assetFileDialogError =
                "ファイル名を .asset.json で終えてください";
            return false;
        }

        try
        {
            // 正規化したアセットルート
            const auto root = std::filesystem::weakly_canonical(
                m_graphics.Assets().AssetRoot());
            // 指定された親フォルダー
            const auto parent =
                (root / m_assetDirectory).lexically_normal();
            // 実体の作成先フォルダー
            const auto resolvedParent =
                std::filesystem::weakly_canonical(parent);
            if (!IsPathWithin(root, resolvedParent)
                || !std::filesystem::is_directory(parent)
                || std::filesystem::is_symlink(parent))
            {
                m_assetFileDialogError =
                    "アセットルート外には作成できません";
                return false;
            }

            // 新アセットの相対パス
            const auto relativePath =
                m_assetDirectory / PathFromUtf8(name);
            // 作成先の絶対パス
            const auto destination =
                (root / relativePath).lexically_normal();
            if (!IsPathWithin(root, destination)
                || std::filesystem::exists(destination))
            {
                m_assetFileDialogError =
                    "同じ名前のファイルが存在します";
                return false;
            }

            // 選択型の登録スキーマ
            const auto* schema = FindDataAssetSchema(
                m_createDataAssetTypeName);
            // 保存するデータJSON
            const auto document = MakeDataAssetDocument(
                m_createDataAssetTypeName,
                schema != nullptr
                    ? std::string_view{ *schema }
                    : std::string_view{});
            // データアセットの出力先
            std::ofstream output(
                destination,
                std::ios::binary | std::ios::trunc);
            if (!output)
            {
                m_assetFileDialogError =
                    "データアセットを作成できませんでした";
                return false;
            }
            output << document.dump(2) << '\n';
            if (!output)
            {
                m_assetFileDialogError =
                    "データアセットを書き込めませんでした";
                return false;
            }
            output.close();

            m_selectedAsset = relativePath;
            RefreshAssets();
            SetStatus(
                "データアセットを作成しました: "
                + PathToUtf8(relativePath));
            return true;
        }
        // 作成・変更処理の失敗原因
        catch (const std::exception& exception)
        {
            m_assetFileDialogError = exception.what();
            return false;
        }
    }

    // 作成先を検証し、グラフから生成したShaderまたは雛形を保存します。
    bool EditorLayer::CreateShaderAsset()
    {
        // 入力された作成・変更名
        const std::string name = m_assetFileNameBuffer.data();
        // 名前の検証エラー
        if (const auto validationError =
            ValidateAssetEntryName(name, "Shader名"))
        {
            m_assetFileDialogError = *validationError;
            return false;
        }
        if (!Lowercase(name).ends_with(".hlsl")
            || name.size() <= 5)
        {
            m_assetFileDialogError =
                "ファイル名を .hlsl で終えてください";
            return false;
        }

        try
        {
            // 正規化したアセットルート
            const auto root = std::filesystem::weakly_canonical(
                m_graphics.Assets().AssetRoot());
            // 指定された親フォルダー
            const auto parent =
                (root / m_assetDirectory).lexically_normal();
            // 実体の作成先フォルダー
            const auto resolvedParent =
                std::filesystem::weakly_canonical(parent);
            if (!IsPathWithin(root, resolvedParent)
                || !std::filesystem::is_directory(parent)
                || std::filesystem::is_symlink(parent))
            {
                m_assetFileDialogError =
                    "アセットルート外には作成できません";
                return false;
            }
            // 新アセットの相対パス
            const auto relativePath =
                m_assetDirectory / PathFromUtf8(name);
            // 作成先の絶対パス
            const auto destination =
                (root / relativePath).lexically_normal();
            if (!IsPathWithin(root, destination)
                || std::filesystem::exists(destination))
            {
                m_assetFileDialogError =
                    "同じ名前のファイルが存在します";
                return false;
            }
            if (m_createShaderFromGraph)
            {
                // 生成Shaderの出力先
                std::ofstream output(
                    destination,
                    std::ios::binary | std::ios::trunc);
                if (!output)
                {
                    throw std::runtime_error(
                        "生成したShaderを書き込めませんでした");
                }
                output << GenerateSimpleMaterialShader(
                    m_createShaderGraph);
                if (!output)
                {
                    throw std::runtime_error(
                        "生成したShaderを保存できませんでした");
                }
            }
            else
            {
                // コピーするShader雛形
                const auto shaderTemplate =
                    m_graphics.Assets().ResolvePath(
                        "shaders/LamaPonCustomMaterial.hlsl");
                if (!std::filesystem::copy_file(
                    shaderTemplate,
                    destination,
                    std::filesystem::copy_options::none))
                {
                    throw std::runtime_error(
                        "Shader雛形をコピーできませんでした");
                }
            }
            m_selectedAsset = relativePath;
            RefreshAssets();
            SetStatus(
                "カスタムShaderを作成しました: "
                + PathToUtf8(relativePath));
            return true;
        }
        // 作成・変更処理の失敗原因
        catch (const std::exception& exception)
        {
            m_assetFileDialogError = exception.what();
            return false;
        }
    }

    // クラス名と作成先を検証し、登録用ヘッダーを使うScript雛形を保存して開きます。
    bool EditorLayer::CreateCppScriptAsset()
    {
        // 入力された作成・変更名
        const std::string name = m_assetFileNameBuffer.data();
        // 名前の検証エラー
        if (const auto validationError =
            ValidateAssetEntryName(name, "C++ Script名"))
        {
            m_assetFileDialogError = *validationError;
            return false;
        }
        if (!Lowercase(name).ends_with(".cpp")
            || name.size() <= std::string_view{ ".cpp" }.size())
        {
            m_assetFileDialogError =
                "ファイル名を .cpp で終えてください";
            return false;
        }

        // Scriptのクラス名
        const std::string className =
            PathToUtf8(PathFromUtf8(name).stem());
        if (!IsCppIdentifier(className))
        {
            m_assetFileDialogError =
                "クラス名には半角英字・数字・アンダースコアを使用してください";
            return false;
        }

        try
        {
            // 正規化したアセットルート
            const auto root = std::filesystem::weakly_canonical(
                m_graphics.Assets().AssetRoot());
            // 指定された親フォルダー
            const auto parent =
                (root / m_assetDirectory).lexically_normal();
            // 実体の作成先フォルダー
            const auto resolvedParent =
                std::filesystem::weakly_canonical(parent);
            if (!IsPathWithin(root, resolvedParent)
                || !std::filesystem::is_directory(parent)
                || std::filesystem::is_symlink(parent))
            {
                m_assetFileDialogError =
                    "アセットルート外には作成できません";
                return false;
            }

            // 新アセットの相対パス
            const auto relativePath =
                m_assetDirectory / PathFromUtf8(name);
            // 作成先の絶対パス
            const auto destination =
                (root / relativePath).lexically_normal();
            if (!IsPathWithin(root, destination)
                || std::filesystem::exists(destination))
            {
                m_assetFileDialogError =
                    "同じ名前のファイルが存在します";
                return false;
            }

            // Script登録用ヘッダー
            const auto registryHeader =
                m_engineRoot
                / "tools"
                / "ProjectGameModule"
                / "ScriptRegistry.h";
            if (!std::filesystem::exists(registryHeader))
            {
                m_assetFileDialogError =
                    "Game ModuleのScriptRegistry.hが見つかりません";
                return false;
            }

            // Script雛形の出力先
            std::ofstream output(
                destination,
                std::ios::binary | std::ios::trunc);
            if (!output)
            {
                m_assetFileDialogError =
                    "C++ Scriptファイルを作成できませんでした";
                return false;
            }
            output << CreateCppScriptSource(className);
            if (!output)
            {
                m_assetFileDialogError =
                    "C++ Scriptファイルへ書き込めませんでした";
                return false;
            }
            output.close();

            m_selectedAsset = relativePath;
            RefreshAssets();
            SetStatus(
                "C++ Scriptを作成しました: "
                + PathToUtf8(relativePath)
                + "（編集後にGame Moduleをビルドしてください）");
            OpenCodeAsset(relativePath);
            return true;
        }
        // 作成・変更処理の失敗原因
        catch (const std::exception& exception)
        {
            m_assetFileDialogError = exception.what();
            return false;
        }
    }

    // フォルダー名を変更してJSONと編集中の参照を更新し、履歴を初期化します。
    bool EditorLayer::RenameAssetFolder()
    {
        if (m_assetDirectory.empty())
        {
            m_assetFolderDialogError =
                "assetsルートの名前は変更できません";
            return false;
        }

        // 入力された作成・変更名
        const std::string name = m_assetFolderNameBuffer.data();
        // 名前の検証エラー
        if (const auto validationError =
            ValidateAssetEntryName(name, "フォルダー名"))
        {
            m_assetFolderDialogError = *validationError;
            return false;
        }

        try
        {
            // 正規化したアセットルート
            const auto root = std::filesystem::weakly_canonical(
                m_graphics.Assets().AssetRoot());
            // 改名元の絶対パス
            const auto source =
                (root / m_assetDirectory).lexically_normal();
            // 改名元の実体パス
            const auto resolvedSource =
                std::filesystem::weakly_canonical(source);
            if (!IsPathWithin(root, resolvedSource)
                || !std::filesystem::is_directory(source)
                || std::filesystem::is_symlink(source))
            {
                m_assetFolderDialogError =
                    "管理対象外のフォルダーは変更できません";
                return false;
            }

            // 改名後の相対パス
            const auto newRelativeDirectory =
                m_assetDirectory.parent_path() / PathFromUtf8(name);
            if (newRelativeDirectory == m_assetDirectory)
            {
                return true;
            }

            // 改名先の絶対パス
            const auto destination =
                (source.parent_path() / PathFromUtf8(name)).lexically_normal();
            if (!IsPathWithin(root, destination))
            {
                m_assetFolderDialogError =
                    "アセットルート外へ移動できません";
                return false;
            }
            if (std::filesystem::exists(destination))
            {
                m_assetFolderDialogError =
                    "同じ名前のファイルまたはフォルダーが存在します";
                return false;
            }

            // フォルダー改名エラー
            std::error_code error;
            std::filesystem::rename(source, destination, error);
            if (error)
            {
                m_assetFolderDialogError =
                    "フォルダー名を変更できませんでした";
                return false;
            }

            // 改名後の参照更新に失敗しても、フォルダー名は元へ戻りません。
            // 改名前の相対パス
            const auto oldRelativeDirectory = m_assetDirectory;
            static_cast<void>(
                m_graphics.Assets().Database().Refresh(
                    true));
            // JSON参照の更新結果
            const auto remapResult =
                m_graphics.Assets().Database().
                    RemapJsonReferences(
                        oldRelativeDirectory,
                        newRelativeDirectory,
                        true);
            RemapAssetReferences(
                oldRelativeDirectory,
                newRelativeDirectory);
            m_assetDirectory = newRelativeDirectory;
            m_clipboardSceneJson.clear();
            m_clipboardObjectId = 0;
            RefreshAssets();
            ResetHistory();
            SetStatus(
                "フォルダー名を変更しました: "
                + PathToUtf8(m_assetDirectory)
                + "（JSON参照 "
                + std::to_string(
                    remapResult.referenceCount)
                + "件更新）");
            return true;
        }
        // 作成・変更処理の失敗原因
        catch (const std::exception& exception)
        {
            m_assetFolderDialogError = exception.what();
            return false;
        }
    }

    // 管理対象の空フォルダーだけを削除し、表示先を親へ切り替えます。
    bool EditorLayer::DeleteAssetFolder()
    {
        if (m_assetDirectory.empty())
        {
            m_assetFolderDialogError =
                "assetsルートは削除できません";
            return false;
        }

        try
        {
            // 正規化したアセットルート
            const auto root = std::filesystem::weakly_canonical(
                m_graphics.Assets().AssetRoot());
            // 削除対象の絶対パス
            const auto source =
                (root / m_assetDirectory).lexically_normal();
            // 削除対象の実体パス
            const auto resolvedSource =
                std::filesystem::weakly_canonical(source);
            if (!IsPathWithin(root, resolvedSource)
                || !std::filesystem::is_directory(source)
                || std::filesystem::is_symlink(source))
            {
                m_assetFolderDialogError =
                    "管理対象外のフォルダーは削除できません";
                return false;
            }

            // 空確認・削除のエラー
            std::error_code error;
            if (!std::filesystem::is_empty(source, error) || error)
            {
                m_assetFolderDialogError =
                    "フォルダーが空ではないため削除できません";
                return false;
            }
            if (!std::filesystem::remove(source, error) || error)
            {
                m_assetFolderDialogError =
                    "フォルダーを削除できませんでした";
                return false;
            }

            // 削除したフォルダー名
            const std::string deletedName =
                PathToUtf8(m_assetDirectory.filename());
            m_assetDirectory = m_assetDirectory.parent_path();
            m_selectedAsset.clear();
            RefreshAssets();
            SetStatus("フォルダーを削除しました: " + deletedName);
            return true;
        }
        // 作成・変更処理の失敗原因
        catch (const std::exception& exception)
        {
            m_assetFolderDialogError = exception.what();
            return false;
        }
    }

    // 元の拡張子を保つ名前を検証し、選択アセットを改名します。
    bool EditorLayer::RenameSelectedAsset()
    {
        if (m_selectedAsset.empty())
        {
            m_assetFileDialogError = "ファイルを選択してください";
            return false;
        }

        // 入力された新ファイル名
        const std::string name = m_assetFileNameBuffer.data();
        // 名前の検証エラー
        if (const auto validationError =
            ValidateAssetEntryName(name, "ファイル名"))
        {
            m_assetFileDialogError = *validationError;
            return false;
        }

        // 維持するアセット拡張子
        const std::string requiredExtension = IsSceneAsset(m_selectedAsset)
            ? ".scene.json"
            : IsPrefabAsset(m_selectedAsset)
                ? ".prefab.json"
                : IsAnimationAsset(m_selectedAsset)
                    ? ".animation.json"
                    : IsAnimatorControllerAsset(
                            m_selectedAsset)
                        ? ".animator.json"
                        : Lowercase(
                            LamaPon::PathToUtf8(m_selectedAsset.extension()));
        // 小文字化した新ファイル名
        const std::string lowercaseName = Lowercase(name);
        if (!requiredExtension.empty()
            && (!lowercaseName.ends_with(requiredExtension)
                || lowercaseName.size() <= requiredExtension.size()))
        {
            m_assetFileDialogError =
                "元の拡張子「"
                + requiredExtension
                + "」を維持してください";
            return false;
        }

        // 改名先の相対パス
        const auto destination =
            m_selectedAsset.parent_path() / PathFromUtf8(name);
        if (destination == m_selectedAsset)
        {
            return true;
        }
        if (!RelocateAssetFile(m_selectedAsset, destination))
        {
            m_assetFileDialogError = m_statusMessage;
            return false;
        }
        return true;
    }

    // 編集中のシーン・保存済みアセット・DBから削除対象の参照を集め、検査不能なら削除を止めます。
    void EditorLayer::RefreshAssetDeleteReferences()
    {
        m_assetDeleteReferences.clear();
        m_assetDeleteScanError.clear();

        if (m_selectedAsset.empty())
        {
            m_assetDeleteScanError =
                "削除対象のファイルが選択されていません。";
            return;
        }

        try
        {
            // 削除対象の絶対パス
            const auto selectedAbsolute =
                m_graphics.Assets().ResolvePath(m_selectedAsset);
            if (IsSceneAsset(m_selectedAsset)
                && !m_scenePath.empty()
                && NormalizeAssetReference(selectedAbsolute)
                    == NormalizeAssetReference(m_scenePath))
            {
                m_assetDeleteScanError =
                    "現在開いているシーンは削除できません。"
                    "別のシーンを開いてから削除してください。";
                return;
            }

            // 編集中の参照検査対象
            for (const auto& gameObject : m_scene.GameObjects())
            {
                if (gameObject->IsPrefabInstanceRoot()
                    && IsSameAssetReference(
                        gameObject->PrefabAssetPath(),
                        m_selectedAsset))
                {
                    m_assetDeleteReferences.push_back(
                        "現在のシーン / "
                        + gameObject->Name()
                        + " / Prefabリンク");
                }
                // 画像参照の検査対象
                if (const auto* sprite =
                    gameObject->GetComponent<SpriteRendererComponent>();
                    sprite != nullptr
                    && IsSameAssetReference(
                        sprite->TexturePath(),
                        m_selectedAsset))
                {
                    m_assetDeleteReferences.push_back(
                        "現在のシーン / "
                        + gameObject->Name()
                        + " / SpriteRenderer");
                }
                // タイル画像の検査対象
                if (const auto* tilemap =
                    gameObject->GetComponent<
                        TilemapComponent>();
                    tilemap != nullptr
                    && IsSameAssetReference(
                        tilemap->TexturePath(),
                        m_selectedAsset))
                {
                    m_assetDeleteReferences.push_back(
                        "現在のシーン / "
                        + gameObject->Name()
                        + " / Tilemap");
                }
                // 粒子画像・Shaderの検査対象
                if (const auto* particles =
                    gameObject->GetComponent<
                        ParticleSystemComponent>();
                    particles != nullptr
                    && (IsSameAssetReference(
                            particles->TexturePath(),
                            m_selectedAsset)
                        || IsSameAssetReference(
                            particles->ShaderPath(),
                            m_selectedAsset)
                        || IsSameAssetReference(
                            particles->AuxiliaryTexturePath(),
                            m_selectedAsset)))
                {
                    m_assetDeleteReferences.push_back(
                        "現在のシーン / "
                        + gameObject->Name()
                        + " / ParticleSystem");
                }
                // ボタン画像・遷移先の検査対象
                if (const auto* button =
                    gameObject->GetComponent<
                        UIButtonComponent>();
                    button != nullptr
                    && IsSameAssetReference(
                        button->TexturePath(),
                        m_selectedAsset))
                {
                    m_assetDeleteReferences.push_back(
                        "現在のシーン / "
                        + gameObject->Name()
                        + " / UIButton");
                }
                // ボタン画像・遷移先の検査対象
                if (const auto* button =
                    gameObject->GetComponent<
                        UIButtonComponent>();
                    button != nullptr
                    && IsSameAssetReference(
                        button->TargetScene(),
                        m_selectedAsset))
                {
                    m_assetDeleteReferences.push_back(
                        "現在のシーン / "
                        + gameObject->Name()
                        + " / UIButton / 移動先Scene");
                }
                // モデルと描画素材の検査対象
                if (const auto* model =
                    gameObject->GetComponent<ModelRendererComponent>();
                    model != nullptr)
                {
                    if (IsSameAssetReference(
                        model->ModelPath(),
                        m_selectedAsset))
                    {
                        m_assetDeleteReferences.push_back(
                            "現在のシーン / "
                            + gameObject->Name()
                            + " / ModelRenderer / モデル");
                    }
                    if (IsSameAssetReference(
                        model->AlbedoTexturePath(),
                        m_selectedAsset))
                    {
                        m_assetDeleteReferences.push_back(
                            "現在のシーン / "
                            + gameObject->Name()
                            + " / ModelRenderer / アルベド");
                    }
                    if (IsSameAssetReference(
                        model->NormalTexturePath(),
                        m_selectedAsset))
                    {
                        m_assetDeleteReferences.push_back(
                            "現在のシーン / "
                            + gameObject->Name()
                            + " / ModelRenderer / 法線マップ");
                    }
                    if (IsSameAssetReference(
                        model->MaterialAssetPath(),
                        m_selectedAsset))
                    {
                        m_assetDeleteReferences.push_back(
                            "現在のシーン / "
                            + gameObject->Name()
                            + " / ModelRenderer / Material");
                    }
                    if (IsSameAssetReference(
                        model->ShaderPath(),
                        m_selectedAsset))
                    {
                        m_assetDeleteReferences.push_back(
                            "現在のシーン / "
                            + gameObject->Name()
                            + " / ModelRenderer / Shader");
                    }
                    // label: マップ名、path: 参照パス
                    for (const auto& [label, path] :
                        PbrMapReferences(*model))
                    {
                        if (IsSameAssetReference(
                            *path,
                            m_selectedAsset))
                        {
                            m_assetDeleteReferences.push_back(
                                "現在のシーン / "
                                + gameObject->Name()
                                + " / ModelRenderer / "
                                + label);
                        }
                    }
                }
                // メッシュ描画素材の検査対象
                if (const auto* mesh =
                    gameObject->GetComponent<MeshRendererComponent>();
                    mesh != nullptr)
                {
                    if (IsSameAssetReference(
                        mesh->AlbedoTexturePath(),
                        m_selectedAsset))
                    {
                        m_assetDeleteReferences.push_back(
                            "現在のシーン / "
                            + gameObject->Name()
                            + " / MeshRenderer / アルベド");
                    }
                    if (IsSameAssetReference(
                        mesh->NormalTexturePath(),
                        m_selectedAsset))
                    {
                        m_assetDeleteReferences.push_back(
                            "現在のシーン / "
                            + gameObject->Name()
                            + " / MeshRenderer / 法線マップ");
                    }
                    if (IsSameAssetReference(
                        mesh->MaterialAssetPath(),
                        m_selectedAsset))
                    {
                        m_assetDeleteReferences.push_back(
                            "現在のシーン / "
                            + gameObject->Name()
                        + " / MeshRenderer / Material");
                    }
                    if (IsSameAssetReference(
                        mesh->ShaderPath(),
                        m_selectedAsset))
                    {
                        m_assetDeleteReferences.push_back(
                            "現在のシーン / "
                            + gameObject->Name()
                            + " / MeshRenderer / Shader");
                    }
                    // label: マップ名、path: 参照パス
                    for (const auto& [label, path] :
                        PbrMapReferences(*mesh))
                    {
                        if (IsSameAssetReference(
                            *path,
                            m_selectedAsset))
                        {
                            m_assetDeleteReferences.push_back(
                                "現在のシーン / "
                                + gameObject->Name()
                                + " / MeshRenderer / "
                                + label);
                        }
                    }
                }
                // 音声参照の検査対象
                if (const auto* audio =
                    gameObject->GetComponent<AudioSourceComponent>();
                    audio != nullptr
                    && IsSameAssetReference(
                        audio->AudioPath(),
                        m_selectedAsset))
                {
                    m_assetDeleteReferences.push_back(
                        "現在のシーン / "
                        + gameObject->Name()
                        + " / AudioSource");
                }
                // アニメーションの検査対象
                if (const auto* animator =
                    gameObject->GetComponent<
                        TransformAnimatorComponent>();
                    animator != nullptr
                    && IsSameAssetReference(
                        animator->ClipPath(),
                        m_selectedAsset))
                {
                    m_assetDeleteReferences.push_back(
                        "現在のシーン / "
                        + gameObject->Name()
                        + " / TransformAnimator");
                }
                // アニメーションの検査対象
                if (const auto* animator =
                    gameObject->GetComponent<
                        TransformAnimatorComponent>();
                    animator != nullptr
                    && IsSameAssetReference(
                        animator->ControllerPath(),
                        m_selectedAsset))
                {
                    m_assetDeleteReferences.push_back(
                        "現在のシーン / "
                        + gameObject->Name()
                        + " / AnimatorController");
                }
            }

            // 参照を調べるシーン・Prefab
            for (const auto& hierarchyAsset : m_assetFiles)
            {
                if ((!IsSceneAsset(hierarchyAsset)
                        && !IsPrefabAsset(hierarchyAsset))
                    || IsSameAssetReference(
                        hierarchyAsset,
                        m_selectedAsset))
                {
                    continue;
                }

                // 検査ファイルの絶対パス
                const auto hierarchyAbsolute =
                    m_graphics.Assets().ResolvePath(hierarchyAsset);
                if (IsSceneAsset(hierarchyAsset)
                    && !m_scenePath.empty()
                    && NormalizeAssetReference(hierarchyAbsolute)
                        == NormalizeAssetReference(m_scenePath))
                {
                    continue;
                }

                // 参照検査用の入力ファイル
                std::ifstream input(hierarchyAbsolute, std::ios::binary);
                if (!input)
                {
                    m_assetDeleteScanError =
                        "参照を確認できないシーンまたはPrefabがあります: "
                        + PathToUtf8(hierarchyAsset);
                    return;
                }

                // シーン・PrefabのJSON
                nlohmann::json hierarchyJson;
                // シーン・PrefabのJSON
                input >> hierarchyJson;
                // オブジェクト配列の位置
                const auto objects = hierarchyJson.find("objects");
                if (objects == hierarchyJson.end() || !objects->is_array())
                {
                    m_assetDeleteScanError =
                        "シーンまたはPrefab形式を確認できません: "
                        + PathToUtf8(hierarchyAsset);
                    return;
                }

                // 検査するオブジェクトJSON
                for (const auto& object : *objects)
                {
                    if (!object.is_object())
                    {
                        continue;
                    }
                    // 参照元オブジェクトの表示名
                    const std::string objectName =
                        object.value("name", "GameObject");
                    // コンポーネント配列の位置
                    const auto components = object.find("components");
                    if (components == object.end()
                        || !components->is_array())
                    {
                        continue;
                    }

                    // 検査するコンポーネントJSON
                    for (const auto& component : *components)
                    {
                        if (!component.is_object())
                        {
                            continue;
                        }

                        // 検査するコンポーネント型
                        const std::string type =
                            component.value("type", "");

                        // 型ごとの参照フィールド名
                        std::array<const char*, 10>
                            referenceFields{};
                        // 検査する参照フィールド数
                        std::size_t referenceFieldCount{};
                        if (type == "SpriteRenderer"
                            || type == "Tilemap"
                            || type == "ParticleSystem"
                            || type == "UIButton"
                            || type == "UIImage")
                        {
                            referenceFields[referenceFieldCount++] =
                                "texture";
                        }
                        if (type == "UIButton")
                        {
                            referenceFields[
                                referenceFieldCount++] =
                                    "targetScene";
                        }
                        else if (type == "MeshCollider3D")
                        {
                            referenceFields[
                                referenceFieldCount++] =
                                    "model";
                        }
                        else if (type == "ModelRenderer")
                        {
                            referenceFields[referenceFieldCount++] =
                                "model";
                            referenceFields[referenceFieldCount++] =
                                "albedoTexture";
                            referenceFields[referenceFieldCount++] =
                                "normalTexture";
                            referenceFields[referenceFieldCount++] =
                                "materialAsset";
                            referenceFields[referenceFieldCount++] =
                                "animationController";
                            referenceFields[referenceFieldCount++] =
                                "shader";
                            referenceFields[referenceFieldCount++] =
                                "roughnessTexture";
                            referenceFields[referenceFieldCount++] =
                                "metallicTexture";
                            referenceFields[referenceFieldCount++] =
                                "occlusionTexture";
                            referenceFields[referenceFieldCount++] =
                                "emissiveTexture";
                        }
                        else if (type == "MeshRenderer")
                        {
                            referenceFields[referenceFieldCount++] =
                                "albedoTexture";
                            referenceFields[referenceFieldCount++] =
                                "normalTexture";
                            referenceFields[referenceFieldCount++] =
                                "materialAsset";
                            referenceFields[referenceFieldCount++] =
                                "shader";
                            referenceFields[referenceFieldCount++] =
                                "roughnessTexture";
                            referenceFields[referenceFieldCount++] =
                                "metallicTexture";
                            referenceFields[referenceFieldCount++] =
                                "occlusionTexture";
                            referenceFields[referenceFieldCount++] =
                                "emissiveTexture";
                        }
                        else if (type == "AudioSource")
                        {
                            referenceFields[referenceFieldCount++] =
                                "audio";
                        }
                        else if (type
                            == "TransformAnimator")
                        {
                            referenceFields[referenceFieldCount++] =
                                "clip";
                            referenceFields[referenceFieldCount++] =
                                "controller";
                        }

                        // 参照フィールドの番号
                        for (std::size_t fieldIndex = 0;
                            // 検査する参照フィールド数
                            fieldIndex < referenceFieldCount;
                            ++fieldIndex)
                        {
                            // 検査する参照フィールド名
                            const char* referenceField =
                                referenceFields[fieldIndex];
                            // 参照値のJSON位置
                            const auto reference =
                                component.find(referenceField);
                            if (reference == component.end()
                                || !reference->is_string()
                                || reference->get_ref<
                                    const std::string&>().empty())
                            {
                                continue;
                            }
                            if (IsSameAssetReference(
                                PathFromUtf8(reference->get_ref<
                                    const std::string&>()),
                                m_selectedAsset))
                            {
                                m_assetDeleteReferences.push_back(
                                    PathToUtf8(hierarchyAsset)
                                    + " / "
                                    + objectName
                                    + " / "
                                    + type
                                    + " / "
                                    + referenceField);
                            }
                        }
                    }
                }
            }

            // 検査する制御器アセット
            for (const auto& controllerAsset :
                m_assetFiles)
            {
                if (!IsAnimatorControllerAsset(
                        controllerAsset)
                    || IsSameAssetReference(
                        controllerAsset,
                        m_selectedAsset))
                {
                    continue;
                }
                // 参照検査用の入力ファイル
                std::ifstream input(
                    m_graphics.Assets().ResolvePath(
                        controllerAsset),
                    std::ios::binary);
                if (!input)
                {
                    m_assetDeleteScanError =
                        "Animator Controllerを確認できません: "
                        + PathToUtf8(controllerAsset);
                    return;
                }
                // 制御器の参照検査用JSON
                nlohmann::json controllerJson;
                // 制御器の参照検査用JSON
                input >> controllerJson;
                // 参照を調べる状態JSON
                for (const auto& state :
                    controllerJson.value(
                        "states",
                        nlohmann::json::array()))
                {
                    // 状態のクリップ参照位置
                    const auto clip =
                        state.find("clip");
                    if (clip != state.end()
                        && clip->is_string()
                        && IsSameAssetReference(
                            PathFromUtf8(
                                clip->get_ref<
                                    const std::string&>()),
                            m_selectedAsset))
                    {
                        m_assetDeleteReferences.push_back(
                            PathToUtf8(
                                controllerAsset)
                            + " / State "
                            + state.value(
                                "name",
                                std::string{
                                    "Unnamed" })
                            + " / clip");
                    }
                    // 混合ツリーのJSON位置
                    if (const auto blendTree =
                            state.find("blendTree");
                        blendTree != state.end()
                            && blendTree->is_object())
                    {
                        // 混合ツリーの子JSON
                        for (const auto& child :
                            blendTree->value(
                                "children",
                                nlohmann::json::array()))
                        {
                            // 子のクリップ参照位置
                            const auto childClip =
                                child.find("clip");
                            if (childClip
                                    != child.end()
                                && childClip->is_string()
                                && IsSameAssetReference(
                                    PathFromUtf8(
                                        childClip->get_ref<
                                            const std::string&>()),
                                    m_selectedAsset))
                            {
                                m_assetDeleteReferences.
                                    push_back(
                                        PathToUtf8(
                                            controllerAsset)
                                        + " / State "
                                        + state.value(
                                            "name",
                                            std::string{
                                                "Unnamed" })
                                        + " / Blend Tree / clip");
                            }
                        }
                    }
                }
            }

            // 検査するMaterialアセット
            for (const auto& materialAsset : m_assetFiles)
            {
                if (!IsMaterialAsset(materialAsset)
                    || IsSameAssetReference(
                        materialAsset,
                        m_selectedAsset))
                {
                    continue;
                }

                // 参照を調べるMaterial
                const auto material = LoadLitMaterialAsset(
                    m_graphics.Assets().ResolvePath(
                        materialAsset),
                    &m_graphics.Assets().Database(),
                    &m_graphics.Assets());
                if (IsSameAssetReference(
                    material.AlbedoTexture(),
                    m_selectedAsset))
                {
                    m_assetDeleteReferences.push_back(
                        PathToUtf8(materialAsset)
                        + " / albedoTexture");
                }
                if (IsSameAssetReference(
                    material.NormalTexture(),
                    m_selectedAsset))
                {
                    m_assetDeleteReferences.push_back(
                        PathToUtf8(materialAsset)
                        + " / normalTexture");
                }
                // MaterialのPBR画像参照
                const std::array<
                    std::pair<
                        const char*,
                        const std::filesystem::path*>,
                    4> materialPbrMaps{
                        std::pair{
                            "roughnessTexture",
                            &material.RoughnessTexture()
                        },
                        std::pair{
                            "metallicTexture",
                            &material.MetallicTexture()
                        },
                        std::pair{
                            "occlusionTexture",
                            &material.OcclusionTexture()
                        },
                        std::pair{
                            "emissiveTexture",
                            &material.EmissiveTexture()
                        }
                    };
                // field: 参照名、path: 参照パス
                for (const auto& [field, path] :
                    materialPbrMaps)
                {
                    if (IsSameAssetReference(
                        *path,
                        m_selectedAsset))
                    {
                        m_assetDeleteReferences.push_back(
                            PathToUtf8(materialAsset)
                            + " / "
                            + field);
                    }
                }
            }

            static_cast<void>(
                m_graphics.Assets().Database().Refresh(
                    true));
            // 削除対象のDB登録情報
            if (const auto* selectedRecord =
                    m_graphics.Assets().Database().
                        FindByPath(m_selectedAsset))
            {
                // 依存アセットのGUID
                for (const auto& dependentGuid :
                    selectedRecord->dependents)
                {
                    // 依存アセットのDB登録情報
                    const auto* dependent =
                        m_graphics.Assets().Database().
                            FindByGuid(dependentGuid);
                    if (dependent == nullptr)
                    {
                        continue;
                    }
                    // 依存元の相対パス文字列
                    const auto dependentPath =
                        PathToUtf8(dependent->path);
                    // 依存元が編集中のシーン
                    const bool isCurrentScene =
                        !m_scenePath.empty()
                        && NormalizeAssetReference(
                            m_graphics.Assets().
                                ResolvePath(
                                    dependent->path))
                            == NormalizeAssetReference(
                                m_scenePath);
                    // 依存元の表示済みを調べます(entry: 既存の参照表示)。
                    const bool alreadyListed =
                        std::ranges::any_of(
                            m_assetDeleteReferences,
                            [&dependentPath,
                                isCurrentScene](
                                const std::string& entry)
                            {
                                return entry.starts_with(
                                        dependentPath)
                                    || (isCurrentScene
                                        && entry.starts_with(
                                            "現在のシーン"));
                            });
                    if (!alreadyListed)
                    {
                        m_assetDeleteReferences.push_back(
                            dependentPath
                            + " / Asset Database依存");
                    }
                }
            }
        }
        // 参照検査を中断した失敗原因
        catch (const std::exception& exception)
        {
            m_assetDeleteScanError =
                std::string{ "アセット参照を確認できません: " }
                + exception.what();
        }
    }

    // 参照確認後に選択アセットを退避・再読込検査し、削除と編集中の参照解除を行います。
    bool EditorLayer::DeleteSelectedAsset()
    {
        RefreshAssetDeleteReferences();
        if (!m_assetDeleteScanError.empty())
        {
            m_assetFileDialogError = m_assetDeleteScanError;
            return false;
        }
        if (!m_assetDeleteReferences.empty()
            && !m_assetDeleteAcknowledged)
        {
            m_assetFileDialogError =
                "参照切れの確認を有効にしてください。";
            return false;
        }
        if (m_selectedAsset.empty()
            || m_selectedAsset.is_absolute()
            || std::ranges::find(m_assetFiles, m_selectedAsset)
                == m_assetFiles.end())
        {
            m_assetFileDialogError =
                "削除対象のアセットが見つかりません。";
            return false;
        }

        // 削除対象の相対パス
        const auto deletedAsset = m_selectedAsset;
        try
        {
            // 正規化したアセットルート
            const auto root = std::filesystem::weakly_canonical(
                m_graphics.Assets().AssetRoot());
            // 削除対象の絶対パス
            const auto source =
                (root / deletedAsset).lexically_normal();
            // 削除対象のGUIDメタ情報
            const auto sourceMeta =
                AssetDatabase::MetaPathFor(source);
            // 削除対象の実体パス
            const auto resolvedSource =
                std::filesystem::weakly_canonical(source);
            if (!IsPathWithin(root, resolvedSource)
                || !std::filesystem::is_regular_file(source)
                || std::filesystem::is_symlink(source))
            {
                m_assetFileDialogError =
                    "管理対象外のファイルは削除できません。";
                return false;
            }

            // 依存検査中の退避パス
            std::filesystem::path stagingPath;
            // 退避名の重複回避番号
            for (std::size_t suffix = 0; suffix < 1000; ++suffix)
            {
                // 重複を避けた退避ファイル名
                std::wstring stagingName =
                    source.filename().wstring()
                    + L".lamapon-delete";
                if (suffix != 0)
                {
                    stagingName += L"-" + std::to_wstring(suffix);
                }
                // 退避先パスの候補
                const auto candidate =
                    source.parent_path() / stagingName;
                if (!std::filesystem::exists(candidate))
                {
                    stagingPath = candidate;
                    break;
                }
            }
            if (stagingPath.empty())
            {
                m_assetFileDialogError =
                    "削除用の一時ファイルを作成できません。";
                return false;
            }

            // 退避・削除の実行エラー
            std::error_code error;
            std::filesystem::rename(source, stagingPath, error);
            if (error)
            {
                m_assetFileDialogError =
                    "ファイルを削除用領域へ移動できませんでした。";
                return false;
            }

            try
            {
                m_graphics.Assets().Clear();

                // 削除後も必要なアセット
                for (const auto& asset : m_assetFiles)
                {
                    if (IsModelAsset(asset)
                        && !IsSameAssetReference(asset, deletedAsset))
                    {
                        static_cast<void>(
                            m_graphics.Assets().LoadModel(asset));
                    }
                }

                // 参照を読み直す対象
                for (const auto& gameObject : m_scene.GameObjects())
                {
                    // 画像参照の更新対象
                    if (const auto* sprite =
                        gameObject->GetComponent<SpriteRendererComponent>();
                        sprite != nullptr
                        && !sprite->TexturePath().empty()
                        && !IsSameAssetReference(
                            sprite->TexturePath(),
                            deletedAsset))
                    {
                        static_cast<void>(
                            m_graphics.Assets().LoadTexture(
                                sprite->TexturePath()));
                    }
                    // タイル画像の更新対象
                    if (const auto* tilemap =
                        gameObject->GetComponent<
                            TilemapComponent>();
                        tilemap != nullptr
                        && !tilemap->
                            TexturePath().empty()
                        && !IsSameAssetReference(
                            tilemap->TexturePath(),
                            deletedAsset))
                    {
                        static_cast<void>(
                            m_graphics.Assets().
                                LoadTexture(
                                    tilemap->
                                        TexturePath()));
                    }
                    // 粒子画像の更新対象
                    if (const auto* particles =
                        gameObject->GetComponent<
                            ParticleSystemComponent>();
                        particles != nullptr
                        && !particles->TexturePath().empty()
                        && !IsSameAssetReference(
                            particles->TexturePath(),
                            deletedAsset))
                    {
                        static_cast<void>(
                            m_graphics.Assets().LoadTexture(
                                particles->TexturePath()));
                    }
                    // メッシュ描画素材の更新対象
                    if (const auto* mesh =
                        gameObject->GetComponent<MeshRendererComponent>();
                        mesh != nullptr)
                    {
                        if (!mesh->AlbedoTexturePath().empty()
                            && !IsSameAssetReference(
                                mesh->AlbedoTexturePath(),
                                deletedAsset))
                        {
                            static_cast<void>(
                                m_graphics.Assets().LoadTexture(
                                    mesh->AlbedoTexturePath()));
                        }
                        if (!mesh->NormalTexturePath().empty()
                            && !IsSameAssetReference(
                                mesh->NormalTexturePath(),
                                deletedAsset))
                        {
                            static_cast<void>(
                            m_graphics.Assets().LoadTexture(
                                mesh->NormalTexturePath()));
                        }
                        // label: マップ名、path: 参照パス
                        for (const auto& [label, path] :
                            PbrMapReferences(*mesh))
                        {
                            static_cast<void>(label);
                            if (!path->empty()
                                && !IsSameAssetReference(
                                    *path,
                                    deletedAsset))
                            {
                                static_cast<void>(
                                    m_graphics.Assets()
                                        .LoadTexture(*path));
                            }
                        }
                    }
                    // モデル描画素材の更新対象
                    if (const auto* model =
                        gameObject->GetComponent<ModelRendererComponent>();
                        model != nullptr)
                    {
                        if (!model->AlbedoTexturePath().empty()
                            && !IsSameAssetReference(
                                model->AlbedoTexturePath(),
                                deletedAsset))
                        {
                            static_cast<void>(
                                m_graphics.Assets().LoadTexture(
                                    model->AlbedoTexturePath()));
                        }
                        if (!model->NormalTexturePath().empty()
                            && !IsSameAssetReference(
                                model->NormalTexturePath(),
                                deletedAsset))
                        {
                            static_cast<void>(
                                m_graphics.Assets().LoadTexture(
                                    model->NormalTexturePath()));
                        }
                        // label: マップ名、path: 参照パス
                        for (const auto& [label, path] :
                            PbrMapReferences(*model))
                        {
                            static_cast<void>(label);
                            if (!path->empty()
                                && !IsSameAssetReference(
                                    *path,
                                    deletedAsset))
                            {
                                static_cast<void>(
                                    m_graphics.Assets()
                                        .LoadTexture(*path));
                            }
                        }
                    }
                }
            }
            // 退避後の依存アセット再読込エラー
            catch (const std::exception& exception)
            {
                // 削除取消・設定復旧エラー
                std::error_code rollbackError;
                std::filesystem::rename(
                    stagingPath,
                    source,
                    rollbackError);
                m_graphics.Assets().Clear();
                // 参照を読み直す対象
                for (const auto& gameObject : m_scene.GameObjects())
                {
                    try
                    {
                        // 画像参照の更新対象
                        if (auto* sprite =
                            gameObject->GetComponent<
                                SpriteRendererComponent>();
                            sprite != nullptr
                            && !sprite->TexturePath().empty())
                        {
                            sprite->SetTexturePath(
                                sprite->TexturePath());
                        }
                        // タイル画像の更新対象
                        if (auto* tilemap =
                            gameObject->GetComponent<
                                TilemapComponent>();
                            tilemap != nullptr
                            && !tilemap->
                                TexturePath().empty())
                        {
                            tilemap->SetTexturePath(
                                tilemap->
                                    TexturePath());
                        }
                        // 粒子画像の更新対象
                        if (auto* particles =
                            gameObject->GetComponent<
                                ParticleSystemComponent>();
                            particles != nullptr
                            && !particles->TexturePath().empty())
                        {
                            particles->SetTexturePath(
                                particles->TexturePath());
                        }
                        // モデル描画素材の更新対象
                        if (auto* model =
                            gameObject->GetComponent<
                                ModelRendererComponent>();
                            model != nullptr)
                        {
                            if (!model->ModelPath().empty())
                            {
                                model->SetModelPath(
                                    model->ModelPath());
                            }
                            model->SetAlbedoTexturePath(
                                model->AlbedoTexturePath());
                            model->SetNormalTexturePath(
                                model->NormalTexturePath());
                            // PBR画像参照の取得・設定
                            for (const auto& accessor :
                                PbrMapAccessors<
                                    ModelRendererComponent>())
                            {
                                (model->*accessor.set)(
                                    (model->*accessor.get)());
                            }
                        }
                        // メッシュ描画素材の更新対象
                        if (auto* mesh =
                            gameObject->GetComponent<
                                MeshRendererComponent>())
                        {
                            mesh->SetAlbedoTexturePath(
                                mesh->AlbedoTexturePath());
                            mesh->SetNormalTexturePath(
                                mesh->NormalTexturePath());
                            // PBR画像参照の取得・設定
                            for (const auto& accessor :
                                PbrMapAccessors<
                                    MeshRendererComponent>())
                            {
                                (mesh->*accessor.set)(
                                    (mesh->*accessor.get)());
                            }
                        }
                    }
                    // 取消後の再読込失敗は無視して残りの対象を復旧します。
                    catch (const std::exception&)
                    {
                    }
                }

                m_assetFileDialogError =
                    rollbackError
                        ? std::string{
                            "依存確認とロールバックに失敗しました: "
                        } + exception.what()
                        : std::string{
                            "他のアセットが依存しているため削除を取り消しました: "
                        } + exception.what();
                return false;
            }

            // 削除対象が起動シーン
            // 本体を削除する前に起動シーン設定を保存し、削除失敗時に復旧を試みます。
            const bool deletingStartupScene =
                IsSameAssetReference(
                    deletedAsset,
                    m_projectSettings.startupScene);
            // 復旧用の起動シーン設定
            const auto previousStartupScene =
                m_projectSettings.startupScene;
            if (deletingStartupScene)
            {
                // 代替の保存済み起動シーン
                const auto replacementStartupScene =
                    m_scenePath.lexically_relative(root);
                if (replacementStartupScene.empty()
                    || replacementStartupScene.is_absolute()
                    || !std::filesystem::is_regular_file(
                        root / replacementStartupScene))
                {
                    std::filesystem::rename(
                        stagingPath,
                        source,
                        error);
                    m_assetFileDialogError =
                        "起動シーンを削除する前に、別の保存済みシーンを"
                        "開いてください。";
                    return false;
                }

                try
                {
                    m_projectSettings.startupScene =
                        replacementStartupScene;
                    SaveProjectConfiguration();
                }
                catch (...)
                {
                    m_projectSettings.startupScene =
                        previousStartupScene;
                    std::filesystem::rename(
                        stagingPath,
                        source,
                        error);
                    throw;
                }
            }

            if (!std::filesystem::remove(stagingPath, error) || error)
            {
                // 削除取消・設定復旧エラー
                std::error_code rollbackError;
                std::filesystem::rename(
                    stagingPath,
                    source,
                    rollbackError);
                if (deletingStartupScene)
                {
                    m_projectSettings.startupScene =
                        previousStartupScene;
                    try
                    {
                        SaveProjectConfiguration();
                    }
                    catch (...)
                    {
                        rollbackError = std::make_error_code(
                            std::errc::io_error);
                    }
                }
                m_graphics.Assets().Clear();
                m_assetFileDialogError =
                    rollbackError
                        ? "削除とロールバックに失敗しました。"
                        : "ファイルを削除できなかったため元に戻しました。";
                return false;
            }
            // 本体削除後の.meta削除失敗では、本体は元へ戻りません。
            if (std::filesystem::exists(sourceMeta))
            {
                std::filesystem::remove(
                    sourceMeta,
                    error);
                if (error)
                {
                    m_assetFileDialogError =
                        "アセットは削除されましたが.metaを削除できませんでした。";
                    return false;
                }
            }

            // 編集中に解除した参照数
            std::size_t clearedReferences{};
            // 参照を読み直す対象
            for (const auto& gameObject : m_scene.GameObjects())
            {
                // 画像参照の更新対象
                if (auto* sprite =
                    gameObject->GetComponent<SpriteRendererComponent>();
                    sprite != nullptr
                    && IsSameAssetReference(
                        sprite->TexturePath(),
                        deletedAsset))
                {
                    sprite->SetTexturePath({});
                    ++clearedReferences;
                }
                // タイル画像の更新対象
                if (auto* tilemap =
                    gameObject->GetComponent<
                        TilemapComponent>();
                    tilemap != nullptr
                    && IsSameAssetReference(
                        tilemap->TexturePath(),
                        deletedAsset))
                {
                    tilemap->SetTexturePath({});
                    ++clearedReferences;
                }
                // 粒子画像の更新対象
                if (auto* particles =
                    gameObject->GetComponent<
                        ParticleSystemComponent>();
                    particles != nullptr
                    && IsSameAssetReference(
                        particles->TexturePath(),
                        deletedAsset))
                {
                    particles->SetTexturePath({});
                    ++clearedReferences;
                }
                // ボタン画像・遷移先の更新対象
                if (auto* button =
                    gameObject->GetComponent<
                        UIButtonComponent>();
                    button != nullptr
                    && IsSameAssetReference(
                        button->TexturePath(),
                        deletedAsset))
                {
                    button->SetTexturePath({});
                    ++clearedReferences;
                }
                // ボタン画像・遷移先の更新対象
                if (auto* button =
                    gameObject->GetComponent<
                        UIButtonComponent>();
                    button != nullptr
                    && IsSameAssetReference(
                        button->TargetScene(),
                        deletedAsset))
                {
                    button->SetTargetScene({});
                    ++clearedReferences;
                }
                // モデル描画素材の更新対象
                if (auto* model =
                    gameObject->GetComponent<ModelRendererComponent>();
                    model != nullptr)
                {
                    if (IsSameAssetReference(
                        model->ModelPath(),
                        deletedAsset))
                    {
                        model->SetModelPath({});
                        ++clearedReferences;
                    }
                    if (IsSameAssetReference(
                        model->AlbedoTexturePath(),
                        deletedAsset))
                    {
                        model->SetAlbedoTexturePath({});
                        ++clearedReferences;
                    }
                    if (IsSameAssetReference(
                        model->NormalTexturePath(),
                        deletedAsset))
                    {
                        model->SetNormalTexturePath({});
                        ++clearedReferences;
                    }
                    if (IsSameAssetReference(
                        model->MaterialAssetPath(),
                        deletedAsset))
                    {
                        model->SetMaterialAssetPath({});
                        ++clearedReferences;
                    }
                    // PBR画像参照の取得・設定
                    for (const auto& accessor :
                        PbrMapAccessors<
                            ModelRendererComponent>())
                    {
                        if (IsSameAssetReference(
                            (model->*accessor.get)(),
                            deletedAsset))
                        {
                            (model->*accessor.set)({});
                            ++clearedReferences;
                        }
                    }
                }
                // メッシュ描画素材の更新対象
                if (auto* mesh =
                    gameObject->GetComponent<MeshRendererComponent>();
                    mesh != nullptr)
                {
                    if (IsSameAssetReference(
                        mesh->AlbedoTexturePath(),
                        deletedAsset))
                    {
                        mesh->SetAlbedoTexturePath({});
                        ++clearedReferences;
                    }
                    if (IsSameAssetReference(
                        mesh->NormalTexturePath(),
                        deletedAsset))
                    {
                        mesh->SetNormalTexturePath({});
                        ++clearedReferences;
                    }
                    if (IsSameAssetReference(
                        mesh->MaterialAssetPath(),
                        deletedAsset))
                    {
                        mesh->SetMaterialAssetPath({});
                        ++clearedReferences;
                    }
                    // PBR画像参照の取得・設定
                    for (const auto& accessor :
                        PbrMapAccessors<
                            MeshRendererComponent>())
                    {
                        if (IsSameAssetReference(
                            (mesh->*accessor.get)(),
                            deletedAsset))
                        {
                            (mesh->*accessor.set)({});
                            ++clearedReferences;
                        }
                    }
                }
            }

            m_selectedAsset.clear();
            m_clipboardSceneJson.clear();
            m_clipboardObjectId = 0;
            m_assetDeleteReferences.clear();
            m_assetDeleteScanError.clear();
            RefreshAssets();
            ResetHistory();

            SetStatus(
                "ファイルを削除しました: "
                + PathToUtf8(deletedAsset)
                + (clearedReferences == 0
                    ? std::string{}
                    : "（現在のシーンの参照を"
                        + std::to_string(clearedReferences)
                        + "件解除）"));
            return true;
        }
        // 削除処理を中断した失敗原因
        catch (const std::exception& exception)
        {
            m_assetFileDialogError = exception.what();
            return false;
        }
    }

    // 編集中にフォルダーと参照を移動し、履歴を初期化します(sourceDirectory: 移動元の相対パス, targetDirectory: 移動先の相対パス)。
    bool EditorLayer::MoveAssetFolder(
        const std::filesystem::path& sourceDirectory,
        const std::filesystem::path& targetDirectory)
    {
        if (m_playing)
        {
            return false;
        }
        if (sourceDirectory.empty()
            || sourceDirectory.is_absolute()
            || targetDirectory.is_absolute()
            || std::ranges::find(
                m_assetDirectories,
                sourceDirectory)
                == m_assetDirectories.end())
        {
            SetStatus(
                "移動対象のフォルダーが見つかりません",
                true);
            return false;
        }

        if (!targetDirectory.empty()
            && std::ranges::find(
                m_assetDirectories,
                targetDirectory)
                == m_assetDirectories.end())
        {
            SetStatus(
                "移動先のフォルダーが見つかりません",
                true);
            return false;
        }
        if (sourceDirectory.parent_path()
            == targetDirectory)
        {

            return true;
        }
        // 空パスはルートを表すため、絶対パス化せず自己・子孫への移動を判定します。
        if (RemapPathPrefix(
                targetDirectory,
                sourceDirectory,
                sourceDirectory).has_value())
        {
            SetStatus(
                "フォルダーを自分自身の中へは移動できません",
                true);
            return false;
        }

        // 移動先フォルダーの相対パス
        const auto destinationRelative =
            targetDirectory / sourceDirectory.filename();
        try
        {
            // 正規化したアセットルート
            const auto root =
                std::filesystem::weakly_canonical(
                    m_graphics.Assets().AssetRoot());
            // 移動元の絶対パス
            const auto source =
                (root / sourceDirectory).lexically_normal();
            // 移動先の絶対パス
            const auto destination =
                (root / destinationRelative).
                    lexically_normal();
            // 移動元の実体パス
            const auto resolvedSource =
                std::filesystem::weakly_canonical(source);
            if (!IsPathWithin(root, resolvedSource)
                || !std::filesystem::is_directory(source)
                || std::filesystem::is_symlink(source))
            {
                SetStatus(
                    "管理対象外のフォルダーは移動できません",
                    true);
                return false;
            }
            if (!IsPathWithin(root, destination))
            {
                SetStatus(
                    "アセットルート外へ移動できません",
                    true);
                return false;
            }
            if (std::filesystem::exists(destination))
            {
                SetStatus(
                    "移動先に同じ名前のフォルダーまたはファイルが"
                    "存在します",
                    true);
                return false;
            }

            // アセット移動の実行エラー
            std::error_code error;
            std::filesystem::rename(
                source,
                destination,
                error);
            if (error)
            {
                SetStatus(
                    "フォルダーを移動できませんでした",
                    true);
                return false;
            }

            // 移動後の参照更新に失敗しても、フォルダーは元へ戻りません。
            static_cast<void>(
                m_graphics.Assets().Database().Refresh(
                    true));
            // JSON参照の更新結果
            const auto remapResult =
                m_graphics.Assets().Database().
                    RemapJsonReferences(
                        sourceDirectory,
                        destinationRelative,
                        true);
            RemapAssetReferences(
                sourceDirectory,
                destinationRelative);
            // 表示先を追従する相対パス
            if (const auto remapped = RemapPathPrefix(
                    m_assetDirectory,
                    sourceDirectory,
                    destinationRelative))
            {

                m_assetDirectory = *remapped;
            }
            m_clipboardSceneJson.clear();
            m_clipboardObjectId = 0;
            RefreshAssets();
            ResetHistory();
            SetStatus(
                "フォルダーを移動しました: "
                + PathToUtf8(sourceDirectory)
                + " → "
                + PathToUtf8(destinationRelative)
                + "（JSON参照 "
                + std::to_string(
                    remapResult.referenceCount)
                + "件更新）");
            return true;
        }
        // 移動・保存処理を中断した失敗原因
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
            return false;
        }
    }

    // 同じファイル名でアセットを移動します(sourceAsset: 移動元の相対パス, targetDirectory: 移動先の相対パス)。
    bool EditorLayer::MoveAssetFile(
        const std::filesystem::path& sourceAsset,
        const std::filesystem::path& targetDirectory)
    {
        return RelocateAssetFile(
            sourceAsset,
            targetDirectory / sourceAsset.filename());
    }

    // アセットとGUIDメタ情報を移動し、再読込検査と参照更新を行います(sourceAsset: 移動元の相対パス, destinationAsset: 移動先の相対パス)。
    bool EditorLayer::RelocateAssetFile(
        const std::filesystem::path& sourceAsset,
        const std::filesystem::path& destinationAsset)
    {
        if (sourceAsset.empty()
            || sourceAsset.is_absolute()
            || destinationAsset.empty()
            || destinationAsset.is_absolute()
            || std::ranges::find(m_assetFiles, sourceAsset)
                == m_assetFiles.end())
        {
            SetStatus("移動対象のアセットが見つかりません", true);
            return false;
        }
        if (sourceAsset == destinationAsset)
        {
            return true;
        }

        try
        {
            // 正規化したアセットルート
            const auto root = std::filesystem::weakly_canonical(
                m_graphics.Assets().AssetRoot());
            // 移動元の絶対パス
            const auto source =
                (root / sourceAsset).lexically_normal();
            // 移動先の絶対パス
            const auto destination =
                (root / destinationAsset).lexically_normal();
            // 移動先の親フォルダー
            const auto destinationDirectory = destination.parent_path();
            // 移動元のGUIDメタ情報
            const auto sourceMeta =
                AssetDatabase::MetaPathFor(source);
            // 移動先のGUIDメタ情報
            const auto destinationMeta =
                AssetDatabase::MetaPathFor(
                    destination);
            // 移動元の実体パス
            const auto resolvedSource =
                std::filesystem::weakly_canonical(source);
            // 移動先フォルダーの実体パス
            const auto resolvedDestinationDirectory =
                std::filesystem::weakly_canonical(destinationDirectory);

            if (!IsPathWithin(root, resolvedSource)
                || !IsPathWithin(root, resolvedDestinationDirectory)
                || !std::filesystem::is_regular_file(source)
                || std::filesystem::is_symlink(source)
                || !std::filesystem::is_directory(destinationDirectory)
                || std::filesystem::is_symlink(destinationDirectory))
            {
                SetStatus(
                    "管理対象外のファイルは移動できません",
                    true);
                return false;
            }
            if (std::filesystem::exists(destination))
            {
                SetStatus(
                    "移動先に同じ名前のファイルが存在します",
                    true);
                return false;
            }
            if (std::filesystem::exists(destinationMeta))
            {
                SetStatus(
                    "移動先に同名の.metaファイルが存在します",
                    true);
                return false;
            }

            // アセット移動の実行エラー
            std::error_code error;
            std::filesystem::rename(source, destination, error);
            if (error)
            {
                SetStatus("ファイルを移動できませんでした", true);
                return false;
            }
            // 移動元にGUIDメタ情報がある
            const bool hadMeta =
                std::filesystem::is_regular_file(
                    sourceMeta);
            if (hadMeta)
            {
                std::filesystem::rename(
                    sourceMeta,
                    destinationMeta,
                    error);
                if (error)
                {
                    // 移動を取り消す復旧エラー
                    std::error_code rollbackError;
                    std::filesystem::rename(
                        destination,
                        source,
                        rollbackError);
                    SetStatus(
                        "GUIDメタデータを移動できないため取り消しました",
                        true);
                    return false;
                }
            }

            // 更新したJSON参照数
            std::size_t remappedReferenceCount{};
            try
            {
                static_cast<void>(
                    m_graphics.Assets().Database().
                        Refresh(true));
                m_graphics.Assets().Clear();
                if (IsTextureAsset(destinationAsset))
                {
                    static_cast<void>(
                        m_graphics.Assets().LoadTexture(destinationAsset));
                }
                else if (IsModelAsset(destinationAsset))
                {
                    static_cast<void>(
                        m_graphics.Assets().LoadModel(destinationAsset));
                }

                // 移動後の参照を検査する対象
                for (const auto& gameObject : m_scene.GameObjects())
                {
                    // 画像参照の更新対象
                    if (const auto* sprite =
                        gameObject->GetComponent<SpriteRendererComponent>();
                        sprite != nullptr
                        && !sprite->TexturePath().empty())
                    {
                        // 移動後に読むアセットパス
                        const auto prospectivePath =
                            sprite->TexturePath() == sourceAsset
                                ? destinationAsset
                                : sprite->TexturePath();
                        static_cast<void>(
                            m_graphics.Assets().LoadTexture(
                                prospectivePath));
                    }
                    // タイル画像の更新対象
                    if (const auto* tilemap =
                        gameObject->GetComponent<
                            TilemapComponent>();
                        tilemap != nullptr
                        && !tilemap->
                            TexturePath().empty())
                    {
                        // 移動後に読むアセットパス
                        const auto prospectivePath =
                            tilemap->TexturePath()
                                == sourceAsset
                            ? destinationAsset
                            : tilemap->TexturePath();
                        static_cast<void>(
                            m_graphics.Assets().
                                LoadTexture(
                                    prospectivePath));
                    }
                    // 粒子画像の更新対象
                    if (const auto* particles =
                        gameObject->GetComponent<
                            ParticleSystemComponent>();
                        particles != nullptr
                        && !particles->TexturePath().empty())
                    {
                        // 移動後に読むアセットパス
                        const auto prospectivePath =
                            particles->TexturePath()
                                == sourceAsset
                            ? destinationAsset
                            : particles->TexturePath();
                        static_cast<void>(
                            m_graphics.Assets().LoadTexture(
                                prospectivePath));
                    }
                    // モデル描画素材の更新対象
                    if (const auto* model =
                        gameObject->GetComponent<ModelRendererComponent>();
                        model != nullptr)
                    {
                        if (!model->ModelPath().empty())
                        {
                            // 移動後に読むアセットパス
                            const auto prospectivePath =
                                model->ModelPath() == sourceAsset
                                    ? destinationAsset
                                    : model->ModelPath();
                            static_cast<void>(
                                m_graphics.Assets().LoadModel(
                                    prospectivePath));
                        }
                        if (!model->AlbedoTexturePath().empty())
                        {
                            // 移動後に読むアセットパス
                            const auto prospectivePath =
                                model->AlbedoTexturePath()
                                    == sourceAsset
                                    ? destinationAsset
                                    : model->AlbedoTexturePath();
                            static_cast<void>(
                                m_graphics.Assets().LoadTexture(
                                    prospectivePath));
                        }
                        if (!model->NormalTexturePath().empty())
                        {
                            // 移動後に読むアセットパス
                            const auto prospectivePath =
                                model->NormalTexturePath()
                                    == sourceAsset
                                    ? destinationAsset
                                    : model->NormalTexturePath();
                            static_cast<void>(
                                m_graphics.Assets().LoadTexture(
                                    prospectivePath));
                        }
                        // label: マップ名、path: 参照パス
                        for (const auto& [label, path] :
                            PbrMapReferences(*model))
                        {
                            static_cast<void>(label);
                            if (path->empty())
                            {
                                continue;
                            }
                            static_cast<void>(
                                m_graphics.Assets().LoadTexture(
                                    *path == sourceAsset
                                        ? destinationAsset
                                        : *path));
                        }
                    }
                    // メッシュ描画素材の更新対象
                    if (const auto* mesh =
                        gameObject->GetComponent<MeshRendererComponent>();
                        mesh != nullptr)
                    {
                        if (!mesh->AlbedoTexturePath().empty())
                        {
                            // 移動後に読むアセットパス
                            const auto prospectivePath =
                                mesh->AlbedoTexturePath()
                                    == sourceAsset
                                    ? destinationAsset
                                    : mesh->AlbedoTexturePath();
                            static_cast<void>(
                                m_graphics.Assets().LoadTexture(
                                    prospectivePath));
                        }
                        if (!mesh->NormalTexturePath().empty())
                        {
                            // 移動後に読むアセットパス
                            const auto prospectivePath =
                                mesh->NormalTexturePath()
                                    == sourceAsset
                                    ? destinationAsset
                                    : mesh->NormalTexturePath();
                            static_cast<void>(
                                m_graphics.Assets().LoadTexture(
                                    prospectivePath));
                        }
                        // label: マップ名、path: 参照パス
                        for (const auto& [label, path] :
                            PbrMapReferences(*mesh))
                        {
                            static_cast<void>(label);
                            if (path->empty())
                            {
                                continue;
                            }
                            static_cast<void>(
                                m_graphics.Assets().LoadTexture(
                                    *path == sourceAsset
                                        ? destinationAsset
                                        : *path));
                        }
                    }
                }

                // ここから参照を書き換えるため、後続の失敗ではファイル復旧だけで参照を元に戻せません。
                RemapAssetFileReferences(
                    sourceAsset,
                    destinationAsset);

                // 移動後の参照を検査する対象
                for (const auto& gameObject : m_scene.GameObjects())
                {
                    // 画像参照の更新対象
                    if (auto* sprite =
                        gameObject->GetComponent<SpriteRendererComponent>();
                        sprite != nullptr
                        && !sprite->TexturePath().empty())
                    {
                        sprite->SetTexturePath(sprite->TexturePath());
                    }
                    // タイル画像の更新対象
                    if (auto* tilemap =
                        gameObject->GetComponent<
                            TilemapComponent>();
                        tilemap != nullptr
                        && !tilemap->
                            TexturePath().empty())
                    {
                        tilemap->SetTexturePath(
                            tilemap->TexturePath());
                    }
                    // 粒子画像の更新対象
                    if (auto* particles =
                        gameObject->GetComponent<
                            ParticleSystemComponent>();
                        particles != nullptr
                        && !particles->TexturePath().empty())
                    {
                        particles->SetTexturePath(
                            particles->TexturePath());
                    }
                    // モデル描画素材の更新対象
                    if (auto* model =
                        gameObject->GetComponent<ModelRendererComponent>();
                        model != nullptr)
                    {
                        if (!model->ModelPath().empty())
                        {
                            model->SetModelPath(
                                model->ModelPath());
                        }
                        model->SetAlbedoTexturePath(
                            model->AlbedoTexturePath());
                        model->SetNormalTexturePath(
                            model->NormalTexturePath());
                        // PBR画像参照の取得・設定
                        for (const auto& accessor :
                            PbrMapAccessors<
                                ModelRendererComponent>())
                        {
                            (model->*accessor.set)(
                                (model->*accessor.get)());
                        }
                    }
                    // メッシュ描画素材の更新対象
                    if (auto* mesh =
                        gameObject->GetComponent<MeshRendererComponent>())
                    {
                        mesh->SetAlbedoTexturePath(
                            mesh->AlbedoTexturePath());
                        mesh->SetNormalTexturePath(
                            mesh->NormalTexturePath());
                        // PBR画像参照の取得・設定
                        for (const auto& accessor :
                            PbrMapAccessors<
                                MeshRendererComponent>())
                        {
                            (mesh->*accessor.set)(
                                (mesh->*accessor.get)());
                        }
                    }
                }
                // JSON参照の更新結果
                const auto remapResult =
                    m_graphics.Assets().Database().
                        RemapJsonReferences(
                            sourceAsset,
                            destinationAsset);
                remappedReferenceCount =
                    remapResult.referenceCount;
            }
            // 移動後の参照更新・再読込エラー
            catch (const std::exception& exception)
            {
                // 移動を取り消す復旧エラー
                std::error_code rollbackError;
                std::filesystem::rename(
                    destination,
                    source,
                    rollbackError);
                if (hadMeta)
                {
                    // GUIDメタ情報の復旧エラー
                    std::error_code metaRollbackError;
                    std::filesystem::rename(
                        destinationMeta,
                        sourceMeta,
                        metaRollbackError);
                    if (metaRollbackError)
                    {
                        rollbackError =
                            metaRollbackError;
                    }
                }
                static_cast<void>(
                    m_graphics.Assets().Database().
                        Refresh(true));
                m_graphics.Assets().Clear();
                if (!rollbackError)
                {
                    // 移動後の参照を検査する対象
                    for (const auto& gameObject : m_scene.GameObjects())
                    {
                        try
                        {
                            // 画像参照の更新対象
                            if (auto* sprite =
                                gameObject->GetComponent<
                                    SpriteRendererComponent>();
                                sprite != nullptr
                                && !sprite->TexturePath().empty())
                            {
                                sprite->SetTexturePath(
                                    sprite->TexturePath());
                            }
                            // モデル描画素材の更新対象
                            if (auto* model =
                                gameObject->GetComponent<
                                    ModelRendererComponent>();
                                model != nullptr)
                            {
                                if (!model->ModelPath().empty())
                                {
                                    model->SetModelPath(
                                        model->ModelPath());
                                }
                                model->SetAlbedoTexturePath(
                                    model->AlbedoTexturePath());
                                model->SetNormalTexturePath(
                                    model->NormalTexturePath());
                                // PBR画像参照の取得・設定
                                for (const auto& accessor :
                                    PbrMapAccessors<
                                        ModelRendererComponent>())
                                {
                                    (model->*accessor.set)(
                                        (model->*accessor.get)());
                                }
                            }
                            // メッシュ描画素材の更新対象
                            if (auto* mesh =
                                gameObject->GetComponent<
                                    MeshRendererComponent>())
                            {
                                mesh->SetAlbedoTexturePath(
                                    mesh->AlbedoTexturePath());
                                mesh->SetNormalTexturePath(
                                    mesh->NormalTexturePath());
                                // PBR画像参照の取得・設定
                                for (const auto& accessor :
                                    PbrMapAccessors<
                                        MeshRendererComponent>())
                                {
                                    (mesh->*accessor.set)(
                                        (mesh->*accessor.get)());
                                }
                            }
                        }
                        // 取消後の再読込失敗は無視して残りの対象を復旧します。
                        catch (const std::exception&)
                        {
                        }
                    }
                }
                SetStatus(
                    rollbackError
                        ? std::string{
                            "参照更新とロールバックに失敗しました: "
                        } + exception.what()
                        : std::string{
                            "参照を更新できないため移動を取り消しました: "
                        } + exception.what(),
                    true);
                return false;
            }

            // 移動元と移動先が同じ親
            const bool renamed =
                sourceAsset.parent_path()
                    == destinationAsset.parent_path();
            m_assetDirectory = destinationAsset.parent_path();
            m_selectedAsset = destinationAsset;
            m_clipboardSceneJson.clear();
            m_clipboardObjectId = 0;
            RefreshAssets();
            ResetHistory();
            SetStatus(
                std::string{
                    renamed
                        ? "ファイル名を変更しました: "
                        : "ファイルを移動しました: "
                }
                + PathToUtf8(destinationAsset)
                + "（GUID維持・JSON参照 "
                + std::to_string(
                    remappedReferenceCount)
                + "件更新）");
            return true;
        }
        // 移動・保存処理を中断した失敗原因
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
            return false;
        }
    }

    // フォルダーの相対パスを左ドラッグで渡します(directory: 対象フォルダーの相対パス)。
    void EditorLayer::BeginAssetFolderDragSource(
        const std::filesystem::path& directory)
    {
        if (m_playing || directory.empty())
        {
            return;
        }

        if (!ImGui::BeginDragDropSource())
        {
            return;
        }

        // フォルダー相対パスのUTF8
        const auto payload = PathToUtf8(directory);
        ImGui::SetDragDropPayload(
            AssetFolderPayload,
            payload.c_str(),
            payload.size() + 1);
        ImGui::Text(
            "フォルダーを移動: %s",
            PathToUtf8(directory.filename()).c_str());
        ImGui::TextDisabled("中身もまとめて移動します");
        ImGui::EndDragDropSource();
    }

    // ファイル・フォルダー移動とGameObjectのPrefab作成を受け付けます(targetDirectory: ドロップ先の相対パス)。
    void EditorLayer::AcceptAssetMoveDrop(
        const std::filesystem::path& targetDirectory)
    {
        if (m_playing || !ImGui::BeginDragDropTarget())
        {
            return;
        }

        // 受け取った移動・作成データ
        if (const ImGuiPayload* payload =
            ImGui::AcceptDragDropPayload(AssetPayload))
        {
            // 移動元の相対パス
            const auto source = PathFromUtf8(
                static_cast<const char*>(payload->Data));
            static_cast<void>(
                MoveAssetFile(source, targetDirectory));
        }
        // 受け取った移動・作成データ
        if (const ImGuiPayload* payload =
            ImGui::AcceptDragDropPayload(
                AssetFolderPayload))
        {
            // 移動元の相対パス
            const auto source = PathFromUtf8(
                static_cast<const char*>(payload->Data));
            static_cast<void>(
                MoveAssetFolder(source, targetDirectory));
        }
        // 受け取った移動・作成データ
        if (const ImGuiPayload* payload =
            ImGui::AcceptDragDropPayload(GameObjectPayload))
        {
            // Prefab化する対象ID
            GameObjectId droppedId{};
            std::memcpy(
                &droppedId,
                payload->Data,
                sizeof(droppedId));
            CreatePrefabFromGameObject(
                droppedId,
                targetDirectory);
        }
        ImGui::EndDragDropTarget();
    }

    // 対象をPrefabとして保存し、選択と履歴を更新します(id: 対象オブジェクトID, targetDirectory: 保存先の相対パス)。
    void EditorLayer::CreatePrefabFromGameObject(
        const GameObjectId id,
        const std::filesystem::path& targetDirectory)
    {
        // Prefab化する対象
        auto* gameObject = m_scene.FindGameObject(id);
        if (gameObject == nullptr || m_playing)
        {
            return;
        }

        try
        {
            // アセットのルートパス
            const auto root =
                m_graphics.Assets().AssetRoot();
            // 対象名から作るファイル名
            const auto fileName = SuggestedPrefabFileStem(gameObject->Name());
            // 作成先Prefabの相対パス
            auto relative = targetDirectory
                / (fileName + L".prefab.json");
            // 同名を避ける連番
            for (int suffix = 2;
                std::filesystem::exists(root / relative)
                    && suffix < 1000;
                ++suffix)
            {
                relative = targetDirectory
                    / (fileName
                        + L" ("
                        + std::to_wstring(suffix)
                        + L").prefab.json");
            }

            // 保存先の絶対パス
            const auto destination = root / relative;
            std::filesystem::create_directories(
                destination.parent_path());
            m_scene.SavePrefab(*gameObject, destination);
            m_selectedAsset = relative;
            RefreshAssets();
            RecordHistory();
            SetStatus(
                "Prefabを作成しました: "
                + PathToUtf8(relative));
        }
        // 移動・保存処理を中断した失敗原因
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    // 編集中の参照と起動シーンのパス接頭辞を置換します(oldDirectory: 移動元の相対パス, newDirectory: 移動先の相対パス)。
    void EditorLayer::RemapAssetReferences(
        const std::filesystem::path& oldDirectory,
        const std::filesystem::path& newDirectory)
    {
        // 接頭辞を置換した参照パス
        if (const auto remapped =
            RemapPathPrefix(
                m_selectedAsset,
                oldDirectory,
                newDirectory))
        {
            m_selectedAsset = *remapped;
        }

        // アセットのルートパス
        const auto root = m_graphics.Assets().AssetRoot();
        // 移動前の絶対パス
        const auto oldAbsolute = root / oldDirectory;
        // 移動後の絶対パス
        const auto newAbsolute = root / newDirectory;
        // 接頭辞を置換した参照パス
        if (const auto remapped =
            RemapPathPrefix(
                m_scenePath,
                oldAbsolute,
                newAbsolute))
        {
            m_scenePath = *remapped;
        }

        // 接頭辞を置換した参照パス
        if (const auto remapped =
            RemapPathPrefix(
                m_projectSettings.startupScene,
                oldDirectory,
                newDirectory))
        {
            m_projectSettings.startupScene = *remapped;
            // 設定保存に失敗しても、先に更新したメモリ内のパスは元へ戻りません。
            SaveProjectConfiguration();
        }

        // 参照パスを更新する対象
        for (const auto& gameObject : m_scene.GameObjects())
        {
            // 接頭辞を置換した参照パス
            if (const auto remapped =
                RemapPathPrefix(
                    gameObject->PrefabAssetPath(),
                    oldDirectory,
                    newDirectory))
            {
                gameObject->SetPrefabAssetPath(
                    *remapped);
            }
            // 画像・Shader参照の更新対象
            if (auto* sprite =
                gameObject->GetComponent<SpriteRendererComponent>())
            {
                // 接頭辞を置換した参照パス
                if (const auto remapped =
                    RemapPathPrefix(
                        sprite->TexturePath(),
                        oldDirectory,
                        newDirectory))
                {
                    sprite->SetTexturePath(*remapped);
                }
                // 接頭辞を置換した参照パス
                if (const auto remapped =
                    RemapPathPrefix(
                        sprite->ShaderPath(),
                        oldDirectory,
                        newDirectory))
                {
                    sprite->SetShaderPath(*remapped);
                }
            }
            // タイル画像の更新対象
            if (auto* tilemap =
                gameObject->GetComponent<
                    TilemapComponent>())
            {
                // 接頭辞を置換した参照パス
                if (const auto remapped =
                    RemapPathPrefix(
                        tilemap->TexturePath(),
                        oldDirectory,
                        newDirectory))
                {
                    tilemap->SetTexturePath(
                        *remapped);
                }
            }
            // 粒子画像・Shaderの更新対象
            if (auto* particles =
                gameObject->GetComponent<
                    ParticleSystemComponent>())
            {
                // 接頭辞を置換した参照パス
                if (const auto remapped =
                    RemapPathPrefix(
                        particles->TexturePath(),
                        oldDirectory,
                        newDirectory))
                {
                    particles->SetTexturePath(
                        *remapped);
                }
                // 接頭辞を置換した参照パス
                if (const auto remapped =
                    RemapPathPrefix(
                        particles->ShaderPath(),
                        oldDirectory,
                        newDirectory))
                {
                    particles->SetShaderPath(
                        *remapped);
                }
                // 接頭辞を置換した参照パス
                if (const auto remapped =
                    RemapPathPrefix(
                        particles->AuxiliaryTexturePath(),
                        oldDirectory,
                        newDirectory))
                {
                    particles->SetAuxiliaryTexturePath(
                        *remapped);
                }
            }
            // ボタン画像・遷移先の更新対象
            if (auto* button =
                gameObject->GetComponent<
                    UIButtonComponent>())
            {
                // 接頭辞を置換した参照パス
                if (const auto remapped =
                    RemapPathPrefix(
                        button->TexturePath(),
                        oldDirectory,
                        newDirectory))
                {
                    button->SetTexturePath(
                        *remapped);
                }
                // 接頭辞を置換した参照パス
                if (const auto remapped =
                    RemapPathPrefix(
                        button->TargetScene(),
                        oldDirectory,
                        newDirectory))
                {
                    button->SetTargetScene(
                        *remapped);
                }
            }
            // アニメーションの更新対象
            if (auto* animator =
                gameObject->GetComponent<
                    TransformAnimatorComponent>())
            {
                // 接頭辞を置換した参照パス
                if (const auto remapped =
                    RemapPathPrefix(
                        animator->ClipPath(),
                        oldDirectory,
                        newDirectory))
                {
                    animator->SetClipPath(
                        *remapped);
                }
                // 接頭辞を置換した参照パス
                if (const auto remapped =
                    RemapPathPrefix(
                        animator->ControllerPath(),
                        oldDirectory,
                        newDirectory))
                {
                    animator->SetControllerPath(
                        *remapped);
                }
            }
            // モデル描画素材の更新対象
            if (auto* model =
                gameObject->GetComponent<ModelRendererComponent>())
            {
                // 接頭辞を置換した参照パス
                if (const auto remapped =
                    RemapPathPrefix(
                        model->AnimationControllerPath(),
                        oldDirectory,
                        newDirectory))
                {
                    model->SetAnimationControllerPath(
                        *remapped);
                }
                // 接頭辞を置換した参照パス
                if (const auto remapped =
                    RemapPathPrefix(
                        model->MaterialAssetPath(),
                        oldDirectory,
                        newDirectory))
                {
                    model->SetMaterialAssetPath(*remapped);
                }
                // 接頭辞を置換した参照パス
                if (const auto remapped =
                    RemapPathPrefix(
                        model->ModelPath(),
                        oldDirectory,
                        newDirectory))
                {
                    model->SetModelPath(*remapped);
                }
                // 接頭辞を置換した参照パス
                if (const auto remapped =
                    RemapPathPrefix(
                        model->AlbedoTexturePath(),
                        oldDirectory,
                        newDirectory))
                {
                    model->SetAlbedoTexturePath(*remapped);
                }
                // 接頭辞を置換した参照パス
                if (const auto remapped =
                    RemapPathPrefix(
                        model->NormalTexturePath(),
                        oldDirectory,
                        newDirectory))
                {
                    model->SetNormalTexturePath(*remapped);
                }
                // PBR画像参照の取得・設定
                for (const auto& accessor :
                    PbrMapAccessors<ModelRendererComponent>())
                {
                    // 接頭辞を置換した参照パス
                    if (const auto remapped =
                        RemapPathPrefix(
                            (model->*accessor.get)(),
                            oldDirectory,
                            newDirectory))
                    {
                        (model->*accessor.set)(*remapped);
                    }
                }
            }
            // メッシュ描画素材の更新対象
            if (auto* mesh =
                gameObject->GetComponent<MeshRendererComponent>())
            {
                // 接頭辞を置換した参照パス
                if (const auto remapped =
                    RemapPathPrefix(
                        mesh->MaterialAssetPath(),
                        oldDirectory,
                        newDirectory))
                {
                    mesh->SetMaterialAssetPath(*remapped);
                }
                // 接頭辞を置換した参照パス
                if (const auto remapped =
                    RemapPathPrefix(
                        mesh->AlbedoTexturePath(),
                        oldDirectory,
                        newDirectory))
                {
                    mesh->SetAlbedoTexturePath(*remapped);
                }
                // 接頭辞を置換した参照パス
                if (const auto remapped =
                    RemapPathPrefix(
                        mesh->NormalTexturePath(),
                        oldDirectory,
                        newDirectory))
                {
                    mesh->SetNormalTexturePath(*remapped);
                }
                // PBR画像参照の取得・設定
                for (const auto& accessor :
                    PbrMapAccessors<MeshRendererComponent>())
                {
                    // 接頭辞を置換した参照パス
                    if (const auto remapped =
                        RemapPathPrefix(
                            (mesh->*accessor.get)(),
                            oldDirectory,
                            newDirectory))
                    {
                        (mesh->*accessor.set)(*remapped);
                    }
                }
            }
        }
    }

    // 編集中の完全一致する参照と起動シーンを置換します(oldAsset: 移動元の相対パス, newAsset: 移動先の相対パス)。
    void EditorLayer::RemapAssetFileReferences(
        const std::filesystem::path& oldAsset,
        const std::filesystem::path& newAsset)
    {
        if (m_selectedAsset == oldAsset)
        {
            m_selectedAsset = newAsset;
        }

        // 移動前の絶対パス
        const auto oldAbsolute =
            m_graphics.Assets().ResolvePath(oldAsset);
        // 移動後の絶対パス
        const auto newAbsolute =
            m_graphics.Assets().ResolvePath(newAsset);
        if (m_scenePath.lexically_normal()
            == oldAbsolute.lexically_normal())
        {
            m_scenePath = newAbsolute;
        }

        if (IsSameAssetReference(
            oldAsset,
            m_projectSettings.startupScene))
        {
            m_projectSettings.startupScene = newAsset;
            // 設定保存に失敗しても、先に更新したメモリ内のパスは元へ戻りません。
            SaveProjectConfiguration();
        }

        // 参照パスを更新する対象
        for (const auto& gameObject : m_scene.GameObjects())
        {
            if (gameObject->PrefabAssetPath() == oldAsset)
            {
                gameObject->SetPrefabAssetPath(
                    newAsset);
            }
            // 画像・Shader参照の更新対象
            if (auto* sprite =
                gameObject->GetComponent<SpriteRendererComponent>();
                sprite != nullptr
                && sprite->TexturePath() == oldAsset)
            {
                sprite->SetTexturePath(newAsset);
            }
            // 画像・Shader参照の更新対象
            if (auto* sprite =
                gameObject->GetComponent<SpriteRendererComponent>();
                sprite != nullptr
                && sprite->ShaderPath() == oldAsset)
            {
                sprite->SetShaderPath(newAsset);
            }
            // タイル画像の更新対象
            if (auto* tilemap =
                gameObject->GetComponent<
                    TilemapComponent>();
                tilemap != nullptr
                && tilemap->TexturePath()
                    == oldAsset)
            {
                tilemap->SetTexturePath(
                    newAsset);
            }
            // 粒子画像・Shaderの更新対象
            if (auto* particles =
                gameObject->GetComponent<
                    ParticleSystemComponent>();
                particles != nullptr
                && particles->TexturePath()
                    == oldAsset)
            {
                particles->SetTexturePath(
                    newAsset);
            }
            // 粒子画像・Shaderの更新対象
            if (auto* particles =
                gameObject->GetComponent<
                    ParticleSystemComponent>();
                particles != nullptr
                && particles->ShaderPath()
                    == oldAsset)
            {
                particles->SetShaderPath(
                    newAsset);
            }
            // 粒子画像・Shaderの更新対象
            if (auto* particles =
                gameObject->GetComponent<
                    ParticleSystemComponent>();
                particles != nullptr
                && particles->AuxiliaryTexturePath()
                    == oldAsset)
            {
                particles->SetAuxiliaryTexturePath(
                    newAsset);
            }
            // ボタン画像・遷移先の更新対象
            if (auto* button =
                gameObject->GetComponent<
                    UIButtonComponent>();
                button != nullptr
                && button->TexturePath()
                    == oldAsset)
            {
                button->SetTexturePath(
                    newAsset);
            }
            // ボタン画像・遷移先の更新対象
            if (auto* button =
                gameObject->GetComponent<
                    UIButtonComponent>();
                button != nullptr
                && button->TargetScene()
                    == oldAsset)
            {
                button->SetTargetScene(
                    newAsset);
            }
            // アニメーションの更新対象
            if (auto* animator =
                gameObject->GetComponent<
                    TransformAnimatorComponent>();
                animator != nullptr
                && animator->ClipPath()
                    == oldAsset)
            {
                animator->SetClipPath(
                    newAsset);
            }
            // アニメーションの更新対象
            if (auto* animator =
                gameObject->GetComponent<
                    TransformAnimatorComponent>();
                animator != nullptr
                && animator->ControllerPath()
                    == oldAsset)
            {
                animator->SetControllerPath(
                    newAsset);
            }
            // モデル描画素材の更新対象
            if (auto* model =
                gameObject->GetComponent<ModelRendererComponent>();
                model != nullptr)
            {
                if (model->AnimationControllerPath()
                    == oldAsset)
                {
                    model->SetAnimationControllerPath(
                        newAsset);
                }
                if (model->MaterialAssetPath() == oldAsset)
                {
                    model->SetMaterialAssetPath(newAsset);
                }
                if (model->ModelPath() == oldAsset)
                {
                    model->SetModelPath(newAsset);
                }
                if (model->AlbedoTexturePath() == oldAsset)
                {
                    model->SetAlbedoTexturePath(newAsset);
                }
                if (model->NormalTexturePath() == oldAsset)
                {
                    model->SetNormalTexturePath(newAsset);
                }
                // PBR画像参照の取得・設定
                for (const auto& accessor :
                    PbrMapAccessors<ModelRendererComponent>())
                {
                    if ((model->*accessor.get)() == oldAsset)
                    {
                        (model->*accessor.set)(newAsset);
                    }
                }
            }
            // メッシュ描画素材の更新対象
            if (auto* mesh =
                gameObject->GetComponent<MeshRendererComponent>();
                mesh != nullptr)
            {
                if (mesh->MaterialAssetPath() == oldAsset)
                {
                    mesh->SetMaterialAssetPath(newAsset);
                }
                if (mesh->AlbedoTexturePath() == oldAsset)
                {
                    mesh->SetAlbedoTexturePath(newAsset);
                }
                if (mesh->NormalTexturePath() == oldAsset)
                {
                    mesh->SetNormalTexturePath(newAsset);
                }
                // PBR画像参照の取得・設定
                for (const auto& accessor :
                    PbrMapAccessors<MeshRendererComponent>())
                {
                    if ((mesh->*accessor.get)() == oldAsset)
                    {
                        (mesh->*accessor.set)(newAsset);
                    }
                }
            }
        }
    }

    // 種別アイコンを初回だけ読み、失敗も記憶します(kind: 配列範囲内のアセット種別)。
    std::shared_ptr<const TextureAsset> EditorLayer::GetFileTypeIcon(
        AssetIconKind kind)
    {
        // アセット種別の配列位置
        const auto index = static_cast<std::size_t>(kind);
        if (m_fileTypeIconAttempted[index])
        {
            return m_fileTypeIcons[index];
        }
        m_fileTypeIconAttempted[index] = true;

        // 種別順のアイコン画像名
        static const wchar_t* const fileNames[] = {
            L"folder.png",
            L"scene.png",
            L"prefab.png",
            L"model.png",
            L"material.png",
            L"shader.png",
            L"animation.png",
            L"animator_controller.png",
            L"cpp_script.png",
            L"file.png",
        };

        // エンジンのアイコン画像パス
        const auto iconPath = m_engineRoot
            / L"assets" / L"icons" / L"filetypes" / fileNames[index];
        if (!std::filesystem::is_regular_file(iconPath))
        {
            return nullptr;
        }

        try
        {
            m_fileTypeIcons[index] = m_graphics.Assets().LoadTexture(iconPath);
        }
        catch (const std::exception&)
        {
            m_fileTypeIcons[index] = nullptr;
        }
        return m_fileTypeIcons[index];
    }

    // アセットの検索・一覧・操作とドロップ受付を描画します(open: ウィンドウの開閉状態)。
    void EditorLayer::DrawAssetBrowser(bool& open)
    {
        if (!open)
        {
            return;
        }

        ImGui::SetNextWindowSize(
            ImVec2{ HierarchyWidth, 420.0f },
            ImGuiCond_FirstUseEver);

        // アセット画面の表示フラグ
        constexpr ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoCollapse;

        // 画面内容が描画可能
        const bool visible = ImGui::Begin(
            "アセット",
            &open,
            flags);
        if (m_selectAssetTabAfterLayoutReset)
        {
            // 選択するアセット画面
            auto* window = ImGui::GetCurrentWindow();
            if (window->DockNode != nullptr
                && window->DockNode->TabBar != nullptr)
            {
                window->DockNode->SelectedTabId =
                    window->TabId;
                window->DockNode->TabBar->SelectedTabId =
                    window->TabId;
                window->DockNode->TabBar->NextSelectedTabId =
                    window->TabId;
                m_selectAssetTabAfterLayoutReset = false;
            }
        }
        if (!visible)
        {
            ImGui::End();
            return;
        }

        // 画面左上の座標
        const ImVec2 assetBrowserPosition =
            ImGui::GetWindowPos();
        // 画面の表示サイズ
        const ImVec2 assetBrowserSize =
            ImGui::GetWindowSize();
        // 外部ドロップの受付矩形
        const RECT assetBrowserBounds{
            static_cast<LONG>(assetBrowserPosition.x),
            static_cast<LONG>(assetBrowserPosition.y),
            static_cast<LONG>(
                assetBrowserPosition.x
                + assetBrowserSize.x),
            static_cast<LONG>(
                assetBrowserPosition.y
                + assetBrowserSize.y)
        };
        ProcessExternalAssetDrops(assetBrowserBounds);

        // 描画中のUIスタイル
        const ImGuiStyle& style = ImGui::GetStyle();
        // 検索欄右側のボタン幅
        const float toolbarButtonsWidth =
            ImGui::CalcTextSize("更新").x
            + ImGui::CalcTextSize("表示方法").x
            + style.FramePadding.x * 4.0f
            + style.ItemSpacing.x * 2.0f;
        ImGui::SetNextItemWidth(-toolbarButtonsWidth);
        ImGui::InputTextWithHint(
            "##AssetFilter",
            "アセットを検索",
            m_assetFilter.data(),
            m_assetFilter.size());
        ImGui::SameLine();
        if (ImGui::Button("更新"))
        {
            RefreshAssets();
        }
        ImGui::SameLine();
        if (ImGui::Button("表示方法"))
        {
            ImGui::OpenPopup("AssetViewOptions");
        }
        if (ImGui::BeginPopup("AssetViewOptions"))
        {
            ImGui::TextDisabled("レイアウト");
            ImGui::Separator();
            if (ImGui::MenuItem("グリッド", nullptr, m_assetGridView))
            {
                m_assetGridView = true;
            }
            if (ImGui::MenuItem("一覧", nullptr, !m_assetGridView))
            {
                m_assetGridView = false;
            }
            ImGui::Separator();
            ImGui::MenuItem(
                "フォルダーツリー",
                nullptr,
                &m_assetDirectoryTreeVisible);
            ImGui::EndPopup();
        }

        OpenPendingAssetDialog();
        DrawAssetFolderDialogs();

        ImGui::Separator();
        if (m_assetDirectoryTreeVisible)
        {
            ImGui::BeginChild(
                "AssetDirectories",
                ImVec2{ 0.0f, 76.0f },
                ImGuiChildFlags_Borders);
            DrawAssetDirectoryTree();
            ImGui::EndChild();
        }

        // 小文字化した検索文字列
        const std::string filter = Lowercase(m_assetFilter.data());
        // 全フォルダーを検索中
        const bool searching = !filter.empty();
        if (searching)
        {
            ImGui::TextDisabled("検索結果（すべてのフォルダー）");
        }
        else
        {
            if (ImGui::SmallButton("assets"))
            {
                m_assetDirectory.clear();
                m_selectedAsset.clear();
            }
            DrawAssetDirectoryContextMenu({}, true);
            AcceptAssetMoveDrop({});

            // パンくずの累積相対パス
            std::filesystem::path breadcrumb;
            // 描画開始時の表示フォルダー
            const std::filesystem::path currentAssetDirectory =
                m_assetDirectory;
            // パンくずのフォルダー名
            for (const auto& part : currentAssetDirectory)
            {
                breadcrumb /= part;
                ImGui::SameLine();
                ImGui::TextUnformatted(">");
                ImGui::SameLine();
                // パンくずボタンの表示名
                const std::string partLabel = PathToUtf8(part);
                ImGui::PushID(PathToUtf8(breadcrumb).c_str());
                if (ImGui::SmallButton(partLabel.c_str()))
                {
                    m_assetDirectory = breadcrumb;
                    m_selectedAsset.clear();
                }
                DrawAssetDirectoryContextMenu(breadcrumb, false);
                AcceptAssetMoveDrop(breadcrumb);
                ImGui::PopID();
            }
        }

        ImGui::BeginChild("AssetList");

        // 直下の空きを調べます(directory: 検査するフォルダー, asset: 検査するアセット)。
        const bool currentDirectoryEmpty =
            !searching
            && std::ranges::none_of(
                m_assetDirectories,
                [this](const auto& directory)
                {
                    return directory.parent_path()
                        == m_assetDirectory;
                })
            && std::ranges::none_of(
                m_assetFiles,
                [this](const auto& asset)
                {
                    return asset.parent_path()
                        == m_assetDirectory;
                });
        if (currentDirectoryEmpty)
        {
            ImGui::TextDisabled(
                "Explorerからここへドロップしてインポート");
        }

        if (m_assetGridView)
        {
            // グリッドの表示列数
            const int columnCount = std::max(
                1,
                static_cast<int>(ImGui::GetContentRegionAvail().x / 88.0f));

            if (ImGui::BeginTable(
                "AssetGrid",
                columnCount,
                ImGuiTableFlags_SizingFixedFit))
            {
                // 種別画像または代替ボタンを描きます(kind: アセット種別, imageId: 画像ボタンID, fallbackLabel: 代替表示名, size: ボタンサイズ)。
                const auto drawTypeButton = [this](
                    AssetIconKind kind,
                    const char* imageId,
                    const char* fallbackLabel,
                    const ImVec2& size) -> bool
                {
                    // 種別ボタンの画像
                    if (const auto icon = GetFileTypeIcon(kind))
                    {
                        return ImGui::ImageButton(
                            imageId,
                            m_editorGuiRenderer->TextureReference(*icon),
                            size);
                    }
                    return ImGui::Button(fallbackLabel, size);
                };

                if (!searching)
                {
                    // 表示する子フォルダー
                    for (const auto& directory : m_assetDirectories)
                    {
                        if (directory.parent_path() != m_assetDirectory)
                        {
                            continue;
                        }

                        ImGui::TableNextColumn();
                        // フォルダーの相対パス表示
                        const std::string directoryLabel =
                            PathToUtf8(directory);
                        ImGui::PushID(directoryLabel.c_str());
                        if (drawTypeButton(
                            AssetIconKind::Folder,
                            "##FolderIcon",
                            "フォルダー",
                            ImVec2{ 64.0f, 64.0f }))
                        {
                            m_assetDirectory = directory;
                            m_selectedAsset.clear();
                        }
                        DrawAssetDirectoryContextMenu(directory, false);
                        BeginAssetFolderDragSource(directory);
                        AcceptAssetMoveDrop(directory);
                        ImGui::TextWrapped(
                            "%s",
                            PathToUtf8(directory.filename()).c_str());
                        ImGui::PopID();
                    }
                }

                // 表示するアセット
                for (const auto& asset : m_assetFiles)
                {
                    // ファイル・フォルダー表示名
                    const std::string label = PathToUtf8(asset);
                    if (searching
                        ? Lowercase(label).find(filter) == std::string::npos
                        : asset.parent_path() != m_assetDirectory)
                    {
                        continue;
                    }

                    ImGui::TableNextColumn();
                    ImGui::PushID(label.c_str());

                    // 選択中のアセット
                    const bool selected = asset == m_selectedAsset;
                    ImGui::PushStyleVar(
                        ImGuiStyleVar_FrameBorderSize,
                        selected ? 2.0f : 0.0f);
                    ImGui::PushStyleColor(
                        ImGuiCol_Border,
                        ImVec4{ 0.20f, 0.75f, 1.0f, 1.0f });

                    // サムネイルが押された
                    bool clicked = false;
                    if (IsTextureAsset(asset))
                    {
                        try
                        {
                            // 画像アセットのサムネイル
                            const auto texture =
                                m_graphics.Assets().LoadTexture(asset);
                            clicked = ImGui::ImageButton(
                                "##AssetThumbnail",
                                m_editorGuiRenderer->TextureReference(
                                    *texture),
                                ImVec2{ 64.0f, 64.0f });
                        }
                        catch (const std::exception&)
                        {
                            clicked = ImGui::Button(
                                "画像",
                                ImVec2{ 64.0f, 64.0f });
                        }
                    }
                    else if (IsSceneAsset(asset))
                    {
                        clicked = drawTypeButton(
                            AssetIconKind::Scene,
                            "##SceneIcon",
                            "シーン",
                            ImVec2{ 64.0f, 64.0f });
                    }
                    else if (IsPrefabAsset(asset))
                    {
                        clicked = drawTypeButton(
                            AssetIconKind::Prefab,
                            "##PrefabIcon",
                            "Prefab",
                            ImVec2{ 64.0f, 64.0f });
                    }
                    else if (IsModelAsset(asset))
                    {
                        clicked = drawTypeButton(
                            AssetIconKind::Model,
                            "##ModelIcon",
                            "3D",
                            ImVec2{ 64.0f, 64.0f });
                    }
                    else if (IsMaterialAsset(asset))
                    {
                        clicked = drawTypeButton(
                            AssetIconKind::Material,
                            "##MaterialIcon",
                            "Lit",
                            ImVec2{ 64.0f, 64.0f });
                    }
                    else if (IsOpenableShaderAsset(asset))
                    {
                        clicked = drawTypeButton(
                            AssetIconKind::Shader,
                            "##ShaderIcon",
                            IsShaderManifestAsset(asset)
                                ? "Manifest"
                                : "HLSL",
                            ImVec2{ 64.0f, 64.0f });
                    }
                    else if (IsAnimationAsset(asset))
                    {
                        clicked = drawTypeButton(
                            AssetIconKind::Animation,
                            "##AnimationIcon",
                            "Anim",
                            ImVec2{ 64.0f, 64.0f });
                    }
                    else if (IsAnimatorControllerAsset(
                        asset))
                    {
                        clicked = drawTypeButton(
                            AssetIconKind::AnimatorController,
                            "##AnimatorControllerIcon",
                            "State",
                            ImVec2{ 64.0f, 64.0f });
                    }
                    else if (IsCppScriptAsset(asset))
                    {
                        clicked = drawTypeButton(
                            AssetIconKind::CppScript,
                            "##CppScriptIcon",
                            "C++",
                            ImVec2{ 64.0f, 64.0f });
                    }
                    else
                    {
                        clicked = drawTypeButton(
                            AssetIconKind::Generic,
                            "##GenericFileIcon",
                            "ファイル",
                            ImVec2{ 64.0f, 64.0f });
                    }

                    // サムネイル上にマウスがある
                    // 後続のPopupやドラッグ描画で項目が変わる前に、サムネイルのホバーを記録します。
                    const bool thumbnailHovered = ImGui::IsItemHovered();
                    if (clicked)
                    {
                        m_selectedAsset = asset;
                    }
                    DrawAssetFileContextMenu(asset);

                    if (ImGui::BeginDragDropSource())
                    {
                        ImGui::SetDragDropPayload(
                            AssetPayload,
                            label.c_str(),
                            label.size() + 1);
                        ImGui::TextUnformatted(label.c_str());
                        ImGui::EndDragDropSource();
                    }

                    if (IsSceneAsset(asset)
                        && thumbnailHovered
                        && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
                        && !m_playing)
                    {
                        m_selectedAsset = asset;
                        OpenSelectedAsset();
                    }
                    else if (IsPrefabAsset(asset)
                        && thumbnailHovered
                        && ImGui::IsMouseDoubleClicked(
                            ImGuiMouseButton_Left)
                        && !m_playing)
                    {
                        m_selectedAsset = asset;
                        InstantiateSelectedPrefab();
                    }
                    else if (IsMaterialAsset(asset)
                        && thumbnailHovered
                        && ImGui::IsMouseDoubleClicked(
                            ImGuiMouseButton_Left)
                        && !m_playing)
                    {
                        m_selectedAsset = asset;
                        AssignSelectedMaterial();
                    }
                    else if (IsAnimationAsset(asset)
                        && thumbnailHovered
                        && ImGui::IsMouseDoubleClicked(
                            ImGuiMouseButton_Left)
                        && !m_playing)
                    {
                        m_selectedAsset = asset;
                        AssignSelectedAnimation();
                    }
                    else if (IsAnimatorControllerAsset(
                            asset)
                        && thumbnailHovered
                        && ImGui::IsMouseDoubleClicked(
                            ImGuiMouseButton_Left)
                        && !m_playing)
                    {
                        m_selectedAsset = asset;
                        OpenAnimatorControllerGraph(asset);
                    }
                    else if (IsCppScriptAsset(asset)
                        && thumbnailHovered
                        && ImGui::IsMouseDoubleClicked(
                            ImGuiMouseButton_Left)
                        && !m_playing)
                    {
                        m_selectedAsset = asset;
                        OpenCodeAsset(asset);
                    }
                    else if (IsOpenableShaderAsset(asset)
                        && thumbnailHovered
                        && ImGui::IsMouseDoubleClicked(
                            ImGuiMouseButton_Left)
                        && !m_playing)
                    {
                        m_selectedAsset = asset;
                        OpenCodeAsset(asset);
                    }

                    ImGui::PopStyleColor();
                    ImGui::PopStyleVar();
                    ImGui::TextWrapped(
                        "%s",
                        PathToUtf8(asset.filename()).c_str());
                    ImGui::PopID();
                }

                ImGui::EndTable();
            }
        }
        else
        {
            if (!searching)
            {
                // 一覧用のフォルダー画像
                const auto folderIcon = GetFileTypeIcon(AssetIconKind::Folder);
                // 表示する子フォルダー
                for (const auto& directory : m_assetDirectories)
                {
                    if (directory.parent_path() != m_assetDirectory)
                    {
                        continue;
                    }

                    // ファイル・フォルダー表示名
                    const std::string label = folderIcon
                        ? PathToUtf8(directory.filename())
                        : "[フォルダー] " + PathToUtf8(directory.filename());
                    ImGui::PushID(PathToUtf8(directory).c_str());
                    if (folderIcon)
                    {
                        ImGui::Image(
                            m_editorGuiRenderer->TextureReference(
                                *folderIcon),
                            ImVec2{ 16.0f, 16.0f });
                        ImGui::SameLine();
                    }
                    if (ImGui::Selectable(label.c_str()))
                    {
                        m_assetDirectory = directory;
                        m_selectedAsset.clear();
                    }
                    DrawAssetDirectoryContextMenu(directory, false);
                    BeginAssetFolderDragSource(directory);
                    AcceptAssetMoveDrop(directory);
                    ImGui::PopID();
                }
            }

            // 表示するアセット
            for (const auto& asset : m_assetFiles)
            {
                // アセットの相対パス表示
                const std::string pathLabel = PathToUtf8(asset);
                if (searching
                    ? Lowercase(pathLabel).find(filter) == std::string::npos
                    : asset.parent_path() != m_assetDirectory)
                {
                    continue;
                }

                // ファイル・フォルダー表示名
                const std::string label = searching
                    ? pathLabel
                    : PathToUtf8(asset.filename());
                // 選択中のアセット
                const bool selected = asset == m_selectedAsset;
                ImGui::PushID(pathLabel.c_str());
                // 一覧用のアセット種別画像
                const auto listIcon = GetFileTypeIcon(
                    IsTextureAsset(asset) ? AssetIconKind::Generic
                    : IsSceneAsset(asset) ? AssetIconKind::Scene
                    : IsPrefabAsset(asset) ? AssetIconKind::Prefab
                    : IsModelAsset(asset) ? AssetIconKind::Model
                    : IsMaterialAsset(asset) ? AssetIconKind::Material
                    : IsOpenableShaderAsset(asset)
                        ? AssetIconKind::Shader
                    : IsAnimationAsset(asset) ? AssetIconKind::Animation
                    : IsAnimatorControllerAsset(asset)
                        ? AssetIconKind::AnimatorController
                    : IsCppScriptAsset(asset) ? AssetIconKind::CppScript
                    : AssetIconKind::Generic);
                if (listIcon)
                {
                    ImGui::Image(
                        m_editorGuiRenderer->TextureReference(*listIcon),
                        ImVec2{ 16.0f, 16.0f });
                    ImGui::SameLine();
                }
                if (ImGui::Selectable(label.c_str(), selected))
                {
                    m_selectedAsset = asset;
                }
                DrawAssetFileContextMenu(asset);
                ImGui::PopID();

                if (IsSceneAsset(asset)
                    && ImGui::IsItemHovered()
                    && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
                    && !m_playing)
                {
                    m_selectedAsset = asset;
                    OpenSelectedAsset();
                }
                else if (IsPrefabAsset(asset)
                    && ImGui::IsItemHovered()
                    && ImGui::IsMouseDoubleClicked(
                        ImGuiMouseButton_Left)
                    && !m_playing)
                {
                    m_selectedAsset = asset;
                    InstantiateSelectedPrefab();
                }
                else if (IsMaterialAsset(asset)
                    && ImGui::IsItemHovered()
                    && ImGui::IsMouseDoubleClicked(
                        ImGuiMouseButton_Left)
                    && !m_playing)
                {
                    m_selectedAsset = asset;
                    AssignSelectedMaterial();
                }
                else if (IsAnimationAsset(asset)
                    && ImGui::IsItemHovered()
                    && ImGui::IsMouseDoubleClicked(
                        ImGuiMouseButton_Left)
                    && !m_playing)
                {
                    m_selectedAsset = asset;
                    AssignSelectedAnimation();
                }
                else if (IsAnimatorControllerAsset(
                        asset)
                    && ImGui::IsItemHovered()
                    && ImGui::IsMouseDoubleClicked(
                        ImGuiMouseButton_Left)
                    && !m_playing)
                {
                    m_selectedAsset = asset;
                    OpenAnimatorControllerGraph(asset);
                }
                else if (IsCppScriptAsset(asset)
                    && ImGui::IsItemHovered()
                    && ImGui::IsMouseDoubleClicked(
                        ImGuiMouseButton_Left)
                    && !m_playing)
                {
                    m_selectedAsset = asset;
                    OpenCodeAsset(asset);
                }
                else if (IsOpenableShaderAsset(asset)
                    && ImGui::IsItemHovered()
                    && ImGui::IsMouseDoubleClicked(
                        ImGuiMouseButton_Left)
                    && !m_playing)
                {
                    m_selectedAsset = asset;
                    OpenCodeAsset(asset);
                }

                if (ImGui::BeginDragDropSource())
                {
                    ImGui::SetDragDropPayload(
                        AssetPayload,
                        pathLabel.c_str(),
                        pathLabel.size() + 1);
                    ImGui::TextUnformatted(pathLabel.c_str());
                    ImGui::EndDragDropSource();
                }
            }
        }

        if (ImGui::IsWindowHovered()
            && ImGui::IsMouseClicked(
                ImGuiMouseButton_Right)
            && !ImGui::IsAnyItemHovered())
        {
            m_selectedAsset.clear();
        }
        if (ImGui::BeginPopupContextWindow(
            "##AssetBrowserBackgroundContext",
            ImGuiPopupFlags_MouseButtonRight
                | ImGuiPopupFlags_NoOpenOverItems))
        {
            DrawAssetDirectoryMenuContents(
                m_assetDirectory,
                m_assetDirectory.empty());
            ImGui::EndPopup();
        }

        ImGui::EndChild();
        ImGui::End();
    }

    // 複数ファイル選択の結果を単一・複数形式から読み、インポートを開始します。
    void EditorLayer::OpenImportAssetsDialog()
    {
        // 複数選択結果の連結バッファ
        std::array<wchar_t, 65536> selectedFiles{};
        // 選択できるファイルの一覧
        constexpr wchar_t filter[] =
            L"対応アセット\0"
            L"*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff;*.dds;"
            L"*.cmo;*.sdkmesh;*.vbo;*.gltf;*.glb;*.fbx;"
            L"*.wav;*.hlsl;*.cpp;*.json\0"
            L"すべてのファイル (*.*)\0*.*\0\0";

        // 複数選択のファイルダイアログ
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = m_window;
        dialog.lpstrFilter = filter;
        dialog.nFilterIndex = 1;
        dialog.lpstrFile = selectedFiles.data();
        dialog.nMaxFile =
            static_cast<DWORD>(selectedFiles.size());
        dialog.lpstrTitle =
            L"アセットへインポート";
        dialog.Flags =
            OFN_ALLOWMULTISELECT
            | OFN_EXPLORER
            | OFN_FILEMUSTEXIST
            | OFN_PATHMUSTEXIST
            | OFN_NOCHANGEDIR;

        if (!GetOpenFileNameW(&dialog))
        {
            if (CommDlgExtendedError() != 0)
            {
                SetStatus(
                    "インポートダイアログを表示できませんでした",
                    true);
            }
            return;
        }

        // 選択したインポート元パス
        std::vector<std::filesystem::path> sources;
        // 最初のパス文字列
        const wchar_t* first = selectedFiles.data();
        // 次の選択名の読み取り位置
        const wchar_t* next = first + std::wcslen(first) + 1;
        if (*next == L'\0')
        {
            sources.emplace_back(first);
        }
        else
        {
            // 複数選択元のフォルダー
            const std::filesystem::path directory{ first };
            while (*next != L'\0')
            {
                sources.push_back(directory / next);
                next += std::wcslen(next) + 1;
            }
        }
        ImportAssets(sources);
    }

    // 編集中にファイルを取り込み、素材の段階読込を予約します(sources: インポート元のパス一覧)。
    void EditorLayer::ImportAssets(
        const std::vector<std::filesystem::path>& sources)
    {
        if (sources.empty())
        {
            return;
        }
        if (m_playing)
        {
            SetStatus(
                "再生中はアセットをインポートできません",
                true);
            return;
        }

        try
        {
            // コピー・改名・失敗の結果
            const auto importResult = AssetImporter::Import(
                sources,
                m_graphics.Assets().AssetRoot(),
                m_assetDirectory);
            // ファイルまたはフォルダーを作成
            const bool importedAnything =
                !importResult.files.empty()
                || importResult.importedDirectoryCount != 0;
            if (importedAnything)
            {
                RefreshAssets();
                // インポート済みファイル情報
                // 使用中の素材を二重確保しないよう、全キャッシュ消去を避けて取込済みファイルだけを無効化します。
                for (const auto& imported : importResult.files)
                {
                    m_graphics.Assets().Invalidate(
                        imported.path);
                }
            }

            // 表示するインポート失敗一覧
            std::vector<std::string> failures;
            failures.reserve(
                importResult.failures.size());
            // インポート元ごとの失敗
            for (const auto& failure : importResult.failures)
            {
                failures.push_back(
                    PathToUtf8(failure.source.filename())
                    + ": "
                    + failure.message);
            }

            // 段階読込へ追加した件数
            std::size_t queuedCount{};
            // インポート済みファイル情報
            for (const auto& imported : importResult.files)
            {
                if (IsTextureAsset(imported.path)
                    || IsModelAsset(imported.path)
                    || IsAudioAsset(imported.path))
                {
                    m_pendingAssetImports.push_back(
                        imported.path);
                    ++queuedCount;
                }
            }
            m_pendingAssetImportTotal += queuedCount;

            if (!importResult.files.empty())
            {
                m_selectedAsset =
                    importResult.files.front().path;
            }

            if (!importedAnything)
            {
                if (!failures.empty())
                {
                    SetStatus(
                        "インポートできませんでした: "
                        + failures.front(),
                        true);
                }
                else
                {
                    SetStatus(
                        ".metaファイルとリンクは"
                        "インポート対象外です");
                }
                return;
            }

            // インポート結果の表示文
            std::string message =
                std::to_string(importResult.files.size())
                + "件のファイルをインポートしました";
            if (importResult.importedDirectoryCount != 0)
            {
                message += "（フォルダー"
                    + std::to_string(
                        importResult.importedDirectoryCount)
                    + "件）";
            }
            if (queuedCount != 0)
            {
                message += " / "
                    + std::to_string(queuedCount)
                    + "件を段階ロードします";
            }
            if (importResult.renamedSourceCount != 0)
            {
                message += " / 同名"
                    + std::to_string(
                        importResult.renamedSourceCount)
                    + "件を自動改名";
            }
            if (!failures.empty())
            {
                message += " / "
                    + std::to_string(failures.size())
                    + "件の処理に失敗: "
                    + failures.front();
            }
            SetStatus(message, !failures.empty());
        }
        // インポート処理を中断した原因
        catch (const std::exception& exception)
        {
            SetStatus(
                std::string{ "インポートに失敗しました: " }
                    + exception.what(),
                true);
        }
    }

    // 先頭の素材を段階読込し、モデルの非同期準備中は次へ進めません。
    void EditorLayer::ProcessPendingAssetImports()
    {
        if (m_pendingAssetImports.empty()
            || m_playing
            || m_gameModuleBuildProcess != nullptr)
        {
            return;
        }

        // 先頭の段階読込アセット
        const auto asset = m_pendingAssetImports.front();
        // 失敗数と最初の理由を記録します(message: 先頭アセットの失敗理由)。
        const auto recordFailure =
            [this, &asset](std::string message)
            {
                ++m_pendingAssetImportFailures;
                if (m_pendingAssetImportFirstFailure.empty())
                {
                    m_pendingAssetImportFirstFailure =
                        PathToUtf8(asset)
                        + ": "
                        + std::move(message);
                }
            };

        if (IsModelAsset(asset))
        {
            try
            {
                // モデル準備の失敗理由
                std::string preparationError;
                // モデルの非同期準備状態
                const auto state =
                    m_graphics.Assets().PollModelPreparation(
                        asset,
                        &preparationError);
                if (state == ModelPreparationState::NotQueued)
                {
                    if (m_graphics.Assets().PrepareModelAsync(asset))
                    {
                        SetStatus(
                            "モデルをバックグラウンド準備中: "
                            + PathToUtf8(asset.filename()));
                    }
                    return;
                }
                if (state == ModelPreparationState::Pending)
                {
                    return;
                }
                if (state == ModelPreparationState::Failed)
                {
                    recordFailure(
                        preparationError.empty()
                            ? "モデルの準備に失敗しました"
                            : std::move(preparationError));
                }
            }
            // 段階読込を失敗として数える原因
            catch (const std::exception& exception)
            {
                recordFailure(exception.what());
            }
        }
        else
        {
            try
            {
                if (IsTextureAsset(asset))
                {
                    static_cast<void>(
                        m_graphics.Assets().LoadTexture(asset));
                }
                else if (IsAudioAsset(asset))
                {
                    static_cast<void>(
                        m_graphics.Audio().LoadSoundEffect(
                            m_graphics.Assets(),
                            m_graphics.Assets().ResolvePath(asset)));
                }
            }
            // 段階読込を失敗として数える原因
            catch (const std::exception& exception)
            {
                recordFailure(exception.what());
            }
        }

        m_pendingAssetImports.pop_front();
        ++m_pendingAssetImportCompleted;
        if (!m_pendingAssetImports.empty())
        {
            SetStatus(
                "アセットを段階ロード中: "
                + std::to_string(m_pendingAssetImportCompleted)
                + "/"
                + std::to_string(m_pendingAssetImportTotal),
                m_pendingAssetImportFailures != 0);
            return;
        }

        // 段階読込完了の表示文
        std::string message =
            "アセットの段階ロードが完了しました: "
            + std::to_string(
                m_pendingAssetImportCompleted
                - m_pendingAssetImportFailures)
            + "/"
            + std::to_string(m_pendingAssetImportTotal)
            + "件";
        if (m_pendingAssetImportFailures != 0)
        {
            message += " / "
                + std::to_string(m_pendingAssetImportFailures)
                + "件失敗: "
                + m_pendingAssetImportFirstFailure;
        }
        SetStatus(
            std::move(message),
            m_pendingAssetImportFailures != 0);
        m_pendingAssetImportTotal = 0;
        m_pendingAssetImportCompleted = 0;
        m_pendingAssetImportFailures = 0;
        m_pendingAssetImportFirstFailure.clear();
    }

    // 矩形内の外部ドロップをまとめて取り込みます(assetBrowserBounds: 画面座標の受付矩形)。
    void EditorLayer::ProcessExternalAssetDrops(
        const RECT& assetBrowserBounds)
    {
        if (m_pendingExternalAssetDrops.empty()
            || m_gameModuleBuildProcess != nullptr)
        {
            return;
        }

        // 処理する外部ドロップ一覧
        // 再生中でも予約は消費され、ImportAssets側でインポートが拒否されます。
        auto pendingDrops =
            std::move(m_pendingExternalAssetDrops);
        m_pendingExternalAssetDrops.clear();
        // 矩形内へ落とした元パス一覧
        std::vector<std::filesystem::path> sources;
        // 矩形外へ落とした回数
        std::size_t outsideDropCount{};
        // 受付位置と元パスの組
        for (auto& pending : pendingDrops)
        {
            if (!PtInRect(
                    &assetBrowserBounds,
                    pending.screenPosition))
            {
                ++outsideDropCount;
                continue;
            }
            sources.insert(
                sources.end(),
                std::make_move_iterator(
                    pending.sources.begin()),
                std::make_move_iterator(
                    pending.sources.end()));
        }

        if (!sources.empty())
        {
            ImportAssets(sources);
        }
        else if (outsideDropCount != 0)
        {
            SetStatus(
                "外部ファイルはアセットウィンドウへ"
                "ドロップしてください",
                true);
        }
    }
}
