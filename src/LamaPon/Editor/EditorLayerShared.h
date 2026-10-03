#pragma once

#include "LamaPon/Core/PathUtils.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// EditorLayerの複数の翻訳単位で使うヘルパーを共有します。
namespace LamaPon::EditorDetail
{
    // ヒエラルキーの既定幅・px
    constexpr float HierarchyWidth = 290.0f;

    // オブジェクトIDのpayload型名
    constexpr const char* GameObjectPayload = "LAMAPON_GAME_OBJECT";

    // 並べ替え用Component*のpayload型名
    constexpr const char* ComponentPayload = "LAMAPON_COMPONENT";

    // 資産パスのpayload型名
    constexpr const char* AssetPayload = "LAMAPON_ASSET_PATH";

    // フォルダードラッグのpayload型名
    constexpr const char* AssetFolderPayload =
        "LAMAPON_ASSET_FOLDER";


    // 表示名から保存名を提案し、非ASCIIならNewPrefabにします(displayName: 元の表示名)。
    inline std::wstring SuggestedPrefabFileStem(const std::string_view displayName)
    {
        // 調整中のPrefabファイル名
        std::wstring stem = PathFromUtf8(displayName).filename().wstring();
        // 検証するファイル名文字
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

    // 文字列を小文字へ変換します(value: 変換する文字列の所有先)。
    inline std::string Lowercase(std::string value)
    {
        // 各文字を小文字へ揃えます(character: 変換する文字)。
        std::ranges::transform(
            value,
            value.begin(),
            [](const unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
        return value;
    }

    // 差分項目の表示名を返し、未登録の項目は元の名前を使います(field: 差分の項目名)。
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

    // 差分パスをオブジェクト・成分の表示名へ整形します(path: 差分のスラッシュ区切りパス)。
    inline std::string FormatPrefabOverridePath(
        const std::string_view path)
    {
        // 差分パスを区切った借用項目
        std::vector<std::string_view> tokens;
        // 次に区切るパスの開始位置
        std::size_t start = path.starts_with('/')
            ? 1
            : 0;
        while (start < path.size())
        {
            // 次のパス区切り位置
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

        // 表示用に整形した差分パス
        std::string result;
        // 差分パスの項目添字
        for (std::size_t index = 0;
            index < tokens.size();
            ++index)
        {
            // 差分パスの表示項目
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

    // 字句正規化した資産パスを小文字化して比較キーを作ります(path: 比較する資産パス)。
    inline std::string NormalizeAssetReference(
        const std::filesystem::path& path)
    {
        return Lowercase(
            LamaPon::PathToUtf8(path.lexically_normal()));
    }

    // 両方が非空の資産パスを正規化して比較します(left: 左の資産パス, right: 右の資産パス)。
    inline bool IsSameAssetReference(
        const std::filesystem::path& left,
        const std::filesystem::path& right)
    {
        return !left.empty()
            && !right.empty()
            && NormalizeAssetReference(left)
                == NormalizeAssetReference(right);
    }


    // 空同士は変更無しとし、他は正規化したパスで変更を判定します(current: 現在選択している資産, candidate: 新たに選択する資産)。
    inline bool IsAssetSelectionChange(
        const std::filesystem::path& current,
        const std::filesystem::path& candidate)
    {
        if (current.empty() || candidate.empty())
        {
            return current.empty() != candidate.empty();
        }
        return !IsSameAssetReference(current, candidate);
    }

    // 内蔵図形か対応する画像拡張子か判定します(path: 調べる資産パス)。
    inline bool IsTextureAsset(const std::filesystem::path& path)
    {
        // 小文字と区切りを揃えた資産名
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
        // 小文字化したファイル拡張子
        const auto extension = Lowercase(LamaPon::PathToUtf8(path.extension()));
        return extension == ".png"
            || extension == ".jpg"
            || extension == ".jpeg"
            || extension == ".bmp"
            || extension == ".tif"
            || extension == ".tiff"
            || extension == ".dds";
    }

    // ファイル名が.scene.jsonで終わるか判定します(path: 調べる資産パス)。
    inline bool IsSceneAsset(const std::filesystem::path& path)
    {
        return Lowercase(LamaPon::PathToUtf8(path.filename())).ends_with(".scene.json");
    }

    // ファイル名が.prefab.jsonで終わるか判定します(path: 調べる資産パス)。
    inline bool IsPrefabAsset(const std::filesystem::path& path)
    {
        return Lowercase(LamaPon::PathToUtf8(path.filename())).ends_with(".prefab.json");
    }


    // ランタイムと同じ.asset.jsonの命名規則で判定します(path: 調べる資産パス)。
    inline bool IsDataAsset(const std::filesystem::path& path)
    {
        return Lowercase(LamaPon::PathToUtf8(path.filename())).ends_with(
            ".asset.json");
    }

    // ファイル名が.material.jsonで終わるか判定します(path: 調べる資産パス)。
    inline bool IsMaterialAsset(const std::filesystem::path& path)
    {
        return Lowercase(LamaPon::PathToUtf8(path.filename())).ends_with(
            ".material.json");
    }

    // ファイル名が.lamashader.jsonで終わるか判定します(path: 調べる資産パス)。
    inline bool IsShaderManifestAsset(
        const std::filesystem::path& path)
    {
        return Lowercase(LamaPon::PathToUtf8(path.filename())).ends_with(
            ".lamashader.json");
    }


    // 直接割当用のHLSLだけを判定し、manifestは用途別に選択します(path: 調べる資産パス)。
    inline bool IsShaderAsset(const std::filesystem::path& path)
    {
        return Lowercase(LamaPon::PathToUtf8(path.extension())) == ".hlsl";
    }


    // コード編集用にHLSLとmanifestの両方を許可します(path: 調べる資産パス)。
    inline bool IsOpenableShaderAsset(
        const std::filesystem::path& path)
    {
        return IsShaderAsset(path)
            || IsShaderManifestAsset(path);
    }


    // 割当を許さないエンジンのエラー表示用シェーダーか判定します(path: 調べる資産パス)。
    inline bool IsShaderErrorPlaceholder(
        const std::filesystem::path& path)
    {
        // 判定用の小文字ファイル名
        const auto filename =
            Lowercase(LamaPon::PathToUtf8(path.filename()));
        return filename == "lamaponshadererror.hlsl"
            || filename == "lamaponspriteerror.hlsl";
    }


    // エラー表示用を除いた直接HLSLの割当可否を判定します(path: 調べる資産パス)。
    inline bool IsAssignableShaderAsset(
        const std::filesystem::path& path)
    {
        return IsShaderAsset(path)
            && !IsShaderErrorPlaceholder(path);
    }

    // ファイル名が.animation.jsonで終わるか判定します(path: 調べる資産パス)。
    inline bool IsAnimationAsset(const std::filesystem::path& path)
    {
        return Lowercase(LamaPon::PathToUtf8(path.filename())).ends_with(
            ".animation.json");
    }

    // ファイル名が.animator.jsonで終わるか判定します(path: 調べる資産パス)。
    inline bool IsAnimatorControllerAsset(
        const std::filesystem::path& path)
    {
        return Lowercase(LamaPon::PathToUtf8(path.filename())).ends_with(
            ".animator.json");
    }

    // 対応するモデル拡張子か判定します(path: 調べる資産パス)。
    inline bool IsModelAsset(const std::filesystem::path& path)
    {
        // 小文字化したファイル拡張子
        const auto extension = Lowercase(LamaPon::PathToUtf8(path.extension()));
        return extension == ".cmo"
            || extension == ".sdkmesh"
            || extension == ".vbo"
            || extension == ".gltf"
            || extension == ".glb"
            || extension == ".fbx";
    }


    // インポート単位設定に対応するFBXか判定します(path: 調べる資産パス)。
    inline bool IsFbxAsset(const std::filesystem::path& path)
    {
        return Lowercase(LamaPon::PathToUtf8(path.extension()))
            == ".fbx";
    }

    // 音声の対応拡張子wavかoggか判定します(path: 調べる資産パス)。
    inline bool IsAudioAsset(const std::filesystem::path& path)
    {
        // 小文字化したファイル拡張子
        const auto extension =
            Lowercase(LamaPon::PathToUtf8(path.extension()));
        return extension == ".wav"
            || extension == ".ogg";
    }

    // 拡張子がcppか判定します(path: 調べる資産パス)。
    inline bool IsCppScriptAsset(const std::filesystem::path& path)
    {
        return Lowercase(LamaPon::PathToUtf8(path.extension())) == ".cpp";
    }

    // 正規化した絶対パスの要素を比較し、基準自身または配下か判定します(root: 基準パス, candidate: 比較する候補パス)。
    inline bool IsPathWithin(
        const std::filesystem::path& root,
        const std::filesystem::path& candidate)
    {
        // 基準の正規化済み絶対パス
        const auto normalizedRoot =
            std::filesystem::absolute(root).lexically_normal();
        // 候補の正規化済み絶対パス
        const auto normalizedCandidate =
            std::filesystem::absolute(candidate).lexically_normal();

        // 比較中の基準パス要素の位置
        auto rootPart = normalizedRoot.begin();
        // 比較中の候補パス要素の位置
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
