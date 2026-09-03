// EditorLayerのアセットブラウザとアセット操作（作成/改名/削除/移動/参照リマップ/インポート/C++スクリプトビルド）をまとめた翻訳単位です。
#include "LamaPon/Editor/EditorLayer.h"

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
#include <optional>
#include <stdexcept>
#include <utility>

using namespace LamaPon::EditorDetail;

namespace
{
    // PBRマップ（粗さ・金属度・遮蔽・発光）のテクスチャ参照一覧。
    // アセットの削除警告・移動・改名の各処理で、法線マップと同じ
    // 扱いをするために使います。MeshRendererとModelRendererで
    // アクセサ名が同じなのでテンプレートです。
    struct PbrMapReference final
    {
        const char* label;
        const std::filesystem::path* path;
    };

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

    // 上と同じ4枠を、書き換え用のsetterと組で返します。
    template<typename Renderer>
    struct PbrMapAccessor final
    {
        const char* label;
        const std::filesystem::path& (Renderer::*get)()
            const noexcept;
        void (Renderer::*set)(std::filesystem::path);
    };

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

    bool IsCppIdentifier(const std::string_view value)
    {
        if (value.empty()
            || !(std::isalpha(static_cast<unsigned char>(value.front()))
                || value.front() == '_'))
        {
            return false;
        }
        return std::ranges::all_of(
            value.substr(1),
            [](const unsigned char character)
            {
                return std::isalnum(character) || character == '_';
            });
    }

    std::string CreateCppScriptSource(const std::string_view className)
    {
        const std::string name{ className };
        std::string source = R"LAMAPON(#include "LamaPon/LamaPon.h"

class __SCRIPT_NAME__ final : public LamaPon::Script
{
public:
    void Start() override
    {
        // 最初のフレームの前に1回だけ呼ばれます。初期化はここへ。
    }

    void Update(const float deltaTime) override
    {
        // 毎フレームの処理をここに書きます。よく使う書き方：
        //
        //   位置を動かす:
        //     GetTransform().position.x += deltaTime;
        //   入力（プロジェクト設定のInput Action）:
        //     const float move = Graphics().Input().Value("MoveHorizontal");
        //   コンポーネント取得:
        //     auto* body = GetComponent<LamaPon::RigidbodyComponent>();
        //   シーンから探す:
        //     auto* player = FindWithTag("Player");
        //
        (void)deltaTime;
    }
};

LAMAPON_SCRIPT(__SCRIPT_NAME__);
)LAMAPON";
        constexpr std::string_view marker = "__SCRIPT_NAME__";
        std::size_t position{};
        while ((position = source.find(marker, position))
            != std::string::npos)
        {
            source.replace(position, marker.size(), name);
            position += name.size();
        }
        return source;
    }

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

        constexpr std::string_view invalidCharacters = "<>:\"/\\|?*";
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

        const auto dot = name.find('.');
        const std::string baseName = Lowercase(
            std::string{ name.substr(0, dot) });
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
    void EditorLayer::DrawAssetDirectoryTree()
    {
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

    void EditorLayer::DrawAssetDirectoryNode(
        const std::filesystem::path& directory)
    {
        const bool hasChildren = std::ranges::any_of(
            m_assetDirectories,
            [&directory](const std::filesystem::path& candidate)
            {
                return candidate.parent_path() == directory;
            });

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

        const std::string id = PathToUtf8(directory);
        const std::string name = PathToUtf8(directory.filename());
        ImGui::PushID(id.c_str());
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

        const auto* selectedObject =
            m_scene.FindGameObject(m_selectedObjectId);
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
        const bool canAssignModel =
            selectedObject != nullptr
            && selectedObject->GetComponent<ModelRendererComponent>() != nullptr
            && IsModelAsset(asset);
        const bool canAssignMaterial =
            selectedObject != nullptr
            && (selectedObject->GetComponent<
                    MeshRendererComponent>() != nullptr
                || selectedObject->GetComponent<
                    ModelRendererComponent>() != nullptr)
            && IsMaterialAsset(asset);
        const bool canAssignAnimation =
            selectedObject != nullptr
            && selectedObject->GetComponent<
                TransformAnimatorComponent>() != nullptr
            && IsAnimationAsset(asset);
        const bool canAssignAnimatorController =
            selectedObject != nullptr
            && (selectedObject->GetComponent<
                    TransformAnimatorComponent>() != nullptr
                || selectedObject->GetComponent<
                    ModelRendererComponent>() != nullptr)
            && IsAnimatorControllerAsset(asset);

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
        if (IsShaderAsset(asset))
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
            // 今のシーンを残したまま足します（常駐UIやステージ
            // 分割の確認用）。すでに追加済みなら選べません。
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
            // このPrefabから作られたインスタンスを探して選択します
            // （どこで使われているかを確認する用）。
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
            "再インポート",
            nullptr,
            false,
            !m_playing))
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

    void EditorLayer::DrawAssetDirectoryContextMenu(
        const std::filesystem::path& directory,
        const bool isRoot)
    {
        // 右クリックで表示中のフォルダーを変えてはいけません
        // （名前変更や削除をしようとしただけで、そのフォルダーへ
        // 移動してしまいます）。メニューの各項目は引数のdirectory
        // を直接使うので、ここで状態を変える必要はありません。
        // 移動したいときはメニューの「開く」を使います。
        if (!ImGui::BeginPopupContextItem())
        {
            return;
        }

        DrawAssetDirectoryMenuContents(
            directory,
            isRoot);
        ImGui::EndPopup();
    }

    void EditorLayer::DrawAssetDirectoryMenuContents(
        const std::filesystem::path& directory,
        const bool isRoot)
    {
        if (ImGui::MenuItem("開く"))
        {
            m_assetDirectory = directory;
            m_selectedAsset.clear();
        }
        if (ImGui::MenuItem(
            "ファイルをインポート...",
            nullptr,
            false,
            !m_playing
                && m_gameModuleBuildProcess == nullptr))
        {
            m_assetDirectory = directory;
            m_selectedAsset.clear();
            OpenImportAssetsDialog();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(
            "新規フォルダー",
            nullptr,
            false,
            !m_playing))
        {
            OpenCreateAssetFolderDialog(directory);
        }
        if (ImGui::MenuItem(
            "新規シーン",
            nullptr,
            false,
            !m_playing))
        {
            OpenCreateSceneDialog(directory);
        }
        if (ImGui::MenuItem(
            "新規Lit Material",
            nullptr,
            false,
            !m_playing))
        {
            OpenCreateMaterialDialog(directory);
        }
        if (ImGui::MenuItem(
            "新規データアセット",
            nullptr,
            false,
            !m_playing))
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
            "新規カスタムShader",
            nullptr,
            false,
            !m_playing))
        {
            OpenCreateShaderDialog(directory);
        }
        if (ImGui::MenuItem(
            "新規C++ Script",
            nullptr,
            false,
            !m_playing))
        {
            OpenCreateCppScriptDialog(directory);
        }
        if (ImGui::MenuItem(
            "Game Moduleをビルド",
            nullptr,
            false,
            !m_playing))
        {
            static_cast<void>(BuildGameModule());
        }
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

    void EditorLayer::OpenCreateAssetFolderDialog(
        const std::filesystem::path& parentDirectory)
    {
        m_assetDialogTarget = parentDirectory;
        m_assetDialogRequest = AssetDialogRequest::CreateFolder;
    }

    void EditorLayer::OpenCreateSceneDialog(
        const std::filesystem::path& parentDirectory)
    {
        m_assetDialogTarget = parentDirectory;
        m_assetDialogRequest = AssetDialogRequest::CreateScene;
    }

    void EditorLayer::OpenCreateMaterialDialog(
        const std::filesystem::path& parentDirectory)
    {
        m_assetDialogTarget = parentDirectory;
        m_assetDialogRequest = AssetDialogRequest::CreateMaterial;
    }

    void EditorLayer::OpenCreateDataAssetDialog(
        const std::filesystem::path& parentDirectory)
    {
        m_assetDialogTarget = parentDirectory;
        m_assetDialogRequest =
            AssetDialogRequest::CreateDataAsset;
    }

    void EditorLayer::OpenCreateShaderDialog(
        const std::filesystem::path& parentDirectory)
    {
        m_assetDialogTarget = parentDirectory;
        m_assetDialogRequest = AssetDialogRequest::CreateShader;
    }

    void EditorLayer::OpenCreateCppScriptDialog(
        const std::filesystem::path& parentDirectory)
    {
        m_assetDialogTarget = parentDirectory;
        m_assetDialogRequest = AssetDialogRequest::CreateCppScript;
    }

    void EditorLayer::OpenRenameAssetFolderDialog(
        const std::filesystem::path& directory)
    {
        m_assetDialogTarget = directory;
        m_assetDialogRequest = AssetDialogRequest::RenameFolder;
    }

    void EditorLayer::OpenDeleteAssetFolderDialog(
        const std::filesystem::path& directory)
    {
        m_assetDialogTarget = directory;
        m_assetDialogRequest = AssetDialogRequest::DeleteFolder;
    }

    void EditorLayer::OpenRenameAssetFileDialog(
        const std::filesystem::path& asset)
    {
        m_assetDialogTarget = asset;
        m_assetDialogRequest = AssetDialogRequest::RenameFile;
    }

    void EditorLayer::OpenDeleteAssetFileDialog(
        const std::filesystem::path& asset)
    {
        m_assetDialogTarget = asset;
        m_assetDialogRequest = AssetDialogRequest::DeleteFile;
    }

    void EditorLayer::OpenPendingAssetDialog()
    {
        switch (m_assetDialogRequest)
        {
        case AssetDialogRequest::CreateFolder:
            m_assetDirectory = m_assetDialogTarget;
            m_selectedAsset.clear();
            strncpy_s(
                m_assetFolderNameBuffer.data(),
                m_assetFolderNameBuffer.size(),
                "新しいフォルダー",
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
                "新しいシーン.scene.json",
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
                "新しいMaterial.material.json",
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
                "新しいデータ.asset.json",
                _TRUNCATE);
            m_assetFileDialogError.clear();
            // 型が1つだけならそれを選んだ状態で開きます。
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

    void EditorLayer::OpenAssetInExplorer(
        const std::filesystem::path& asset,
        const bool selectFile)
    {
        try
        {
            const auto root = std::filesystem::weakly_canonical(
                m_graphics.Assets().AssetRoot());
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

            HINSTANCE result{};
            if (selectFile)
            {
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
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    void EditorLayer::OpenCodeAsset(
        const std::filesystem::path& asset)
    {
        if (!IsCppScriptAsset(asset)
            && !IsShaderAsset(asset))
        {
            return;
        }

        try
        {
            const auto root = std::filesystem::weakly_canonical(
                m_graphics.Assets().AssetRoot());
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

            // プロジェクト設定で外部エディターが指定されていれば
            // それを使い、未指定ならWindowsのファイル関連付けへ
            // フォールバックします（「システムの既定のエディター」）。
            const auto& editor =
                m_projectSettings.scriptEditorPath;
            HINSTANCE result{};
            if (!editor.empty()
                && std::filesystem::is_regular_file(editor))
            {
                const std::wstring parameters =
                    L"\"" + resolved.wstring() + L"\"";
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
                + PathToUtf8(asset));
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

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
            const auto projectRoot =
                std::filesystem::weakly_canonical(
                    m_graphics.Assets().AssetRoot().parent_path());
            const auto executableDirectory = ExecutableDirectory();
            // コマンドの構築はLamaPonCliと共有です（GameModuleBuilder）。
            // ここに直接書いていた頃は「エディターだけ通る組み立て」に
            // なる恐れがありました。
            const auto buildCommand =
                MakeGameModuleBuildCommand(
                    projectRoot,
                    m_engineRoot,
                    executableDirectory,
                    m_buildConfiguration);
            // WebDAV上のプロジェクトでソースのmtimeが古いまま見えると
            // NMakeが変更を見失う。内容ハッシュで検出して時刻を進める
            // （LamaPonCli buildと同じ対策。詳細はGameModuleBuilder.h）。
            static_cast<void>(RefreshStaleGameModuleSources(
                projectRoot,
                buildCommand.buildDirectory));
            m_gameModuleBuildLogPath =
                buildCommand.logPath;
            const std::wstring& command =
                buildCommand.parameters;

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
                "Game Moduleをバックグラウンドでビルドしています");
            return true;
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
            return false;
        }
    }

    std::filesystem::file_time_type
        EditorLayer::LatestScriptWriteTime() const
    {
        // assets配下の.cpp/.h/.hppの中で最も新しい更新時刻を返します。
        // ファイルが無ければ既定値（最小値）です。
        std::filesystem::file_time_type latest{};
        const auto& assetRoot =
            m_graphics.Assets().AssetRoot();
        std::error_code error;
        if (!std::filesystem::is_directory(assetRoot, error))
        {
            return latest;
        }

        const auto options =
            std::filesystem::directory_options::
                skip_permission_denied;
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
            const auto extension = Lowercase(
                PathToUtf8(iterator->path().extension()));
            if (extension != ".cpp"
                && extension != ".h"
                && extension != ".hpp")
            {
                continue;
            }
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

    void EditorLayer::UpdateScriptAutoBuild()
    {
        if (!m_projectSettings.autoBuildGameModuleOnSave
            || m_playing)
        {
            return;
        }

        const double now = ImGui::GetTime();
        // 走査は間隔を空けて抑えます（毎フレームだとディスクを
        // 舐め続けることになるため）。間隔は前回の実測コストから
        // 決めます: 走査に要した時間の20倍（0.5〜30秒）空けるので、
        // ローカルの小さなプロジェクトは従来どおり0.5秒間隔、
        // WebDAV上の大きなプロジェクト（1回約1秒）では20秒間隔まで
        // 自動で下がり、走査が走査を追いかける状態になりません。
        if (now - m_lastScriptScanAt >= m_scriptScanIntervalSeconds)
        {
            m_lastScriptScanAt = now;
            const auto scanStartedAt =
                std::chrono::steady_clock::now();
            const auto latest = LatestScriptWriteTime();
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
                // テンプレートやGitから既に置かれていたScriptは
                // 「保存された瞬間」を監視できません。DLLが無い／古い
                // 場合だけ初回起動時にビルドし、すぐPlayできる状態へ
                // 揃えます。
                const auto state = InspectGameModuleBuildState(
                    m_graphics.Assets().AssetRoot().parent_path());
                if (state.buildRequired)
                {
                    // 「1.5秒のデバウンスを飛ばして即ビルド」の意図だが、
                    // 起動直後は now(起動からの秒数) が1.5未満のため負に
                    // なり、「未検出」センチネル(<=0)に飲まれて初回ビルドが
                    // 一度も発火しなかった。最低でも正の値を入れる
                    m_scriptChangeDetectedAt = std::max(now - 1.5, 0.000001);
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
                    // ビルド中の変更は、完了後にもう一度回します。
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
        // 保存の連続（エディターの自動保存など）が落ち着くまで待ちます。
        if (now - m_scriptChangeDetectedAt < 1.5)
        {
            return;
        }
        m_scriptChangeDetectedAt = 0.0;
        static_cast<void>(BuildGameModule());
    }

    void EditorLayer::UpdateGameModuleBuild()
    {
        if (m_gameModuleBuildProcess == nullptr)
        {
            return;
        }

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
            SetStatus(
                "C++ Scriptのビルドに失敗しました。ログ: "
                + PathToUtf8(m_gameModuleBuildLogPath),
                true);
            return;
        }

        auto* module = GameModuleHost::Current();
        if (module == nullptr || !module->Reload())
        {
            m_pendingScriptAttachments.clear();
            SetStatus(
                module != nullptr
                    ? module->LastError()
                    : "Game Moduleを再読み込みできませんでした",
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

    void EditorLayer::QueueCppScriptAttachment(
        GameObject& gameObject,
        const std::filesystem::path& asset)
    {
        try
        {
            const auto assetRoot =
                std::filesystem::weakly_canonical(
                    m_graphics.Assets().AssetRoot());
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

            const std::string className =
                PathToUtf8(resolved.stem());
            if (!IsCppIdentifier(className))
            {
                SetStatus(
                    "C++ Script名には半角英字・数字・アンダースコアを使用してください",
                    true);
                return;
            }
            const std::string scriptType =
                "Game." + className;
            const bool alreadyAttached =
                std::ranges::any_of(
                    gameObject.Components(),
                    [&scriptType](
                        const std::unique_ptr<Component>& component)
                    {
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
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    void EditorLayer::CompletePendingCppScriptAttachments()
    {
        auto* module = GameModuleHost::Current();
        if (module == nullptr)
        {
            m_pendingScriptAttachments.clear();
            SetStatus(
                "Game Moduleが読み込まれていません",
                true);
            return;
        }

        std::size_t attachedCount{};
        std::string firstError;
        for (const auto& pending :
            m_pendingScriptAttachments)
        {
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

            const bool alreadyAttached =
                std::ranges::any_of(
                    gameObject->Components(),
                    [&pending](
                        const std::unique_ptr<Component>& component)
                    {
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
            const auto* host = GameModuleHost::Current();
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
            const std::string preview =
                m_createDataAssetTypeName.empty()
                    ? std::string{ "選択してください" }
                    : m_createDataAssetTypeName;
            if (ImGui::BeginCombo("型", preview.c_str()))
            {
                for (const auto& type :
                    host->RegisteredDataAssets())
                {
                    const bool selected =
                        type.typeName
                            == m_createDataAssetTypeName;
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
            const bool submit = ImGui::InputText(
                "名前",
                m_assetFileNameBuffer.data(),
                m_assetFileNameBuffer.size(),
                ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::TextDisabled(
                "VSMain / PSMainと4本のMaterialパラメーターを持つ雛形です。");
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

                constexpr std::size_t MaximumVisibleReferences = 8;
                const std::size_t visibleReferences = std::min(
                    MaximumVisibleReferences,
                    m_assetDeleteReferences.size());
                ImGui::PushTextWrapPos(
                    ImGui::GetCursorPosX() + 560.0f);
                for (std::size_t index = 0;
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

    bool EditorLayer::CreateAssetFolder()
    {
        const std::string name = m_assetFolderNameBuffer.data();
        if (const auto validationError =
            ValidateAssetEntryName(name, "フォルダー名"))
        {
            m_assetFolderDialogError = *validationError;
            return false;
        }

        try
        {
            const auto root = std::filesystem::weakly_canonical(
                m_graphics.Assets().AssetRoot());
            const auto parentPath =
                (root / m_assetDirectory).lexically_normal();
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

            const auto newRelativeDirectory =
                m_assetDirectory / PathFromUtf8(name);
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
        catch (const std::exception& exception)
        {
            m_assetFolderDialogError = exception.what();
            return false;
        }
    }

    bool EditorLayer::CreateSceneAsset()
    {
        const std::string name = m_assetFileNameBuffer.data();
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
            const auto root = std::filesystem::weakly_canonical(
                m_graphics.Assets().AssetRoot());
            const auto parent =
                (root / m_assetDirectory).lexically_normal();
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

            const auto relativePath =
                m_assetDirectory / PathFromUtf8(name);
            const auto destination =
                (root / relativePath).lexically_normal();
            if (!IsPathWithin(root, destination)
                || std::filesystem::exists(destination))
            {
                m_assetFileDialogError =
                    "同じ名前のファイルが存在します";
                return false;
            }

            Scene newScene(m_graphics);
            auto& cameraObject =
                newScene.CreateGameObject("メインカメラ");
            cameraObject.GetTransform().position =
                { 0.0f, 1.6f, 7.0f };
            cameraObject.GetTransform().SetEulerAngles(
                { -0.12f, 0.0f, 0.0f });
            auto& camera =
                cameraObject.AddComponent<CameraComponent>();
            newScene.SetMainCamera(camera);

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
        catch (const std::exception& exception)
        {
            m_assetFileDialogError = exception.what();
            return false;
        }
    }

    bool EditorLayer::CreateMaterialAsset()
    {
        const std::string name = m_assetFileNameBuffer.data();
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
            const auto root = std::filesystem::weakly_canonical(
                m_graphics.Assets().AssetRoot());
            const auto parent =
                (root / m_assetDirectory).lexically_normal();
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

            const auto relativePath =
                m_assetDirectory / PathFromUtf8(name);
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
        catch (const std::exception& exception)
        {
            m_assetFileDialogError = exception.what();
            return false;
        }
    }

    bool EditorLayer::CreateDataAsset()
    {
        const std::string name = m_assetFileNameBuffer.data();
        if (m_createDataAssetTypeName.empty())
        {
            m_assetFileDialogError =
                "データアセットの型を選んでください";
            return false;
        }
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
            const auto root = std::filesystem::weakly_canonical(
                m_graphics.Assets().AssetRoot());
            const auto parent =
                (root / m_assetDirectory).lexically_normal();
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

            const auto relativePath =
                m_assetDirectory / PathFromUtf8(name);
            const auto destination =
                (root / relativePath).lexically_normal();
            if (!IsPathWithin(root, destination)
                || std::filesystem::exists(destination))
            {
                m_assetFileDialogError =
                    "同じ名前のファイルが存在します";
                return false;
            }

            const auto* schema = FindDataAssetSchema(
                m_createDataAssetTypeName);
            const auto document = MakeDataAssetDocument(
                m_createDataAssetTypeName,
                schema != nullptr
                    ? std::string_view{ *schema }
                    : std::string_view{});
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
        catch (const std::exception& exception)
        {
            m_assetFileDialogError = exception.what();
            return false;
        }
    }

    bool EditorLayer::CreateShaderAsset()
    {
        const std::string name = m_assetFileNameBuffer.data();
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
            const auto root = std::filesystem::weakly_canonical(
                m_graphics.Assets().AssetRoot());
            const auto parent =
                (root / m_assetDirectory).lexically_normal();
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
            const auto relativePath =
                m_assetDirectory / PathFromUtf8(name);
            const auto destination =
                (root / relativePath).lexically_normal();
            if (!IsPathWithin(root, destination)
                || std::filesystem::exists(destination))
            {
                m_assetFileDialogError =
                    "同じ名前のファイルが存在します";
                return false;
            }
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
            m_selectedAsset = relativePath;
            RefreshAssets();
            SetStatus(
                "カスタムShaderを作成しました: "
                + PathToUtf8(relativePath));
            return true;
        }
        catch (const std::exception& exception)
        {
            m_assetFileDialogError = exception.what();
            return false;
        }
    }

    bool EditorLayer::CreateCppScriptAsset()
    {
        const std::string name = m_assetFileNameBuffer.data();
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
            const auto root = std::filesystem::weakly_canonical(
                m_graphics.Assets().AssetRoot());
            const auto parent =
                (root / m_assetDirectory).lexically_normal();
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

            const auto relativePath =
                m_assetDirectory / PathFromUtf8(name);
            const auto destination =
                (root / relativePath).lexically_normal();
            if (!IsPathWithin(root, destination)
                || std::filesystem::exists(destination))
            {
                m_assetFileDialogError =
                    "同じ名前のファイルが存在します";
                return false;
            }

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
        catch (const std::exception& exception)
        {
            m_assetFileDialogError = exception.what();
            return false;
        }
    }

    bool EditorLayer::RenameAssetFolder()
    {
        if (m_assetDirectory.empty())
        {
            m_assetFolderDialogError =
                "assetsルートの名前は変更できません";
            return false;
        }

        const std::string name = m_assetFolderNameBuffer.data();
        if (const auto validationError =
            ValidateAssetEntryName(name, "フォルダー名"))
        {
            m_assetFolderDialogError = *validationError;
            return false;
        }

        try
        {
            const auto root = std::filesystem::weakly_canonical(
                m_graphics.Assets().AssetRoot());
            const auto source =
                (root / m_assetDirectory).lexically_normal();
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

            const auto newRelativeDirectory =
                m_assetDirectory.parent_path() / PathFromUtf8(name);
            if (newRelativeDirectory == m_assetDirectory)
            {
                return true;
            }

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

            std::error_code error;
            std::filesystem::rename(source, destination, error);
            if (error)
            {
                m_assetFolderDialogError =
                    "フォルダー名を変更できませんでした";
                return false;
            }

            const auto oldRelativeDirectory = m_assetDirectory;
            static_cast<void>(
                m_graphics.Assets().Database().Refresh(
                    true));
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
        catch (const std::exception& exception)
        {
            m_assetFolderDialogError = exception.what();
            return false;
        }
    }

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
            const auto root = std::filesystem::weakly_canonical(
                m_graphics.Assets().AssetRoot());
            const auto source =
                (root / m_assetDirectory).lexically_normal();
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

            const std::string deletedName =
                PathToUtf8(m_assetDirectory.filename());
            m_assetDirectory = m_assetDirectory.parent_path();
            m_selectedAsset.clear();
            RefreshAssets();
            SetStatus("フォルダーを削除しました: " + deletedName);
            return true;
        }
        catch (const std::exception& exception)
        {
            m_assetFolderDialogError = exception.what();
            return false;
        }
    }

    bool EditorLayer::RenameSelectedAsset()
    {
        if (m_selectedAsset.empty())
        {
            m_assetFileDialogError = "ファイルを選択してください";
            return false;
        }

        const std::string name = m_assetFileNameBuffer.data();
        if (const auto validationError =
            ValidateAssetEntryName(name, "ファイル名"))
        {
            m_assetFileDialogError = *validationError;
            return false;
        }

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
                    // PBRマップと発光マップも参照として数えます。
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
                    // PBRマップと発光マップも参照として数えます。
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

                const auto hierarchyAbsolute =
                    m_graphics.Assets().ResolvePath(hierarchyAsset);
                if (IsSceneAsset(hierarchyAsset)
                    && !m_scenePath.empty()
                    && NormalizeAssetReference(hierarchyAbsolute)
                        == NormalizeAssetReference(m_scenePath))
                {
                    continue;
                }

                std::ifstream input(hierarchyAbsolute, std::ios::binary);
                if (!input)
                {
                    m_assetDeleteScanError =
                        "参照を確認できないシーンまたはPrefabがあります: "
                        + PathToUtf8(hierarchyAsset);
                    return;
                }

                nlohmann::json hierarchyJson;
                input >> hierarchyJson;
                const auto objects = hierarchyJson.find("objects");
                if (objects == hierarchyJson.end() || !objects->is_array())
                {
                    m_assetDeleteScanError =
                        "シーンまたはPrefab形式を確認できません: "
                        + PathToUtf8(hierarchyAsset);
                    return;
                }

                for (const auto& object : *objects)
                {
                    if (!object.is_object())
                    {
                        continue;
                    }
                    const std::string objectName =
                        object.value("name", "GameObject");
                    const auto components = object.find("components");
                    if (components == object.end()
                        || !components->is_array())
                    {
                        continue;
                    }

                    for (const auto& component : *components)
                    {
                        if (!component.is_object())
                        {
                            continue;
                        }

                        const std::string type =
                            component.value("type", "");
                        // ModelRendererが最多で、model/albedo/normal/
                        // materialAsset/animationController/shader＋
                        // PBRマップ4枠の計10個です。
                        std::array<const char*, 10>
                            referenceFields{};
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

                        for (std::size_t fieldIndex = 0;
                            fieldIndex < referenceFieldCount;
                            ++fieldIndex)
                        {
                            const char* referenceField =
                                referenceFields[fieldIndex];
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
                nlohmann::json controllerJson;
                input >> controllerJson;
                for (const auto& state :
                    controllerJson.value(
                        "states",
                        nlohmann::json::array()))
                {
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
                    if (const auto blendTree =
                            state.find("blendTree");
                        blendTree != state.end()
                            && blendTree->is_object())
                    {
                        for (const auto& child :
                            blendTree->value(
                                "children",
                                nlohmann::json::array()))
                        {
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

            for (const auto& materialAsset : m_assetFiles)
            {
                if (!IsMaterialAsset(materialAsset)
                    || IsSameAssetReference(
                        materialAsset,
                        m_selectedAsset))
                {
                    continue;
                }

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
                // .material.json内のPBRマップも参照として数えます。
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
            if (const auto* selectedRecord =
                    m_graphics.Assets().Database().
                        FindByPath(m_selectedAsset))
            {
                for (const auto& dependentGuid :
                    selectedRecord->dependents)
                {
                    const auto* dependent =
                        m_graphics.Assets().Database().
                            FindByGuid(dependentGuid);
                    if (dependent == nullptr)
                    {
                        continue;
                    }
                    const auto dependentPath =
                        PathToUtf8(dependent->path);
                    const bool isCurrentScene =
                        !m_scenePath.empty()
                        && NormalizeAssetReference(
                            m_graphics.Assets().
                                ResolvePath(
                                    dependent->path))
                            == NormalizeAssetReference(
                                m_scenePath);
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
        catch (const std::exception& exception)
        {
            m_assetDeleteScanError =
                std::string{ "アセット参照を確認できません: " }
                + exception.what();
        }
    }

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

        const auto deletedAsset = m_selectedAsset;
        try
        {
            const auto root = std::filesystem::weakly_canonical(
                m_graphics.Assets().AssetRoot());
            const auto source =
                (root / deletedAsset).lexically_normal();
            const auto sourceMeta =
                AssetDatabase::MetaPathFor(source);
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

            std::filesystem::path stagingPath;
            for (std::size_t suffix = 0; suffix < 1000; ++suffix)
            {
                std::wstring stagingName =
                    source.filename().wstring()
                    + L".lamapon-delete";
                if (suffix != 0)
                {
                    stagingName += L"-" + std::to_wstring(suffix);
                }
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

                for (const auto& asset : m_assetFiles)
                {
                    if (IsModelAsset(asset)
                        && !IsSameAssetReference(asset, deletedAsset))
                    {
                        static_cast<void>(
                            m_graphics.Assets().LoadModel(asset));
                    }
                }

                for (const auto& gameObject : m_scene.GameObjects())
                {
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
            catch (const std::exception& exception)
            {
                std::error_code rollbackError;
                std::filesystem::rename(
                    stagingPath,
                    source,
                    rollbackError);
                m_graphics.Assets().Clear();
                for (const auto& gameObject : m_scene.GameObjects())
                {
                    try
                    {
                        if (auto* sprite =
                            gameObject->GetComponent<
                                SpriteRendererComponent>();
                            sprite != nullptr
                            && !sprite->TexturePath().empty())
                        {
                            sprite->SetTexturePath(
                                sprite->TexturePath());
                        }
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
                        if (auto* particles =
                            gameObject->GetComponent<
                                ParticleSystemComponent>();
                            particles != nullptr
                            && !particles->TexturePath().empty())
                        {
                            particles->SetTexturePath(
                                particles->TexturePath());
                        }
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
                            // PBRマップも同じパスを入れ直して
                            // 読み込みをやり直させます。
                            for (const auto& accessor :
                                PbrMapAccessors<
                                    ModelRendererComponent>())
                            {
                                (model->*accessor.set)(
                                    (model->*accessor.get)());
                            }
                        }
                        if (auto* mesh =
                            gameObject->GetComponent<
                                MeshRendererComponent>())
                        {
                            mesh->SetAlbedoTexturePath(
                                mesh->AlbedoTexturePath());
                            mesh->SetNormalTexturePath(
                                mesh->NormalTexturePath());
                            for (const auto& accessor :
                                PbrMapAccessors<
                                    MeshRendererComponent>())
                            {
                                (mesh->*accessor.set)(
                                    (mesh->*accessor.get)());
                            }
                        }
                    }
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

            if (!std::filesystem::remove(stagingPath, error) || error)
            {
                std::error_code rollbackError;
                std::filesystem::rename(
                    stagingPath,
                    source,
                    rollbackError);
                m_graphics.Assets().Clear();
                m_assetFileDialogError =
                    rollbackError
                        ? "削除とロールバックに失敗しました。"
                        : "ファイルを削除できなかったため元に戻しました。";
                return false;
            }
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

            std::size_t clearedReferences{};
            for (const auto& gameObject : m_scene.GameObjects())
            {
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
                    // PBRマップの参照もクリアします。
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
                    // PBRマップの参照もクリアします。
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
        catch (const std::exception& exception)
        {
            m_assetFileDialogError = exception.what();
            return false;
        }
    }

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
        // 移動先がルート以外なら、管理下のフォルダーであること。
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
            // すでにその場所にあるので何もしません。
            return true;
        }
        // 自分自身や自分の子孫へは移動できません。空パス（ルート）を
        // 絶対パス化すると環境依存になるため、字句的に判定します。
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

        const auto destinationRelative =
            targetDirectory / sourceDirectory.filename();
        try
        {
            const auto root =
                std::filesystem::weakly_canonical(
                    m_graphics.Assets().AssetRoot());
            const auto source =
                (root / sourceDirectory).lexically_normal();
            const auto destination =
                (root / destinationRelative).
                    lexically_normal();
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

            // 中身のファイルはフォルダーごと動くため、参照は
            // パスの先頭一致で付け替えます（名前変更と同じ流儀）。
            static_cast<void>(
                m_graphics.Assets().Database().Refresh(
                    true));
            const auto remapResult =
                m_graphics.Assets().Database().
                    RemapJsonReferences(
                        sourceDirectory,
                        destinationRelative,
                        true);
            RemapAssetReferences(
                sourceDirectory,
                destinationRelative);
            if (const auto remapped = RemapPathPrefix(
                    m_assetDirectory,
                    sourceDirectory,
                    destinationRelative))
            {
                // 表示中のフォルダーが移動対象の中にあった場合は
                // 追従させます。
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
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
            return false;
        }
    }

    bool EditorLayer::MoveAssetFile(
        const std::filesystem::path& sourceAsset,
        const std::filesystem::path& targetDirectory)
    {
        return RelocateAssetFile(
            sourceAsset,
            targetDirectory / sourceAsset.filename());
    }

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
            const auto root = std::filesystem::weakly_canonical(
                m_graphics.Assets().AssetRoot());
            const auto source =
                (root / sourceAsset).lexically_normal();
            const auto destination =
                (root / destinationAsset).lexically_normal();
            const auto destinationDirectory = destination.parent_path();
            const auto sourceMeta =
                AssetDatabase::MetaPathFor(source);
            const auto destinationMeta =
                AssetDatabase::MetaPathFor(
                    destination);
            const auto resolvedSource =
                std::filesystem::weakly_canonical(source);
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

            std::error_code error;
            std::filesystem::rename(source, destination, error);
            if (error)
            {
                SetStatus("ファイルを移動できませんでした", true);
                return false;
            }
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

                for (const auto& gameObject : m_scene.GameObjects())
                {
                    if (const auto* sprite =
                        gameObject->GetComponent<SpriteRendererComponent>();
                        sprite != nullptr
                        && !sprite->TexturePath().empty())
                    {
                        const auto prospectivePath =
                            sprite->TexturePath() == sourceAsset
                                ? destinationAsset
                                : sprite->TexturePath();
                        static_cast<void>(
                            m_graphics.Assets().LoadTexture(
                                prospectivePath));
                    }
                    if (const auto* tilemap =
                        gameObject->GetComponent<
                            TilemapComponent>();
                        tilemap != nullptr
                        && !tilemap->
                            TexturePath().empty())
                    {
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
                    if (const auto* particles =
                        gameObject->GetComponent<
                            ParticleSystemComponent>();
                        particles != nullptr
                        && !particles->TexturePath().empty())
                    {
                        const auto prospectivePath =
                            particles->TexturePath()
                                == sourceAsset
                            ? destinationAsset
                            : particles->TexturePath();
                        static_cast<void>(
                            m_graphics.Assets().LoadTexture(
                                prospectivePath));
                    }
                    if (const auto* model =
                        gameObject->GetComponent<ModelRendererComponent>();
                        model != nullptr)
                    {
                        if (!model->ModelPath().empty())
                        {
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
                            const auto prospectivePath =
                                model->NormalTexturePath()
                                    == sourceAsset
                                    ? destinationAsset
                                    : model->NormalTexturePath();
                            static_cast<void>(
                                m_graphics.Assets().LoadTexture(
                                    prospectivePath));
                        }
                        // PBRマップも移動後のパスで先読みします。
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
                    if (const auto* mesh =
                        gameObject->GetComponent<MeshRendererComponent>();
                        mesh != nullptr)
                    {
                        if (!mesh->AlbedoTexturePath().empty())
                        {
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
                            const auto prospectivePath =
                                mesh->NormalTexturePath()
                                    == sourceAsset
                                    ? destinationAsset
                                    : mesh->NormalTexturePath();
                            static_cast<void>(
                                m_graphics.Assets().LoadTexture(
                                    prospectivePath));
                        }
                        // PBRマップも移動後のパスで先読みします。
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

                RemapAssetFileReferences(
                    sourceAsset,
                    destinationAsset);

                for (const auto& gameObject : m_scene.GameObjects())
                {
                    if (auto* sprite =
                        gameObject->GetComponent<SpriteRendererComponent>();
                        sprite != nullptr
                        && !sprite->TexturePath().empty())
                    {
                        sprite->SetTexturePath(sprite->TexturePath());
                    }
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
                    if (auto* particles =
                        gameObject->GetComponent<
                            ParticleSystemComponent>();
                        particles != nullptr
                        && !particles->TexturePath().empty())
                    {
                        particles->SetTexturePath(
                            particles->TexturePath());
                    }
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
                        for (const auto& accessor :
                            PbrMapAccessors<
                                ModelRendererComponent>())
                        {
                            (model->*accessor.set)(
                                (model->*accessor.get)());
                        }
                    }
                    if (auto* mesh =
                        gameObject->GetComponent<MeshRendererComponent>())
                    {
                        mesh->SetAlbedoTexturePath(
                            mesh->AlbedoTexturePath());
                        mesh->SetNormalTexturePath(
                            mesh->NormalTexturePath());
                        for (const auto& accessor :
                            PbrMapAccessors<
                                MeshRendererComponent>())
                        {
                            (mesh->*accessor.set)(
                                (mesh->*accessor.get)());
                        }
                    }
                }
                const auto remapResult =
                    m_graphics.Assets().Database().
                        RemapJsonReferences(
                            sourceAsset,
                            destinationAsset);
                remappedReferenceCount =
                    remapResult.referenceCount;
            }
            catch (const std::exception& exception)
            {
                std::error_code rollbackError;
                std::filesystem::rename(
                    destination,
                    source,
                    rollbackError);
                if (hadMeta)
                {
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
                    for (const auto& gameObject : m_scene.GameObjects())
                    {
                        try
                        {
                            if (auto* sprite =
                                gameObject->GetComponent<
                                    SpriteRendererComponent>();
                                sprite != nullptr
                                && !sprite->TexturePath().empty())
                            {
                                sprite->SetTexturePath(
                                    sprite->TexturePath());
                            }
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
                                for (const auto& accessor :
                                    PbrMapAccessors<
                                        ModelRendererComponent>())
                                {
                                    (model->*accessor.set)(
                                        (model->*accessor.get)());
                                }
                            }
                            if (auto* mesh =
                                gameObject->GetComponent<
                                    MeshRendererComponent>())
                            {
                                mesh->SetAlbedoTexturePath(
                                    mesh->AlbedoTexturePath());
                                mesh->SetNormalTexturePath(
                                    mesh->NormalTexturePath());
                                for (const auto& accessor :
                                    PbrMapAccessors<
                                        MeshRendererComponent>())
                                {
                                    (mesh->*accessor.set)(
                                        (mesh->*accessor.get)());
                                }
                            }
                        }
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
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
            return false;
        }
    }

    void EditorLayer::BeginAssetFolderDragSource(
        const std::filesystem::path& directory)
    {
        if (m_playing || directory.empty())
        {
            return;
        }
        // 左ボタンで少し引きずるとドラッグ開始です（右クリックは
        // コンテキストメニューのままにするため、既定の左ボタン
        // 判定を使います）。
        if (!ImGui::BeginDragDropSource())
        {
            return;
        }

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

    void EditorLayer::AcceptAssetMoveDrop(
        const std::filesystem::path& targetDirectory)
    {
        if (m_playing || !ImGui::BeginDragDropTarget())
        {
            return;
        }

        if (const ImGuiPayload* payload =
            ImGui::AcceptDragDropPayload(AssetPayload))
        {
            const auto source = PathFromUtf8(
                static_cast<const char*>(payload->Data));
            static_cast<void>(
                MoveAssetFile(source, targetDirectory));
        }
        // フォルダーを落としたら、中身ごと移動します。
        if (const ImGuiPayload* payload =
            ImGui::AcceptDragDropPayload(
                AssetFolderPayload))
        {
            const auto source = PathFromUtf8(
                static_cast<const char*>(payload->Data));
            static_cast<void>(
                MoveAssetFolder(source, targetDirectory));
        }
        // HierarchyのGameObjectを落としたら、その場でPrefabを
        // その場でPrefabを作成します。
        if (const ImGuiPayload* payload =
            ImGui::AcceptDragDropPayload(GameObjectPayload))
        {
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

    void EditorLayer::CreatePrefabFromGameObject(
        const GameObjectId id,
        const std::filesystem::path& targetDirectory)
    {
        auto* gameObject = m_scene.FindGameObject(id);
        if (gameObject == nullptr || m_playing)
        {
            return;
        }

        try
        {
            const auto root =
                m_graphics.Assets().AssetRoot();
            // 同名があれば連番を付けて衝突を避けます。
            auto fileName =
                PathFromUtf8(gameObject->Name());
            if (fileName.empty())
            {
                fileName = L"GameObject";
            }
            auto relative = targetDirectory
                / (fileName.wstring() + L".prefab.json");
            for (int suffix = 2;
                std::filesystem::exists(root / relative)
                    && suffix < 1000;
                ++suffix)
            {
                relative = targetDirectory
                    / (fileName.wstring()
                        + L" ("
                        + std::to_wstring(suffix)
                        + L").prefab.json");
            }

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
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    void EditorLayer::RemapAssetReferences(
        const std::filesystem::path& oldDirectory,
        const std::filesystem::path& newDirectory)
    {
        if (const auto remapped =
            RemapPathPrefix(
                m_selectedAsset,
                oldDirectory,
                newDirectory))
        {
            m_selectedAsset = *remapped;
        }

        const auto root = m_graphics.Assets().AssetRoot();
        const auto oldAbsolute = root / oldDirectory;
        const auto newAbsolute = root / newDirectory;
        if (const auto remapped =
            RemapPathPrefix(
                m_scenePath,
                oldAbsolute,
                newAbsolute))
        {
            m_scenePath = *remapped;
        }

        if (const auto remapped =
            RemapPathPrefix(
                m_projectSettings.startupScene,
                oldDirectory,
                newDirectory))
        {
            m_projectSettings.startupScene = *remapped;
            SaveProjectConfiguration();
        }

        for (const auto& gameObject : m_scene.GameObjects())
        {
            if (const auto remapped =
                RemapPathPrefix(
                    gameObject->PrefabAssetPath(),
                    oldDirectory,
                    newDirectory))
            {
                gameObject->SetPrefabAssetPath(
                    *remapped);
            }
            if (auto* sprite =
                gameObject->GetComponent<SpriteRendererComponent>())
            {
                if (const auto remapped =
                    RemapPathPrefix(
                        sprite->TexturePath(),
                        oldDirectory,
                        newDirectory))
                {
                    sprite->SetTexturePath(*remapped);
                }
                if (const auto remapped =
                    RemapPathPrefix(
                        sprite->ShaderPath(),
                        oldDirectory,
                        newDirectory))
                {
                    sprite->SetShaderPath(*remapped);
                }
            }
            if (auto* tilemap =
                gameObject->GetComponent<
                    TilemapComponent>())
            {
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
            if (auto* particles =
                gameObject->GetComponent<
                    ParticleSystemComponent>())
            {
                if (const auto remapped =
                    RemapPathPrefix(
                        particles->TexturePath(),
                        oldDirectory,
                        newDirectory))
                {
                    particles->SetTexturePath(
                        *remapped);
                }
                if (const auto remapped =
                    RemapPathPrefix(
                        particles->ShaderPath(),
                        oldDirectory,
                        newDirectory))
                {
                    particles->SetShaderPath(
                        *remapped);
                }
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
            if (auto* button =
                gameObject->GetComponent<
                    UIButtonComponent>())
            {
                if (const auto remapped =
                    RemapPathPrefix(
                        button->TexturePath(),
                        oldDirectory,
                        newDirectory))
                {
                    button->SetTexturePath(
                        *remapped);
                }
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
            if (auto* animator =
                gameObject->GetComponent<
                    TransformAnimatorComponent>())
            {
                if (const auto remapped =
                    RemapPathPrefix(
                        animator->ClipPath(),
                        oldDirectory,
                        newDirectory))
                {
                    animator->SetClipPath(
                        *remapped);
                }
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
            if (auto* model =
                gameObject->GetComponent<ModelRendererComponent>())
            {
                if (const auto remapped =
                    RemapPathPrefix(
                        model->AnimationControllerPath(),
                        oldDirectory,
                        newDirectory))
                {
                    model->SetAnimationControllerPath(
                        *remapped);
                }
                if (const auto remapped =
                    RemapPathPrefix(
                        model->MaterialAssetPath(),
                        oldDirectory,
                        newDirectory))
                {
                    model->SetMaterialAssetPath(*remapped);
                }
                if (const auto remapped =
                    RemapPathPrefix(
                        model->ModelPath(),
                        oldDirectory,
                        newDirectory))
                {
                    model->SetModelPath(*remapped);
                }
                if (const auto remapped =
                    RemapPathPrefix(
                        model->AlbedoTexturePath(),
                        oldDirectory,
                        newDirectory))
                {
                    model->SetAlbedoTexturePath(*remapped);
                }
                if (const auto remapped =
                    RemapPathPrefix(
                        model->NormalTexturePath(),
                        oldDirectory,
                        newDirectory))
                {
                    model->SetNormalTexturePath(*remapped);
                }
                // PBRマップのパスも接頭辞を差し替えます。
                for (const auto& accessor :
                    PbrMapAccessors<ModelRendererComponent>())
                {
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
            if (auto* mesh =
                gameObject->GetComponent<MeshRendererComponent>())
            {
                if (const auto remapped =
                    RemapPathPrefix(
                        mesh->MaterialAssetPath(),
                        oldDirectory,
                        newDirectory))
                {
                    mesh->SetMaterialAssetPath(*remapped);
                }
                if (const auto remapped =
                    RemapPathPrefix(
                        mesh->AlbedoTexturePath(),
                        oldDirectory,
                        newDirectory))
                {
                    mesh->SetAlbedoTexturePath(*remapped);
                }
                if (const auto remapped =
                    RemapPathPrefix(
                        mesh->NormalTexturePath(),
                        oldDirectory,
                        newDirectory))
                {
                    mesh->SetNormalTexturePath(*remapped);
                }
                // PBRマップのパスも接頭辞を差し替えます。
                for (const auto& accessor :
                    PbrMapAccessors<MeshRendererComponent>())
                {
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

    void EditorLayer::RemapAssetFileReferences(
        const std::filesystem::path& oldAsset,
        const std::filesystem::path& newAsset)
    {
        if (m_selectedAsset == oldAsset)
        {
            m_selectedAsset = newAsset;
        }

        const auto oldAbsolute =
            m_graphics.Assets().ResolvePath(oldAsset);
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
            SaveProjectConfiguration();
        }

        for (const auto& gameObject : m_scene.GameObjects())
        {
            if (gameObject->PrefabAssetPath() == oldAsset)
            {
                gameObject->SetPrefabAssetPath(
                    newAsset);
            }
            if (auto* sprite =
                gameObject->GetComponent<SpriteRendererComponent>();
                sprite != nullptr
                && sprite->TexturePath() == oldAsset)
            {
                sprite->SetTexturePath(newAsset);
            }
            if (auto* sprite =
                gameObject->GetComponent<SpriteRendererComponent>();
                sprite != nullptr
                && sprite->ShaderPath() == oldAsset)
            {
                sprite->SetShaderPath(newAsset);
            }
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
                // PBRマップの参照も新しいパスへ差し替えます。
                for (const auto& accessor :
                    PbrMapAccessors<ModelRendererComponent>())
                {
                    if ((model->*accessor.get)() == oldAsset)
                    {
                        (model->*accessor.set)(newAsset);
                    }
                }
            }
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
                // PBRマップの参照も新しいパスへ差し替えます。
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

    std::shared_ptr<const TextureAsset> EditorLayer::GetFileTypeIcon(
        AssetIconKind kind)
    {
        const auto index = static_cast<std::size_t>(kind);
        if (m_fileTypeIconAttempted[index])
        {
            return m_fileTypeIcons[index];
        }
        m_fileTypeIconAttempted[index] = true;

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

    void EditorLayer::DrawAssetBrowser()
    {
        if (!m_assetBrowserPanelOpen)
        {
            return;
        }

        ImGui::SetNextWindowSize(
            ImVec2{ HierarchyWidth, 420.0f },
            ImGuiCond_FirstUseEver);

        constexpr ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoCollapse;

        if (!ImGui::Begin("アセット", &m_assetBrowserPanelOpen, flags))
        {
            ImGui::End();
            return;
        }

        const ImVec2 assetBrowserPosition =
            ImGui::GetWindowPos();
        const ImVec2 assetBrowserSize =
            ImGui::GetWindowSize();
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

        const ImGuiStyle& style = ImGui::GetStyle();
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

        const std::string filter = Lowercase(m_assetFilter.data());
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

            std::filesystem::path breadcrumb;
            const std::filesystem::path currentAssetDirectory =
                m_assetDirectory;
            for (const auto& part : currentAssetDirectory)
            {
                breadcrumb /= part;
                ImGui::SameLine();
                ImGui::TextUnformatted(">");
                ImGui::SameLine();
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
            const int columnCount = std::max(
                1,
                static_cast<int>(ImGui::GetContentRegionAvail().x / 88.0f));

            if (ImGui::BeginTable(
                "AssetGrid",
                columnCount,
                ImGuiTableFlags_SizingFixedFit))
            {
                const auto drawTypeButton = [this](
                    AssetIconKind kind,
                    const char* imageId,
                    const char* fallbackLabel,
                    const ImVec2& size) -> bool
                {
                    if (const auto icon = GetFileTypeIcon(kind))
                    {
                        return ImGui::ImageButton(
                            imageId,
                            MakeTextureReference(icon->view.Get()),
                            size);
                    }
                    return ImGui::Button(fallbackLabel, size);
                };

                if (!searching)
                {
                    for (const auto& directory : m_assetDirectories)
                    {
                        if (directory.parent_path() != m_assetDirectory)
                        {
                            continue;
                        }

                        ImGui::TableNextColumn();
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

                for (const auto& asset : m_assetFiles)
                {
                    const std::string label = PathToUtf8(asset);
                    if (searching
                        ? Lowercase(label).find(filter) == std::string::npos
                        : asset.parent_path() != m_assetDirectory)
                    {
                        continue;
                    }

                    ImGui::TableNextColumn();
                    ImGui::PushID(label.c_str());

                    const bool selected = asset == m_selectedAsset;
                    ImGui::PushStyleVar(
                        ImGuiStyleVar_FrameBorderSize,
                        selected ? 2.0f : 0.0f);
                    ImGui::PushStyleColor(
                        ImGuiCol_Border,
                        ImVec4{ 0.20f, 0.75f, 1.0f, 1.0f });

                    bool clicked = false;
                    if (IsTextureAsset(asset))
                    {
                        try
                        {
                            const auto texture =
                                m_graphics.Assets().LoadTexture(asset);
                            clicked = ImGui::ImageButton(
                                "##AssetThumbnail",
                                MakeTextureReference(texture->view.Get()),
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
                    else if (IsShaderAsset(asset))
                    {
                        clicked = drawTypeButton(
                            AssetIconKind::Shader,
                            "##ShaderIcon",
                            "HLSL",
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
                    else if (IsShaderAsset(asset)
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
                const auto folderIcon = GetFileTypeIcon(AssetIconKind::Folder);
                for (const auto& directory : m_assetDirectories)
                {
                    if (directory.parent_path() != m_assetDirectory)
                    {
                        continue;
                    }

                    const std::string label = folderIcon
                        ? PathToUtf8(directory.filename())
                        : "[フォルダー] " + PathToUtf8(directory.filename());
                    ImGui::PushID(PathToUtf8(directory).c_str());
                    if (folderIcon)
                    {
                        ImGui::Image(
                            MakeTextureReference(folderIcon->view.Get()),
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

            for (const auto& asset : m_assetFiles)
            {
                const std::string pathLabel = PathToUtf8(asset);
                if (searching
                    ? Lowercase(pathLabel).find(filter) == std::string::npos
                    : asset.parent_path() != m_assetDirectory)
                {
                    continue;
                }

                const std::string label = searching
                    ? pathLabel
                    : PathToUtf8(asset.filename());
                const bool selected = asset == m_selectedAsset;
                ImGui::PushID(pathLabel.c_str());
                const auto listIcon = GetFileTypeIcon(
                    IsTextureAsset(asset) ? AssetIconKind::Generic
                    : IsSceneAsset(asset) ? AssetIconKind::Scene
                    : IsPrefabAsset(asset) ? AssetIconKind::Prefab
                    : IsModelAsset(asset) ? AssetIconKind::Model
                    : IsMaterialAsset(asset) ? AssetIconKind::Material
                    : IsShaderAsset(asset) ? AssetIconKind::Shader
                    : IsAnimationAsset(asset) ? AssetIconKind::Animation
                    : IsAnimatorControllerAsset(asset)
                        ? AssetIconKind::AnimatorController
                    : IsCppScriptAsset(asset) ? AssetIconKind::CppScript
                    : AssetIconKind::Generic);
                if (listIcon)
                {
                    ImGui::Image(
                        MakeTextureReference(listIcon->view.Get()),
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
                else if (IsShaderAsset(asset)
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

    void EditorLayer::OpenImportAssetsDialog()
    {
        std::array<wchar_t, 65536> selectedFiles{};
        constexpr wchar_t filter[] =
            L"対応アセット\0"
            L"*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff;*.dds;"
            L"*.cmo;*.sdkmesh;*.vbo;*.gltf;*.glb;*.fbx;"
            L"*.wav;*.hlsl;*.cpp;*.json\0"
            L"すべてのファイル (*.*)\0*.*\0\0";

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

        std::vector<std::filesystem::path> sources;
        const wchar_t* first = selectedFiles.data();
        const wchar_t* next = first + std::wcslen(first) + 1;
        if (*next == L'\0')
        {
            sources.emplace_back(first);
        }
        else
        {
            const std::filesystem::path directory{ first };
            while (*next != L'\0')
            {
                sources.push_back(directory / next);
                next += std::wcslen(next) + 1;
            }
        }
        ImportAssets(sources);
    }

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
            const auto importResult = AssetImporter::Import(
                sources,
                m_graphics.Assets().AssetRoot(),
                m_assetDirectory);
            const bool importedAnything =
                !importResult.files.empty()
                || importResult.importedDirectoryCount != 0;
            if (importedAnything)
            {
                RefreshAssets();
                // キャッシュ全消し（Clear）は、シーンが使用中の
                // モデルやテクスチャをGPU上へ二重に確保させ、
                // インポートを繰り返すとメモリ枯渇で落ちるため、
                // インポートしたファイルだけを無効化します。
                for (const auto& imported : importResult.files)
                {
                    m_graphics.Assets().Invalidate(
                        imported.path);
                }
            }

            std::vector<std::string> failures;
            failures.reserve(
                importResult.failures.size());
            for (const auto& failure : importResult.failures)
            {
                failures.push_back(
                    PathToUtf8(failure.source.filename())
                    + ": "
                    + failure.message);
            }

            std::size_t queuedCount{};
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
        catch (const std::exception& exception)
        {
            SetStatus(
                std::string{ "インポートに失敗しました: " }
                    + exception.what(),
                true);
        }
    }

    void EditorLayer::ProcessPendingAssetImports()
    {
        if (m_pendingAssetImports.empty()
            || m_playing
            || m_gameModuleBuildProcess != nullptr)
        {
            return;
        }

        const auto asset = m_pendingAssetImports.front();
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
                std::string preparationError;
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

    void EditorLayer::ProcessExternalAssetDrops(
        const RECT& assetBrowserBounds)
    {
        if (m_pendingExternalAssetDrops.empty()
            || m_gameModuleBuildProcess != nullptr)
        {
            return;
        }

        auto pendingDrops =
            std::move(m_pendingExternalAssetDrops);
        m_pendingExternalAssetDrops.clear();
        std::vector<std::filesystem::path> sources;
        std::size_t outsideDropCount{};
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
