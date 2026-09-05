#pragma once

#include "LamaPon/Core/PathUtils.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

struct ID3D11ShaderResourceView;

// EditorLayerの分割翻訳単位（EditorLayer*.cpp）で共有する
// 小さなヘルパー群です。特定の翻訳単位でしか使わない大きな
// ヘルパーは、その翻訳単位の匿名名前空間に置いてください。
namespace LamaPon::EditorDetail
{
    constexpr float HierarchyWidth = 290.0f;

    constexpr const char* GameObjectPayload = "LAMAPON_GAME_OBJECT";

    // インスペクター内のコンポーネント並び替え（Component*を運ぶ）。
    constexpr const char* ComponentPayload = "LAMAPON_COMPONENT";

    constexpr const char* AssetPayload = "LAMAPON_ASSET_PATH";

    // フォルダーのドラッグはファイルと種別を分けます（フォルダーを
    // ファイル用の受け取り先へ落とせないようにするため）。
    constexpr const char* AssetFolderPayload =
        "LAMAPON_ASSET_FOLDER";

    // 表示名と保存名は分けます。日本語などを含む表示名には英語の既定名を
    // 提案し、メニュー保存とドラッグ作成で同じファイル命名規則を使います。
    inline std::wstring SuggestedPrefabFileStem(const std::string_view displayName)
    {
        std::wstring stem = PathFromUtf8(displayName).filename().wstring();
        for (auto& character : stem)
        {
            if (character > 0x7f)
            {
                return L"NewPrefab";
            }
            if (character < L' '
                || std::wstring_view{ L"<>:\"/\\|?*" }.find(character)
                    != std::wstring_view::npos)
            {
                character = L'_';
            }
        }
        while (!stem.empty() && (stem.back() == L' ' || stem.back() == L'.'))
        {
            stem.pop_back();
        }
        return stem.empty() ? L"NewPrefab" : stem;
    }

    inline std::string Lowercase(std::string value)
    {
        std::ranges::transform(
            value,
            value.begin(),
            [](const unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
        return value;
    }

    inline std::string PrefabOverrideFieldLabel(
        const std::string_view field)
    {
        if (field == "name") return "名前";
        if (field == "enabled") return "有効";
        if (field == "transform") return "Transform";
        if (field == "position") return "位置";
        if (field == "rotation") return "回転";
        if (field == "scale") return "拡縮";
        if (field == "prefabAsset") return "Nested Prefab";
        if (field == "objects") return "GameObject階層";
        if (field == "components") return "Component";
        if (field == "color") return "カラー";
        if (field == "roughness") return "粗さ";
        if (field == "normalStrength") return "法線強度";
        return std::string{ field };
    }

    inline std::string FormatPrefabOverridePath(
        const std::string_view path)
    {
        std::vector<std::string_view> tokens;
        std::size_t start = path.starts_with('/')
            ? 1
            : 0;
        while (start < path.size())
        {
            const auto end = path.find('/', start);
            tokens.push_back(
                path.substr(
                    start,
                    end == std::string_view::npos
                        ? path.size() - start
                        : end - start));
            if (end == std::string_view::npos)
            {
                break;
            }
            start = end + 1;
        }

        std::string result;
        for (std::size_t index = 0;
            index < tokens.size();
            ++index)
        {
            std::string part;
            if (tokens[index] == "objects"
                && index + 1 < tokens.size())
            {
                part = "GameObject["
                    + std::string{ tokens[++index] }
                    + "]";
            }
            else if (tokens[index]
                    == "components"
                && index + 1 < tokens.size())
            {
                part = "Component["
                    + std::string{ tokens[++index] }
                    + "]";
            }
            else
            {
                part = PrefabOverrideFieldLabel(
                    tokens[index]);
            }
            if (!result.empty())
            {
                result += " / ";
            }
            result += part;
        }
        return result.empty()
            ? std::string{ path }
            : result;
    }

    inline std::string NormalizeAssetReference(
        const std::filesystem::path& path)
    {
        return Lowercase(
            LamaPon::PathToUtf8(path.lexically_normal()));
    }

    inline bool IsSameAssetReference(
        const std::filesystem::path& left,
        const std::filesystem::path& right)
    {
        return !left.empty()
            && !right.empty()
            && NormalizeAssetReference(left)
                == NormalizeAssetReference(right);
    }

    inline bool IsTextureAsset(const std::filesystem::path& path)
    {
        auto normalized = Lowercase(
            LamaPon::PathToUtf8(path));
        std::ranges::replace(
            normalized,
            '\\',
            '/');
        if (normalized == "builtin/circle"
            || normalized == "builtin/triangle"
            || normalized == "builtin/ring")
        {
            return true;
        }
        const auto extension = Lowercase(LamaPon::PathToUtf8(path.extension()));
        return extension == ".png"
            || extension == ".jpg"
            || extension == ".jpeg"
            || extension == ".bmp"
            || extension == ".tif"
            || extension == ".tiff"
            || extension == ".dds";
    }

    inline bool IsSceneAsset(const std::filesystem::path& path)
    {
        return Lowercase(LamaPon::PathToUtf8(path.filename())).ends_with(".scene.json");
    }

    inline bool IsPrefabAsset(const std::filesystem::path& path)
    {
        return Lowercase(LamaPon::PathToUtf8(path.filename())).ends_with(".prefab.json");
    }

    // データアセット（ScriptableObject相当）。判定はランタイム側の
    // LamaPon::IsDataAssetPathと同じ規則です。
    inline bool IsDataAsset(const std::filesystem::path& path)
    {
        return Lowercase(LamaPon::PathToUtf8(path.filename())).ends_with(
            ".asset.json");
    }

    inline bool IsMaterialAsset(const std::filesystem::path& path)
    {
        return Lowercase(LamaPon::PathToUtf8(path.filename())).ends_with(
            ".material.json");
    }

    inline bool IsShaderAsset(const std::filesystem::path& path)
    {
        return Lowercase(LamaPon::PathToUtf8(path.extension())) == ".hlsl";
    }

    // エンジンが「壊れている印」として使うShader（コンパイルに失敗した
    // ときの代役）。利用者が自分で割り当てられると、マゼンタが
    // 「壊れている」のか「そう描きたい」のか区別できなくなり、印の
    // 意味が無くなります。
    inline bool IsShaderErrorPlaceholder(
        const std::filesystem::path& path)
    {
        const auto filename =
            Lowercase(LamaPon::PathToUtf8(path.filename()));
        return filename == "lamaponshadererror.hlsl"
            || filename == "lamaponspriteerror.hlsl";
    }

    // マテリアル／スプライトへ割り当て可能なShaderかを判定します。
    // コンボ、ドラッグ＆ドロップ、「選択Shaderを設定」の各経路で
    // 同じ判定を使用します。
    inline bool IsAssignableShaderAsset(
        const std::filesystem::path& path)
    {
        return IsShaderAsset(path)
            && !IsShaderErrorPlaceholder(path);
    }

    inline bool IsAnimationAsset(const std::filesystem::path& path)
    {
        return Lowercase(LamaPon::PathToUtf8(path.filename())).ends_with(
            ".animation.json");
    }

    inline bool IsAnimatorControllerAsset(
        const std::filesystem::path& path)
    {
        return Lowercase(LamaPon::PathToUtf8(path.filename())).ends_with(
            ".animator.json");
    }

    inline bool IsModelAsset(const std::filesystem::path& path)
    {
        const auto extension = Lowercase(LamaPon::PathToUtf8(path.extension()));
        return extension == ".cmo"
            || extension == ".sdkmesh"
            || extension == ".vbo"
            || extension == ".gltf"
            || extension == ".glb"
            || extension == ".fbx";
    }

    // インポートスケール設定はFbxImporterだけが対応しています
    // （FBXはファイルごとに単位設定が食い違っていることが多く、
    // 実寸と大きく異なるサイズでインポートされる原因になるため）。
    inline bool IsFbxAsset(const std::filesystem::path& path)
    {
        return Lowercase(LamaPon::PathToUtf8(path.extension()))
            == ".fbx";
    }

    inline bool IsAudioAsset(const std::filesystem::path& path)
    {
        const auto extension =
            Lowercase(LamaPon::PathToUtf8(path.extension()));
        return extension == ".wav"
            || extension == ".ogg";
    }

    inline bool IsCppScriptAsset(const std::filesystem::path& path)
    {
        return Lowercase(LamaPon::PathToUtf8(path.extension())) == ".cpp";
    }

    inline ImTextureRef MakeTextureReference(ID3D11ShaderResourceView* texture)
    {
        return ImTextureRef{
            static_cast<ImTextureID>(
                reinterpret_cast<std::uintptr_t>(texture))
        };
    }

    inline bool IsPathWithin(
        const std::filesystem::path& root,
        const std::filesystem::path& candidate)
    {
        const auto normalizedRoot =
            std::filesystem::absolute(root).lexically_normal();
        const auto normalizedCandidate =
            std::filesystem::absolute(candidate).lexically_normal();

        auto rootPart = normalizedRoot.begin();
        auto candidatePart = normalizedCandidate.begin();
        for (; rootPart != normalizedRoot.end(); ++rootPart, ++candidatePart)
        {
            if (candidatePart == normalizedCandidate.end()
                || *rootPart != *candidatePart)
            {
                return false;
            }
        }
        return true;
    }
}
