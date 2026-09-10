#include "LamaPon/Editor/GameExporter.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Assets/AssetPacker.h"
#include "LamaPon/Assets/FbxImporter.h"
#include "LamaPon/Assets/GltfImporter.h"
#include "LamaPon/Core/Crypto.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/ProjectSettings.h"
#include "LamaPon/Editor/ExeIconTool.h"
#include "LamaPon/Editor/GameModuleBuilder.h"
#include "LamaPon/Graphics/LitMaterialAsset.h"
#include "LamaPon/Graphics/ShaderCompiler.h"
#include "LamaPon/Graphics/ShaderDiagnostics.h"
#include "LamaPon/Graphics/ShaderManifest.h"

#include <nlohmann/json.hpp>

#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{
    std::vector<std::uint8_t> ReadAllBytes(
        const std::filesystem::path& path)
    {
        std::ifstream input(
            path,
            std::ios::binary | std::ios::ate);
        if (!input)
        {
            throw std::runtime_error(
                "Could not open the exported file: "
                + LamaPon::PathToUtf8(path));
        }
        // tellg()はstd::fposなので、intと三項演算子で混ぜられません
        // （C2445）。負値の判定を先に済ませます。
        const auto end = input.tellg();
        if (end < 0)
        {
            throw std::runtime_error(
                "Could not determine the size of the exported"
                " file: "
                + LamaPon::PathToUtf8(path));
        }
        std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(end));
        input.seekg(0);
        if (!bytes.empty())
        {
            input.read(
                reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
            if (!input)
            {
                throw std::runtime_error(
                    "Could not read the exported file: "
                    + LamaPon::PathToUtf8(path));
            }
        }
        return bytes;
    }

    void WriteAllBytes(
        const std::filesystem::path& path,
        const std::vector<std::uint8_t>& bytes)
    {
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "Could not rewrite the exported file: "
                + LamaPon::PathToUtf8(path));
        }
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output)
        {
            throw std::runtime_error(
                "Could not finish writing the exported file: "
                + LamaPon::PathToUtf8(path));
        }
        output.close();
        if (!output)
        {
            throw std::runtime_error(
                "Could not close the exported file after writing: "
                + LamaPon::PathToUtf8(path));
        }
    }

    // 配布物ごとに生成した鍵を、書き出したLamaPonRuntime.dllの
    // 鍵スロットへ埋め込みます（配置はCrypto.hを参照）。
    // 共通鍵はソースから確認できるため、鍵を書き出しごとに分けることで、
    // 一つの鍵が別の配布物へ流用されるのを防ぎます。
    // 鍵スロットが見つからない場合は、既定鍵を含む配布物を生成しないよう
    // 書き出しを中止します。
    void EmbedArchiveKey(
        const std::filesystem::path& runtimeLibrary,
        const LamaPon::Crypto::AesKey& key)
    {
        // 鍵の差し替えはABIを変えません。モジュールの鮮度判定に使う
        // ビルド時刻を保ち、正常なGame Moduleを古いと誤判定させません。
        // 時刻の取得・復元に失敗した場合も、書き出し全体を中断します。
        const auto buildTime = std::filesystem::last_write_time(runtimeLibrary);
        auto bytes = ReadAllBytes(runtimeLibrary);
        const auto marker =
            LamaPon::Crypto::ExpectedKeySlotMarker();
        const auto begin = bytes.begin();
        const auto end = bytes.end();

        auto found = std::search(
            begin,
            end,
            marker.begin(),
            marker.end());
        if (found == end)
        {
            throw std::runtime_error(
                "LamaPonRuntime.dll has no archive key slot. "
                "The engine installation is older than this "
                "editor; update it and export again.");
        }
        // 2つ以上あるときは、どちらが本物か決められません
        // （偶然一致した並びを書き換えるとDLLを壊します）。
        if (std::search(
                found + 1,
                end,
                marker.begin(),
                marker.end())
            != end)
        {
            throw std::runtime_error(
                "LamaPonRuntime.dll has more than one archive "
                "key slot; refusing to patch it.");
        }

        const auto slot = LamaPon::Crypto::MakeKeySlot(key);
        if (static_cast<std::size_t>(std::distance(found, end))
            < slot.size())
        {
            throw std::runtime_error(
                "LamaPonRuntime.dll is truncated around its "
                "archive key slot.");
        }
        std::copy(slot.begin(), slot.end(), found);
        WriteAllBytes(runtimeLibrary, bytes);
        std::filesystem::last_write_time(runtimeLibrary, buildTime);
    }

    // 配布物へ同梱する（アーカイブへ入らない）ファイルを、その場で
    // 暗号化します。事前コンパイル済みシェーダーが対象です。
    // HLSLソースを外しても、DXBCがそのまま置いてあれば逆アセンブルで
    // 中身は読めてしまいます。
    std::size_t SealFilesInDirectory(
        const std::filesystem::path& directory,
        const LamaPon::Crypto::AesKey& key)
    {
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error))
        {
            return 0;
        }
        std::size_t sealed{};
        for (const auto& entry :
            std::filesystem::recursive_directory_iterator(
                directory))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            const auto bytes = ReadAllBytes(entry.path());
            if (LamaPon::Crypto::IsSealed(
                    bytes.data(),
                    bytes.size()))
            {
                continue;
            }
            const auto sealedBytes = LamaPon::Crypto::Seal(
                bytes.data(),
                bytes.size(),
                key);
            WriteAllBytes(entry.path(), sealedBytes);
            const auto written = ReadAllBytes(entry.path());
            if (!LamaPon::Crypto::IsSealed(
                    written.data(),
                    written.size())
                || !LamaPon::Crypto::Unseal(
                    written.data(),
                    written.size(),
                    key).has_value())
            {
                throw std::runtime_error(
                    "Could not verify the sealed shader cache file: "
                    + LamaPon::PathToUtf8(entry.path()));
            }
            ++sealed;
        }
        return sealed;
    }

    bool IsRelativePathSafe(const std::filesystem::path& path)
    {
        if (path.empty()
            || path.has_root_name()
            || path.has_root_directory()
            || path.is_absolute())
        {
            return false;
        }

        for (const auto& part : path)
        {
            if (part == L"..")
            {
                return false;
            }
        }
        return true;
    }

    bool IsPathWithin(
        const std::filesystem::path& root,
        const std::filesystem::path& candidate)
    {
        const auto relative = candidate.lexically_relative(root);
        return !relative.empty()
            && IsRelativePathSafe(relative);
    }

    std::filesystem::path MakeSiblingWorkingPath(
        const std::filesystem::path& output,
        const std::wstring_view label)
    {
        const auto suffix = std::to_wstring(
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count());
        return output.parent_path()
            / (output.filename().wstring()
                + L".lamapon-"
                + std::wstring(label)
                + L"-"
                + suffix);
    }

    bool RenameWithRetry(
        const std::filesystem::path& from,
        const std::filesystem::path& to,
        std::error_code& error)
    {
        constexpr int MaxAttempts = 6;
        for (int attempt = 0;
            attempt < MaxAttempts;
            ++attempt)
        {
            error.clear();
            std::filesystem::rename(from, to, error);
            if (!error)
            {
                return true;
            }

            // WebDAVはrenameを完了した後にERROR_NOT_SUPPORTEDを返す
            // 場合があります。移動先だけが存在するなら操作は完了済み
            // なので、重ねて失敗扱いにしません。
            const auto renameError = error;
            std::error_code sourceError;
            std::error_code destinationError;
            const bool sourceExists =
                std::filesystem::exists(from, sourceError);
            const bool destinationExists =
                std::filesystem::exists(to, destinationError);
            if (!sourceError
                && !destinationError
                && !sourceExists
                && destinationExists)
            {
                error.clear();
                return true;
            }
            error = renameError;

            // Windows DefenderやExplorerが直前に触ったフォルダーを
            // 一時的に保持することがあります。アクセス拒否だけは
            // 短く待って再試行し、それ以外のエラーはすぐ返します。
            if (error.value() != ERROR_ACCESS_DENIED
                && error != std::errc::permission_denied)
            {
                return false;
            }
            Sleep(25u * (1u << attempt));
        }
        return false;
    }

    std::runtime_error ExportError(
        const std::string_view message,
        const std::filesystem::path& path)
    {
        return std::runtime_error(
            std::string(message)
            + ": "
            + LamaPon::PathToUtf8(path));
    }

    // エクスポート先のゲームへ同梱するVC++ランタイム。エディター
    // （配布版エンジン）の隣に置かれたDLLをそのままコピーします。
    constexpr std::array<std::wstring_view, 5>
        RuntimeCrtLibraries{
            L"vcruntime140.dll",
            L"vcruntime140_1.dll",
            L"msvcp140.dll",
            L"msvcp140_1.dll",
            L"msvcp140_2.dll"
        };

    // JSONの中の"shaderKeywords"配列を、入れ子も含めて全部集めます。
    void CollectShaderKeywords(
        const nlohmann::json& node,
        std::vector<std::string>& keywords)
    {
        if (node.is_object())
        {
            for (const auto& [key, value] : node.items())
            {
                if (key == "shaderKeywords"
                    && value.is_array())
                {
                    for (const auto& keyword : value)
                    {
                        if (keyword.is_string())
                        {
                            keywords.push_back(
                                keyword.get<std::string>());
                        }
                    }
                    continue;
                }
                CollectShaderKeywords(value, keywords);
            }
            return;
        }
        if (node.is_array())
        {
            for (const auto& value : node)
            {
                CollectShaderKeywords(value, keywords);
            }
        }
    }

    enum DirectShaderRequirement : std::uint8_t
    {
        DirectShaderVertex = 1u << 0,
        DirectShaderPixel = 1u << 1,
        DirectShaderCompute = 1u << 2,
        DirectShaderSkinnedVertex = 1u << 3,
        DirectShaderSkinnedPixel = 1u << 4
    };

    enum ManifestShaderRequirement : std::uint8_t
    {
        ManifestShaderScreenEffect = 1u << 0,
        ManifestShaderMaterialForward = 1u << 1,
        ManifestShaderMaterialSkinned = 1u << 2,
        ManifestShaderCompute = 1u << 3
    };

    using DirectShaderRequirements =
        std::unordered_map<std::string, std::uint8_t>;
    using ManifestShaderRequirements =
        std::unordered_map<std::string, std::uint8_t>;
    struct ShaderConsumerRequirements final
    {
        std::uint8_t direct{};
        std::uint8_t manifest{};
    };
    using MaterialAssetRequirements =
        std::unordered_map<std::string, ShaderConsumerRequirements>;
    using AssetGuidPaths =
        std::unordered_map<std::string, std::filesystem::path>;

    struct ModelRendererShaderRequirements final
    {
        std::uint8_t direct =
            DirectShaderVertex | DirectShaderPixel;
        std::uint8_t manifest =
            ManifestShaderMaterialForward;
        std::string error;
    };
    using ModelRendererRequirementCache =
        std::unordered_map<
            std::string,
            ModelRendererShaderRequirements>;

    std::string ShaderReferenceKey(
        const std::filesystem::path& path)
    {
        auto key = LamaPon::PathToUtf8(path.lexically_normal());
        std::replace(key.begin(), key.end(), '\\', '/');
        std::transform(
            key.begin(),
            key.end(),
            key.begin(),
            [](const unsigned char value)
            {
                return static_cast<char>(std::tolower(value));
            });
        return key;
    }

    bool IsTemporaryAssetFile(
        const std::filesystem::path& path)
    {
        const auto name = path.filename().wstring();
        return name.find(L".lamapon-delete")
                != std::wstring::npos
            || name.ends_with(L".lamapon-remap.tmp")
            || name.ends_with(L".bak");
    }

    // AssetDatabase::Refreshは不足.metaや依存cacheを書き得るため、exportの
    // 検証では既存.metaだけを読み、GUID -> 現在pathの表をread-onlyで作ります。
    AssetGuidPaths ReadAssetGuidPaths(
        const std::filesystem::path& assetDirectory)
    {
        AssetGuidPaths paths;
        for (const auto& entry :
            std::filesystem::recursive_directory_iterator(assetDirectory))
        {
            if (!entry.is_regular_file()
                || !LamaPon::AssetDatabase::IsMetaFile(entry.path()))
            {
                continue;
            }
            try
            {
                std::ifstream input(entry.path(), std::ios::binary);
                if (!input)
                {
                    continue;
                }
                nlohmann::json metadata;
                input >> metadata;
                const auto guid = metadata.value(
                    "guid",
                    std::string{});
                if (metadata.value("format", std::string{})
                        != "LamaPonAssetMeta"
                    || metadata.value("version", 0) != 1
                    || !LamaPon::AssetDatabase::IsValidGuid(guid))
                {
                    continue;
                }
                auto assetPath = entry.path();
                assetPath.replace_extension();
                if (!std::filesystem::is_regular_file(assetPath)
                    || IsTemporaryAssetFile(assetPath))
                {
                    continue;
                }
                const auto relative =
                    assetPath.lexically_relative(assetDirectory);
                if (IsRelativePathSafe(relative))
                {
                    const auto [existing, inserted] =
                        paths.try_emplace(guid, relative);
                    if (!inserted)
                    {
                        // 重複GUIDを走査順でどちらかへ決めません。
                        // 後段のAssetDatabaseが明示的な重複診断を返します。
                        existing->second.clear();
                    }
                }
            }
            catch (const std::exception&)
            {
                // 壊れた.metaは通常のAssetDatabaseと同じく無視し、JSONに
                // 保存されたfallback pathを使います。
            }
        }
        std::erase_if(
            paths,
            [](const auto& entry)
            {
                return entry.second.empty();
            });
        return paths;
    }

    std::size_t RewriteJsonGuidReferences(
        nlohmann::json& node,
        const AssetGuidPaths& guidPaths)
    {
        if (node.is_array())
        {
            std::size_t changes{};
            for (auto& value : node)
            {
                changes += RewriteJsonGuidReferences(value, guidPaths);
            }
            return changes;
        }
        if (!node.is_object())
        {
            return 0;
        }

        std::vector<std::pair<std::string, std::string>> replacements;
        for (const auto& [key, value] : node.items())
        {
            constexpr std::string_view suffix = "Guid";
            if (key.size() <= suffix.size()
                || !key.ends_with(suffix)
                || !value.is_string())
            {
                continue;
            }
            const auto resolved = guidPaths.find(
                value.get<std::string>());
            if (resolved == guidPaths.end())
            {
                continue;
            }

            const auto field = key.substr(0, key.size() - suffix.size());
            const auto fallback = node.find(field);
            if (fallback != node.end() && !fallback->is_string())
            {
                // 型が壊れたJSONをexportだけで黙って修復しません。
                continue;
            }
            const auto currentPath =
                LamaPon::PathToUtf8(resolved->second);
            if (fallback == node.end()
                || fallback->get_ref<const std::string&>() != currentPath)
            {
                replacements.emplace_back(field, currentPath);
            }
        }

        std::size_t changes = replacements.size();
        for (const auto& [field, path] : replacements)
        {
            node[field] = path;
        }
        for (auto& value : node)
        {
            changes += RewriteJsonGuidReferences(value, guidPaths);
        }
        return changes;
    }

    // 配布ランタイムには.metaを同梱せず、AssetDatabaseのGUID表も
    // 作れません。そのためGUIDで解決した現在pathを、元ファイルには
    // 触れず、暗号化直前のJSONだけへ反映します。
    void RewritePackedJsonGuidReferences(
        const std::filesystem::path& relativePath,
        std::vector<std::uint8_t>& contents,
        const AssetGuidPaths& guidPaths)
    {
        const auto filename =
            ShaderReferenceKey(relativePath.filename());
        if (!filename.ends_with(".scene.json")
            && !filename.ends_with(".prefab.json")
            && !filename.ends_with(".material.json")
            && !filename.ends_with(".animator.json"))
        {
            return;
        }
        try
        {
            auto document = nlohmann::json::parse(
                contents.begin(),
                contents.end());
            if (RewriteJsonGuidReferences(document, guidPaths) == 0)
            {
                return;
            }
            const auto serialized = document.dump();
            contents.assign(serialized.begin(), serialized.end());
        }
        catch (const std::exception&)
        {
            // JSONの妥当性は各loaderの診断へ任せ、従来どおり元byteを
            // packします。変換処理だけで無関係なassetを拒否しません。
        }
    }

    std::filesystem::path ResolveJsonAssetReference(
        const nlohmann::json& node,
        const std::string_view field,
        const AssetGuidPaths& guidPaths,
        bool& malformed)
    {
        malformed = false;
        const std::string fieldName(field);
        std::filesystem::path fallback;
        const auto value = node.find(fieldName);
        if (value != node.end() && !value->is_null())
        {
            if (!value->is_string())
            {
                malformed = true;
                return {};
            }
            try
            {
                fallback = LamaPon::PathFromUtf8(
                    value->get<std::string>());
            }
            catch (const std::exception&)
            {
                malformed = true;
                return {};
            }
        }

        const auto guid = node.find(fieldName + "Guid");
        if (guid != node.end() && !guid->is_null())
        {
            if (!guid->is_string())
            {
                malformed = true;
                return {};
            }
            const auto found = guidPaths.find(
                guid->get<std::string>());
            if (found != guidPaths.end())
            {
                return found->second;
            }
        }
        return fallback;
    }

    ModelRendererShaderRequirements
        ResolveModelRendererShaderRequirements(
            const nlohmann::json& node,
            const AssetGuidPaths& guidPaths,
            LamaPon::AssetManager& assets,
            ModelRendererRequirementCache& cache,
            std::vector<std::string>& invalidReferences)
    {
        ModelRendererShaderRequirements result;
        bool malformed{};
        const auto modelPath = ResolveJsonAssetReference(
            node,
            "model",
            guidPaths,
            malformed);
        if (malformed)
        {
            invalidReferences.push_back(
                "<invalid model reference> (ModelRenderer)");
            return result;
        }
        // model未指定／load前はm_modelがnullなので、実行時と同じく
        // 通常のMaterialShader（Forward）として扱います。
        if (modelPath.empty())
        {
            return result;
        }
        if (!IsRelativePathSafe(modelPath))
        {
            invalidReferences.push_back(
                LamaPon::PathToUtf8(modelPath)
                + " (ModelRenderer model path must be "
                    "asset-root-relative without '..')");
            return result;
        }

        const auto key = ShaderReferenceKey(modelPath);
        if (const auto found = cache.find(key);
            found != cache.end())
        {
            if (!found->second.error.empty())
            {
                invalidReferences.push_back(found->second.error);
            }
            return found->second;
        }

        const auto extension =
            ShaderReferenceKey(modelPath.extension());
        const auto resolvedPath = assets.ResolvePath(modelPath);
        if (!assets.FileExists(resolvedPath))
        {
            result.error = LamaPon::PathToUtf8(modelPath)
                + " (referenced ModelRenderer model does not exist)";
        }
        else if (extension == ".gltf" || extension == ".glb")
        {
            // glTF/FBXはskin無しでもSkeletalModelとしてloadされるため、
            // 従来HLSLは常にSkinnedMaterialShaderです。一方Manifestは
            // primitiveごとにForward／Skinned roleを選びます。
            result.direct = DirectShaderSkinnedVertex
                | DirectShaderSkinnedPixel;
            try
            {
                bool requiresForwardRole{};
                const bool requiresSkinnedRole =
                    LamaPon::GltfImporter::RequiresSkinning(
                        assets,
                        resolvedPath,
                        &requiresForwardRole);
                result.manifest = 0;
                if (requiresForwardRole)
                {
                    result.manifest |=
                        ManifestShaderMaterialForward;
                }
                if (requiresSkinnedRole)
                {
                    result.manifest |=
                        ManifestShaderMaterialSkinned;
                }
            }
            catch (const std::exception& exception)
            {
                result.error = LamaPon::PathToUtf8(modelPath)
                    + " (could not inspect ModelRenderer glTF: "
                    + exception.what() + ")";
            }
        }
        else if (extension == ".fbx")
        {
            result.direct = DirectShaderSkinnedVertex
                | DirectShaderSkinnedPixel;
            try
            {
                bool requiresForwardRole{};
                const bool requiresSkinnedRole =
                    LamaPon::FbxImporter::RequiresSkinning(
                        assets,
                        resolvedPath,
                        &requiresForwardRole);
                result.manifest = 0;
                if (requiresForwardRole)
                {
                    result.manifest |=
                        ManifestShaderMaterialForward;
                }
                if (requiresSkinnedRole)
                {
                    result.manifest |=
                        ManifestShaderMaterialSkinned;
                }
            }
            catch (const std::exception& exception)
            {
                result.error = LamaPon::PathToUtf8(modelPath)
                    + " (could not inspect ModelRenderer FBX: "
                    + exception.what() + ")";
            }
        }
        else if (extension != ".cmo"
            && extension != ".sdkmesh"
            && extension != ".vbo")
        {
            result.error = LamaPon::PathToUtf8(modelPath)
                + " (unsupported ModelRenderer model format)";
        }

        const auto [stored, inserted] = cache.emplace(key, result);
        static_cast<void>(inserted);
        if (!stored->second.error.empty())
        {
            invalidReferences.push_back(stored->second.error);
        }
        return stored->second;
    }

    bool AddReferencedAssetRequirement(
        const nlohmann::json& node,
        const std::string_view field,
        const std::string_view expectedSuffix,
        const ShaderConsumerRequirements required,
        const AssetGuidPaths& guidPaths,
        MaterialAssetRequirements& requirements,
        std::vector<std::string>& invalidReferences)
    {
        bool malformed{};
        const auto path = ResolveJsonAssetReference(
            node,
            field,
            guidPaths,
            malformed);
        if (malformed)
        {
            invalidReferences.push_back(
                "<invalid " + std::string(field) + " reference>");
            return true;
        }
        if (path.empty())
        {
            return false;
        }
        if (!IsRelativePathSafe(path))
        {
            invalidReferences.push_back(LamaPon::PathToUtf8(path));
            return true;
        }
        const auto key = ShaderReferenceKey(path);
        if (key.ends_with(expectedSuffix))
        {
            auto& existing = requirements[key];
            existing.direct |= required.direct;
            existing.manifest |= required.manifest;
            return true;
        }
        invalidReferences.push_back(
            LamaPon::PathToUtf8(path)
            + " (expected " + std::string(expectedSuffix) + ")");
        return true;
    }

    void AddShaderPathRequirement(
        const std::filesystem::path& path,
        const std::uint8_t directRequired,
        const std::uint8_t manifestRequired,
        DirectShaderRequirements& directRequirements,
        ManifestShaderRequirements& manifestRequirements,
        std::vector<std::string>& invalidReferences)
    {
        if (path.empty())
        {
            return;
        }
        if (!IsRelativePathSafe(path))
        {
            invalidReferences.push_back(
                LamaPon::PathToUtf8(path)
                + " (path must be asset-root-relative without '..')");
            return;
        }

        const auto key = ShaderReferenceKey(path);
        if (key.ends_with(".hlsl"))
        {
            directRequirements[key] |= directRequired;
            return;
        }
        if (LamaPon::IsShaderManifestPath(path) && manifestRequired != 0)
        {
            manifestRequirements[key] |= manifestRequired;
            return;
        }
        invalidReferences.push_back(
            LamaPon::PathToUtf8(path)
            + (LamaPon::IsShaderManifestPath(path)
                ? " (this component does not support a shader manifest)"
                : " (expected .hlsl or .lamashader.json)"));
    }

    void AddReferencedShaderRequirement(
        const nlohmann::json& node,
        const std::uint8_t directRequired,
        const std::uint8_t manifestRequired,
        const AssetGuidPaths& guidPaths,
        DirectShaderRequirements& directRequirements,
        ManifestShaderRequirements& manifestRequirements,
        std::vector<std::string>& invalidReferences)
    {
        bool malformed{};
        const auto path = ResolveJsonAssetReference(
            node,
            "shader",
            guidPaths,
            malformed);
        if (malformed)
        {
            invalidReferences.push_back("<invalid shader reference>");
            return;
        }
        AddShaderPathRequirement(
            path,
            directRequired,
            manifestRequired,
            directRequirements,
            manifestRequirements,
            invalidReferences);
    }

    // Scene／Prefabのcomponentに保存された従来の.hlsl直接参照と、
    // そのcomponentが使うMaterial Assetを集めます。sourceを外す配布では、
    // 用途ごとの必須entryが一つでも焼けなければ実行時に直せないため、
    // export前に止めます。
    void CollectShaderRequirements(
        const nlohmann::json& node,
        const AssetGuidPaths& guidPaths,
        LamaPon::AssetManager& assets,
        ModelRendererRequirementCache& modelRequirementCache,
        DirectShaderRequirements& shaderRequirements,
        ManifestShaderRequirements& manifestRequirements,
        MaterialAssetRequirements& materialAssetRequirements,
        std::vector<std::string>& invalidReferences)
    {
        if (node.is_object())
        {
            std::uint8_t shaderRequired{};
            std::uint8_t manifestRequired{};
            ShaderConsumerRequirements materialRequired{};
            const auto type = node.find("type");
            const auto componentType =
                type != node.end() && type->is_string()
                    ? type->get<std::string>()
                    : std::string{};
            if (componentType == "MeshRenderer")
            {
                shaderRequired =
                    DirectShaderVertex | DirectShaderPixel;
                manifestRequired = ManifestShaderMaterialForward;
                materialRequired = {
                    shaderRequired,
                    manifestRequired
                };
            }
            else if (componentType == "ModelRenderer")
            {
                const auto required =
                    ResolveModelRendererShaderRequirements(
                        node,
                        guidPaths,
                        assets,
                        modelRequirementCache,
                        invalidReferences);
                shaderRequired = required.direct;
                manifestRequired = required.manifest;
                materialRequired = {
                    required.direct,
                    required.manifest
                };
            }
            else if (componentType == "ScreenEffect")
            {
                shaderRequired =
                    DirectShaderVertex | DirectShaderPixel;
                manifestRequired = ManifestShaderScreenEffect;
            }
            else if (componentType == "SpriteRenderer"
                || componentType == "ParticleSystem")
            {
                shaderRequired = DirectShaderPixel;
            }
            else if (componentType == "ComputeEffect")
            {
                shaderRequired = DirectShaderCompute;
                manifestRequired = ManifestShaderCompute;
            }

            bool usesMaterialAsset{};
            if (materialRequired.direct != 0)
            {
                usesMaterialAsset = AddReferencedAssetRequirement(
                    node,
                    "materialAsset",
                    ".material.json",
                    materialRequired,
                    guidPaths,
                    materialAssetRequirements,
                    invalidReferences);
            }
            // Material Assetが指定されていればOnInitializeでcomponent内の
            // LitMaterial全体（保存済みshaderを含む）を上書きします。
            // 使われない古いcomponent shaderを必須扱いしません。
            if (shaderRequired != 0 && !usesMaterialAsset)
            {
                AddReferencedShaderRequirement(
                    node,
                    shaderRequired,
                    manifestRequired,
                    guidPaths,
                    shaderRequirements,
                    manifestRequirements,
                    invalidReferences);
            }

            for (const auto& value : node)
            {
                CollectShaderRequirements(
                    value,
                    guidPaths,
                    assets,
                    modelRequirementCache,
                    shaderRequirements,
                    manifestRequirements,
                    materialAssetRequirements,
                    invalidReferences);
            }
            return;
        }
        if (node.is_array())
        {
            for (const auto& value : node)
            {
                CollectShaderRequirements(
                    value,
                    guidPaths,
                    assets,
                    modelRequirementCache,
                    shaderRequirements,
                    manifestRequirements,
                    materialAssetRequirements,
                    invalidReferences);
            }
        }
    }

    void RunSystemTar(
        const std::filesystem::path& folder,
        const std::filesystem::path& zipPath)
    {
        std::error_code removeError;
        std::filesystem::remove(zipPath, removeError);

        wchar_t systemDirectory[MAX_PATH]{};
        if (GetSystemDirectoryW(
                systemDirectory,
                MAX_PATH) == 0)
        {
            throw std::runtime_error(
                "Could not locate the Windows system directory.");
        }
        const auto tarPath =
            std::filesystem::path(systemDirectory)
            / L"tar.exe";
        if (!std::filesystem::is_regular_file(tarPath))
        {
            throw ExportError(
                "tar.exe was not found (required for zip export, bundled with Windows 10 and later)",
                tarPath);
        }

        // -a: 拡張子からzip形式を推定 / -C: 親フォルダーへ移動して
        // フォルダー名だけをアーカイブへ入れます。
        std::wstring commandLine =
            L"\"" + tarPath.wstring() + L"\" -a -c -f \""
            + zipPath.wstring() + L"\" -C \""
            + folder.parent_path().wstring() + L"\" \""
            + folder.filename().wstring() + L"\"";

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (CreateProcessW(
                tarPath.c_str(),
                commandLine.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &startup,
                &process) == FALSE)
        {
            throw ExportError(
                "Could not start tar.exe",
                tarPath);
        }
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        if (exitCode != 0)
        {
            std::filesystem::remove(zipPath, removeError);
            throw ExportError(
                "Zip archive creation failed",
                zipPath);
        }

        std::error_code sizeError;
        const auto zipSize =
            std::filesystem::file_size(zipPath, sizeError);
        // 22 bytesは、entryが1件もないZIPのEnd of Central Directory
        // だけを書いたサイズです。Windows tarはWebDAVの-Cを列挙できず、
        // exit code 0の空ZIPを返すことがあるため、終了コードだけでなく
        // 成果物も検査します。
        if (sizeError || zipSize <= 22)
        {
            std::filesystem::remove(zipPath, removeError);
            throw ExportError(
                "Zip archive was empty after creation",
                zipPath);
        }
    }

    void PublishZipArchive(
        const std::filesystem::path& sourceZip,
        const std::filesystem::path& destinationZip)
    {
        const auto stagingZip = MakeSiblingWorkingPath(
            destinationZip,
            L"staging");
        const auto backupZip = MakeSiblingWorkingPath(
            destinationZip,
            L"backup");

        std::error_code copyError;
        std::filesystem::copy_file(
            sourceZip,
            stagingZip,
            std::filesystem::copy_options::none,
            copyError);

        std::error_code sourceSizeError;
        std::error_code stagingSizeError;
        const auto sourceSize = std::filesystem::file_size(
            sourceZip,
            sourceSizeError);
        const auto stagingSize = std::filesystem::file_size(
            stagingZip,
            stagingSizeError);
        // WebDAVはcopy完了後に失敗コードを返すこともあるため、
        // 最終サイズが一致していれば成功として扱います。
        if (sourceSizeError
            || stagingSizeError
            || sourceSize != stagingSize)
        {
            std::error_code cleanupError;
            std::filesystem::remove(stagingZip, cleanupError);
            if (copyError)
            {
                throw std::filesystem::filesystem_error(
                    "Could not copy the completed zip archive",
                    sourceZip,
                    stagingZip,
                    copyError);
            }
            throw ExportError(
                "Copied zip archive did not match its source",
                stagingZip);
        }

        const bool hadPreviousZip =
            std::filesystem::exists(destinationZip);
        if (hadPreviousZip)
        {
            std::error_code renameError;
            if (!RenameWithRetry(
                    destinationZip,
                    backupZip,
                    renameError))
            {
                std::error_code cleanupError;
                std::filesystem::remove(stagingZip, cleanupError);
                throw std::filesystem::filesystem_error(
                    "Could not move the previous zip archive",
                    destinationZip,
                    backupZip,
                    renameError);
            }
        }

        std::error_code renameError;
        if (!RenameWithRetry(
                stagingZip,
                destinationZip,
                renameError))
        {
            std::error_code cleanupError;
            std::filesystem::remove(stagingZip, cleanupError);
            if (hadPreviousZip
                && !std::filesystem::exists(destinationZip))
            {
                RenameWithRetry(
                    backupZip,
                    destinationZip,
                    cleanupError);
            }
            throw std::filesystem::filesystem_error(
                "Could not publish the zip archive",
                stagingZip,
                destinationZip,
                renameError);
        }

        if (hadPreviousZip)
        {
            std::error_code cleanupError;
            std::filesystem::remove(backupZip, cleanupError);
        }
    }

    // Windows標準のtar.exe（bsdtar）でフォルダーを.zipへ固めます。
    // WebDAV上ではtarの-Cがexit 0の空ZIPを作るため、入力を一時的に
    // ローカルへ複製し、完成ZIPだけをtransactionalに配布先へ移します。
    void CreateZipWithSystemTar(
        const std::filesystem::path& folder,
        const std::filesystem::path& zipPath)
    {
        const auto temporaryRoot =
            std::filesystem::temp_directory_path()
            / (L"LamaPonExportZip-"
                + std::to_wstring(
                    std::chrono::steady_clock::now()
                        .time_since_epoch()
                        .count()));
        std::error_code directoryError;
        if (!LamaPon::EnsureDirectoryExists(
                temporaryRoot,
                directoryError))
        {
            throw std::filesystem::filesystem_error(
                "Could not create the local zip workspace",
                temporaryRoot,
                directoryError);
        }

        const bool useLocalInput =
            LamaPon::ShouldUseLocalGameModuleBuildCache(folder);
        const auto archiveFolder = useLocalInput
            ? temporaryRoot / folder.filename()
            : folder;
        const auto localZip = temporaryRoot
            / (folder.filename().wstring() + L".zip");
        try
        {
            if (useLocalInput)
            {
                std::error_code copyError;
                std::filesystem::copy(
                    folder,
                    archiveFolder,
                    std::filesystem::copy_options::recursive,
                    copyError);
                if (copyError)
                {
                    throw std::filesystem::filesystem_error(
                        "Could not copy the export into the local zip workspace",
                        folder,
                        archiveFolder,
                        copyError);
                }
            }

            RunSystemTar(archiveFolder, localZip);
            PublishZipArchive(localZip, zipPath);
        }
        catch (...)
        {
            std::error_code cleanupError;
            std::filesystem::remove_all(
                temporaryRoot,
                cleanupError);
            throw;
        }

        std::error_code cleanupError;
        std::filesystem::remove_all(
            temporaryRoot,
            cleanupError);
    }
}

namespace LamaPon
{
    std::wstring SanitizeGameFileName(
        const std::string& gameName)
    {
        constexpr std::wstring_view invalidCharacters =
            LR"(\/:*?"<>|)";
        std::wstring result;
        for (const wchar_t character : Utf8ToWide(gameName))
        {
            const bool invalid = character < 0x20
                || invalidCharacters.find(character)
                    != std::wstring_view::npos;
            result.push_back(invalid ? L'_' : character);
        }

        // 先頭・末尾の空白とドットはWindowsのファイル名で
        // 使えないため取り除きます。
        const auto first = result.find_first_not_of(L" .");
        const auto last = result.find_last_not_of(L" .");
        result = first == std::wstring::npos
            ? std::wstring{}
            : result.substr(first, last - first + 1);
        if (result.empty())
        {
            return L"LamaPonGame";
        }

        // CONやNULなどの予約デバイス名はそのまま使えないため
        // 先頭へ「_」を付けます。
        std::wstring upper = result;
        std::transform(
            upper.begin(),
            upper.end(),
            upper.begin(),
            [](const wchar_t value)
            {
                return static_cast<wchar_t>(
                    std::towupper(value));
            });
        constexpr std::array<std::wstring_view, 22>
            reservedNames{
                L"CON", L"PRN", L"AUX", L"NUL",
                L"COM1", L"COM2", L"COM3", L"COM4",
                L"COM5", L"COM6", L"COM7", L"COM8",
                L"COM9",
                L"LPT1", L"LPT2", L"LPT3", L"LPT4",
                L"LPT5", L"LPT6", L"LPT7", L"LPT8",
                L"LPT9"
            };
        if (std::ranges::find(reservedNames, upper)
            != reservedNames.end())
        {
            result.insert(result.begin(), L'_');
        }
        return result;
    }

    GameExportResult ExportGamePackage(
        const GameExportOptions& options)
    {
        const auto runtimeDirectory = std::filesystem::weakly_canonical(
            options.runtimeDirectory);
        const auto assetDirectory = std::filesystem::weakly_canonical(
            options.assetDirectory);
        const auto outputDirectory = std::filesystem::absolute(
            options.outputDirectory).lexically_normal();

        if (!std::filesystem::is_directory(runtimeDirectory))
        {
            throw ExportError(
                "Runtime directory was not found",
                runtimeDirectory);
        }
        if (!std::filesystem::is_directory(assetDirectory))
        {
            throw ExportError(
                "Asset directory was not found",
                assetDirectory);
        }
        if (outputDirectory.filename().empty())
        {
            throw ExportError(
                "Export directory requires a folder name",
                outputDirectory);
        }
        if (std::filesystem::exists(outputDirectory)
            && !std::filesystem::is_directory(outputDirectory))
        {
            throw ExportError(
                "Export destination is not a directory",
                outputDirectory);
        }
        if (outputDirectory == assetDirectory
            || IsPathWithin(assetDirectory, outputDirectory)
            || IsPathWithin(outputDirectory, assetDirectory))
        {
            throw ExportError(
                "Export directory cannot contain or be inside the asset directory",
                outputDirectory);
        }
        if (outputDirectory == runtimeDirectory
            || IsPathWithin(outputDirectory, runtimeDirectory))
        {
            throw ExportError(
                "Export directory cannot contain the runtime directory",
                outputDirectory);
        }
        ValidateProjectSettings(options.projectSettings);

        const auto gameExecutable =
            runtimeDirectory / L"LamaPonGame.exe";
        const auto runtimeLibrary =
            runtimeDirectory / L"LamaPonRuntime.dll";
        const auto audioRuntime =
            runtimeDirectory / L"xaudio2_9redist.dll";
        const auto gameModule = options.gameModulePath.empty()
            ? runtimeDirectory / L"LamaPonGameModule.dll"
            : std::filesystem::absolute(
                options.gameModulePath).lexically_normal();
        const auto startupScene =
            assetDirectory
            / options.projectSettings.startupScene;
        if (!std::filesystem::is_regular_file(gameExecutable))
        {
            throw ExportError(
                "LamaPonGame.exe was not found",
                gameExecutable);
        }
        if (!std::filesystem::is_regular_file(runtimeLibrary))
        {
            throw ExportError(
                "LamaPonRuntime.dll was not found",
                runtimeLibrary);
        }
        if (!std::filesystem::is_regular_file(audioRuntime))
        {
            throw ExportError(
                "xaudio2_9redist.dll was not found",
                audioRuntime);
        }
        if (!std::filesystem::is_regular_file(startupScene))
        {
            throw ExportError(
                "Startup scene was not found",
                startupScene);
        }

        // C++ Scriptを含むProjectで古い／無いGame Moduleをそのまま
        // 梱包すると、Sceneだけは読めるのにScriptが一つも動かず、空や
        // 背景色だけのゲームになります。配布先で初めて壊れるのではなく、
        // 書き出し時点で理由と直し方を返します。
        const auto moduleState = InspectGameModuleBuildState(
            assetDirectory.parent_path(),
            gameModule);
        if (moduleState.hasSources && !moduleState.outputExists)
        {
            throw ExportError(
                "C++ sources exist, but the Game Module is missing. Build the Game Module before exporting",
                gameModule);
        }
        if (moduleState.buildRequired)
        {
            throw ExportError(
                "The Game Module is older than a project C++ source. Build the Game Module before exporting",
                gameModule);
        }
        if (moduleState.outputExists)
        {
            std::error_code moduleTimeError;
            std::error_code runtimeTimeError;
            const auto moduleTime = std::filesystem::last_write_time(
                gameModule,
                moduleTimeError);
            const auto runtimeTime = std::filesystem::last_write_time(
                runtimeLibrary,
                runtimeTimeError);
            if (moduleTimeError || runtimeTimeError)
            {
                throw ExportError(
                    "Could not verify Game Module compatibility timestamps",
                    gameModule);
            }
            if (moduleTime < runtimeTime)
            {
                throw ExportError(
                    "The Game Module is older than LamaPonRuntime.dll and would be rejected at startup. Rebuild the Game Module before exporting",
                    gameModule);
            }
        }
        const auto gameIconSource =
            options.projectSettings.gameIcon.empty()
                ? std::filesystem::path{}
                : assetDirectory
                    / options.projectSettings.gameIcon;
        if (!gameIconSource.empty()
            && !std::filesystem::is_regular_file(
                gameIconSource))
        {
            throw ExportError(
                "Game icon was not found",
                gameIconSource);
        }

        const auto outputParent = outputDirectory.parent_path();
        std::error_code outputParentError;
        if (!LamaPon::EnsureDirectoryExists(
                outputParent,
                outputParentError))
        {
            throw std::filesystem::filesystem_error(
                "Could not create the export parent directory",
                outputParent,
                outputParentError);
        }
        const auto stagingDirectory = MakeSiblingWorkingPath(
            outputDirectory,
            L"staging");
        const auto backupDirectory = MakeSiblingWorkingPath(
            outputDirectory,
            L"backup");

        // 実行ファイルはゲーム名を反映した名前で出力します。
        const std::wstring exportedExecutableName =
            SanitizeGameFileName(
                options.projectSettings.gameName)
            + L".exe";

        try
        {
            std::error_code stagingError;
            if (!LamaPon::EnsureDirectoryExists(
                    stagingDirectory,
                    stagingError))
            {
                throw std::filesystem::filesystem_error(
                    "Could not create the export staging directory",
                    stagingDirectory,
                    stagingError);
            }
            std::filesystem::copy_file(
                gameExecutable,
                stagingDirectory / exportedExecutableName);
            std::filesystem::copy_file(
                runtimeLibrary,
                stagingDirectory / runtimeLibrary.filename());
            std::filesystem::copy_file(
                audioRuntime,
                stagingDirectory / audioRuntime.filename());
            // 実行コードに伴う通知を配布先にも残します。欠けたSDKから
            // 不完全な配布物を作らないよう、コピー失敗時は中断します。
            const auto licenses = runtimeDirectory / "licenses";
            if (!std::filesystem::is_directory(licenses)
                || std::filesystem::is_empty(licenses))
            {
                throw std::runtime_error("Runtime license directory is missing.");
            }
            std::filesystem::copy(
                licenses, stagingDirectory / "licenses",
                std::filesystem::copy_options::recursive);
            std::filesystem::copy_file(
                runtimeDirectory / "THIRD_PARTY_NOTICES.md",
                stagingDirectory / "THIRD_PARTY_NOTICES.md");

            // このゲームだけのアーカイブ鍵を作り、書き出した
            // LamaPonRuntime.dllへ焼き込みます。先に済ませるのは、
            // 失敗したときにシェーダーの事前コンパイル（数秒）を
            // 無駄にしないためです。
            const auto archiveKey = LamaPon::Crypto::RandomKey();
            EmbedArchiveKey(
                stagingDirectory / runtimeLibrary.filename(),
                archiveKey);

            // ゲームアイコンを実行ファイルへ埋め込みます
            // （ExplorerのファイルアイコンとウィンドウのLamaPon標準
            // アイコンが差し替わります）。
            if (!gameIconSource.empty())
            {
                ReplaceExecutableIcon(
                    stagingDirectory / exportedExecutableName,
                    BuildIcoFromImageFile(gameIconSource));
            }

            // VC++ランタイムを同梱し、再頒布可能パッケージ未導入の
            // PCでもそのまま起動できるようにします（配布版エンジンの
            // 隣にあるDLLをコピー。無ければスキップ）。
            for (const auto crtLibrary : RuntimeCrtLibraries)
            {
                const auto crtSource =
                    runtimeDirectory / crtLibrary;
                if (std::filesystem::is_regular_file(
                        crtSource))
                {
                    std::filesystem::copy_file(
                        crtSource,
                        stagingDirectory
                            / crtSource.filename());
                }
            }
            if (std::filesystem::is_regular_file(
                    gameModule))
            {
                std::filesystem::copy_file(
                    gameModule,
                    stagingDirectory
                        / gameModule.filename());

                // Game Moduleが参照するプロジェクト固有のDLLを実行ファイルの
                // 隣へコピーし、Windowsローダーが同じ場所から解決できる
                // ようにします。
                for (const auto& entry :
                    std::filesystem::directory_iterator(
                        gameModule.parent_path()))
                {
                    if (!entry.is_regular_file())
                    {
                        continue;
                    }
                    auto extension = entry.path().extension().wstring();
                    std::transform(
                        extension.begin(),
                        extension.end(),
                        extension.begin(),
                        [](const wchar_t value)
                        {
                            return static_cast<wchar_t>(
                                std::towlower(value));
                        });
                    if (extension != L".dll")
                    {
                        continue;
                    }

                    const auto destination =
                        stagingDirectory / entry.path().filename();
                    if (!std::filesystem::exists(destination))
                    {
                        std::filesystem::copy_file(
                            entry.path(),
                            destination);
                    }
                }
            }
            // HLSLソースを外す設定なら、アーカイブから除きます。
            // .hlsliも同じ（#include専用なので単体では使えませんが、
            // 中身は読めてしまうため）。
            const std::vector<std::wstring> skippedExtensions =
                options.projectSettings.stripShaderSourceOnExport
                    ? std::vector<std::wstring>{
                        L".hlsl",
                        L".hlsli" }
                    : std::vector<std::wstring>{};
            const auto assetGuidPaths =
                ReadAssetGuidPaths(assetDirectory);
            static_cast<void>(
                PackAssets(
                    assetDirectory,
                    stagingDirectory / L"assets.tpak",
                    archiveKey,
                    skippedExtensions,
                    [&assetGuidPaths](
                        const std::filesystem::path& relativePath,
                        std::vector<std::uint8_t>& contents)
                    {
                        RewritePackedJsonGuidReferences(
                            relativePath,
                            contents,
                            assetGuidPaths);
                    }));

            // シェーダーを事前にコンパイルして同梱し、プレイヤーの
            // 初回起動時に発生するコンパイル待ちを避けます。
            //
            // 入口は総当たりです。VSOutlineのような「あれば使う」枠は
            // 持っていないシェーダーのほうが多く、その失敗も覚えないと
            // 実行時に毎回試し直されてしまいます。
            // AssetManagerはWIC／D2Dのファクトリーを作るのでCOMが要ります。
            // エディターからの書き出しでは既に初期化済みですが、
            // 書き出しをテストや別プロセスから呼ぶこともあるので、
            // ここで面倒を見ます（既に初期化済みなら何もしません）。
            const HRESULT comResult = CoInitializeEx(
                nullptr,
                COINIT_APARTMENTTHREADED);
            const bool comInitialized = SUCCEEDED(comResult);
            const auto cacheDirectory =
                stagingDirectory / L"shader-cache";
            try
            {
                AssetManager exportAssets{ nullptr, nullptr };
                // export検証はプロジェクトを変更しません。既存.metaだけを
                // 読み、無いassetの一時GUIDはこのAssetDatabase内に限定します。
                exportAssets.SetAssetRoot(
                    assetDirectory,
                    false);

                const auto reportShaderPrecompileFailure =
                    [&](std::string message)
                    {
                        if (options.projectSettings
                                .stripShaderSourceOnExport)
                        {
                            throw std::runtime_error(
                                "A source-stripped export cannot "
                                "include an unusable shader. "
                                + message);
                        }
                        Logger::Instance().Warning(message);
                    };

                // shader_featureのストリップ用に、プロジェクトの
                // どこかで実際に立てられているキーワードを集めます。
                // シーンやPrefabのJSONへ"shaderKeywords"として
                // 保存されているものが対象です。
                //
                // シェーダーごとに紐付けず、プロジェクト全体の和を
                // 取っているのは安全側だからです。別のシェーダーの
                // キーワードが紛れても「余分に焼く」だけで済み、
                // 必要なものを落とすことはありません。
                std::vector<std::string> usedKeywords;
                DirectShaderRequirements directShaderRequirements;
                ManifestShaderRequirements manifestShaderRequirements;
                MaterialAssetRequirements materialAssetRequirements;
                ModelRendererRequirementCache modelRequirementCache;
                std::vector<std::string> invalidShaderReferences;
                for (const auto& entry :
                    std::filesystem::recursive_directory_iterator(
                        assetDirectory))
                {
                    if (!entry.is_regular_file()
                        || IsTemporaryAssetFile(entry.path())
                        || ShaderReferenceKey(entry.path().extension())
                            != ".json")
                    {
                        continue;
                    }
                    try
                    {
                        std::ifstream input(
                            entry.path(),
                            std::ios::binary);
                        if (!input)
                        {
                            continue;
                        }
                        nlohmann::json document;
                        input >> document;
                        CollectShaderKeywords(
                            document,
                            usedKeywords);
                        const auto invalidBegin =
                            invalidShaderReferences.size();
                        CollectShaderRequirements(
                            document,
                            assetGuidPaths,
                            exportAssets,
                            modelRequirementCache,
                            directShaderRequirements,
                            manifestShaderRequirements,
                            materialAssetRequirements,
                            invalidShaderReferences);
                        for (auto index = invalidBegin;
                            index < invalidShaderReferences.size();
                            ++index)
                        {
                            invalidShaderReferences[index] =
                                PathToUtf8(entry.path()) + " -> "
                                + invalidShaderReferences[index];
                        }
                    }
                    catch (const std::exception&)
                    {
                        // 読めないJSONは飛ばします。ここで失敗しても
                        // 「絞れない＝全部焼く」になるだけです。
                    }
                }
                std::sort(
                    usedKeywords.begin(),
                    usedKeywords.end());
                usedKeywords.erase(
                    std::unique(
                        usedKeywords.begin(),
                        usedKeywords.end()),
                    usedKeywords.end());

                // componentのMaterial Asset参照をshader参照まで辿ります。
                // 実行時と同じくGUIDをfallback pathより優先するため、assetを
                // 移動してscene/materialが未保存でも正しいHLSLを検証できます。
                for (const auto& [materialKey, required] :
                    materialAssetRequirements)
                {
                    const auto materialRelative =
                        PathFromUtf8(materialKey);
                    const auto materialPath =
                        exportAssets.ResolvePath(materialRelative);
                    std::string materialError;
                    try
                    {
                        const auto material = LoadLitMaterialAsset(
                            materialPath,
                            &exportAssets.Database(),
                            &exportAssets);
                        const auto invalidBegin =
                            invalidShaderReferences.size();
                        AddShaderPathRequirement(
                            material.Shader(),
                            required.direct,
                            required.manifest,
                            directShaderRequirements,
                            manifestShaderRequirements,
                            invalidShaderReferences);
                        for (auto index = invalidBegin;
                            index < invalidShaderReferences.size();
                            ++index)
                        {
                            invalidShaderReferences[index] =
                                PathToUtf8(materialRelative) + " -> "
                                + invalidShaderReferences[index];
                        }
                    }
                    catch (const std::exception& exception)
                    {
                        materialError = exception.what();
                    }
                    if (!materialError.empty())
                    {
                        reportShaderPrecompileFailure(
                            "A referenced material asset is unusable: "
                            + PathToUtf8(materialRelative) + ": "
                            + materialError);
                    }
                }

                for (const auto& reference :
                    invalidShaderReferences)
                {
                    reportShaderPrecompileFailure(
                        "A persisted shader, material, or model "
                        "reference is "
                        "invalid for export: "
                        + reference);
                }

                std::uint32_t shaderFiles{};
                for (const auto& entry :
                    std::filesystem::recursive_directory_iterator(
                        assetDirectory))
                {
                    if (!entry.is_regular_file())
                    {
                        continue;
                    }
                    if (IsTemporaryAssetFile(entry.path()))
                    {
                        continue;
                    }
                    auto extension =
                        entry.path().extension().wstring();
                    std::transform(
                        extension.begin(),
                        extension.end(),
                        extension.begin(),
                        [](const wchar_t value)
                        {
                            return static_cast<wchar_t>(
                                std::towlower(value));
                        });
                    // .hlsliは#include専用なので単体では通りません。
                    if (extension != L".hlsl")
                    {
                        continue;
                    }
                    // ソースを外すときは全バリアントを焼きます。
                    // ストリップと同時にやると、取りこぼした
                    // 組み合わせを実行時に作り直せず（ソースが
                    // 無いので）標準Litへ落ちてしまいます。
                    static_cast<void>(PrecompileShader(
                        exportAssets,
                        entry.path(),
                        cacheDirectory,
                        {},
                        options.projectSettings
                                .stripShaderSourceOnExport
                            ? nullptr
                            : &usedKeywords));

                    // 総当たりには「入口が無い」という正常な失敗も混ざる
                    // ため、その戻り値だけでは必須entryの成否が分かりません。
                    // 永続化された用途と、ソースに実際に宣言された従来entryを
                    // 個別に再検証し、sourceを外した後のlookup missを防ぎます。
                    std::vector<ShaderEntryPoint> requiredEntries;
                    const auto addRequiredEntry =
                        [&](const char* const entryPoint,
                            const char* const target)
                        {
                            const auto duplicate = std::find_if(
                                requiredEntries.begin(),
                                requiredEntries.end(),
                                [&](const ShaderEntryPoint& candidate)
                                {
                                    return std::string_view{
                                        candidate.entryPoint }
                                            == entryPoint
                                        && std::string_view{
                                            candidate.target }
                                            == target;
                                });
                            if (duplicate == requiredEntries.end())
                            {
                                requiredEntries.push_back(
                                    ShaderEntryPoint{ entryPoint, target });
                            }
                        };

                    const auto relativePath =
                        entry.path().lexically_relative(assetDirectory);
                    const auto referenced =
                        directShaderRequirements.find(
                            ShaderReferenceKey(relativePath));
                    std::uint8_t referencedRequirements{};
                    if (referenced != directShaderRequirements.end())
                    {
                        referencedRequirements = referenced->second;
                        directShaderRequirements.erase(referenced);
                        if ((referencedRequirements
                                & DirectShaderVertex) != 0)
                        {
                            addRequiredEntry("VSMain", "vs_5_0");
                        }
                        if ((referencedRequirements
                                & DirectShaderPixel) != 0)
                        {
                            addRequiredEntry("PSMain", "ps_5_0");
                        }
                        if ((referencedRequirements
                                & DirectShaderCompute) != 0)
                        {
                            addRequiredEntry("CSMain", "cs_5_0");
                        }
                        if ((referencedRequirements
                                & DirectShaderSkinnedVertex) != 0)
                        {
                            addRequiredEntry(
                                "VSSkinnedMain",
                                "vs_5_0");
                        }
                        if ((referencedRequirements
                                & DirectShaderSkinnedPixel) != 0)
                        {
                            addRequiredEntry(
                                "PSSkinnedMain",
                                "ps_5_0");
                        }
                    }

                    try
                    {
                        const auto source =
                            exportAssets.ReadFileBytes(entry.path());
                        const auto declared = ParseShaderEntryPoints(
                            std::string_view{
                                reinterpret_cast<const char*>(
                                    source.data()),
                                source.size() });
                        if (declared.vertex)
                        {
                            addRequiredEntry("VSMain", "vs_5_0");
                        }
                        if (declared.pixel)
                        {
                            addRequiredEntry("PSMain", "ps_5_0");
                        }
                        if (declared.skinnedVertex)
                        {
                            addRequiredEntry(
                                "VSSkinnedMain",
                                "vs_5_0");
                        }
                        if (declared.skinnedPixel)
                        {
                            addRequiredEntry(
                                "PSSkinnedMain",
                                "ps_5_0");
                        }
                        if (declared.geometry)
                        {
                            addRequiredEntry("GSMain", "gs_5_0");
                        }
                        if (declared.hull)
                        {
                            addRequiredEntry("HSMain", "hs_5_0");
                        }
                        if (declared.domain)
                        {
                            addRequiredEntry("DSMain", "ds_5_0");
                        }
                        if (declared.compute)
                        {
                            addRequiredEntry("CSMain", "cs_5_0");
                        }
                    }
                    catch (const std::exception& exception)
                    {
                        if (!requiredEntries.empty())
                        {
                            reportShaderPrecompileFailure(
                                "A referenced direct HLSL could not be "
                                "read for export: "
                                + PathToUtf8(relativePath) + ": "
                                + exception.what());
                        }
                    }

                    for (const auto& required : requiredEntries)
                    {
                        std::string compileError;
                        const auto compiled = PrecompileShaderVariants(
                            exportAssets,
                            entry.path(),
                            cacheDirectory,
                            std::span<const ShaderEntryPoint>{
                                &required,
                                1 },
                            {},
                            options.projectSettings
                                    .stripShaderSourceOnExport
                                ? nullptr
                                : &usedKeywords,
                            &compileError);
                        if (compiled == 0 || !compileError.empty())
                        {
                            reportShaderPrecompileFailure(
                                "Required direct-HLSL entry was not "
                                "precompiled for export: "
                                + PathToUtf8(relativePath)
                                + " (entry '" + required.entryPoint
                                + "', target '" + required.target + "')"
                                + (compileError.empty()
                                    ? "."
                                    : ": " + compileError));
                        }
                    }
                    ++shaderFiles;
                }

                for (const auto& [missingShader, required] :
                    directShaderRequirements)
                {
                    static_cast<void>(required);
                    reportShaderPrecompileFailure(
                        "A referenced direct HLSL file does not exist: "
                        + missingShader);
                }

                // Manifestの入口名はKnownShaderEntryPointsには含まれない
                // ため、宣言されたsource/pass/stageを追加で焼きます。
                // Materialは全pass、ScreenEffectは実行対象の先頭pass、
                // Computeは最初の必須compute passが対象です。
                // Manifest自体はJSONなので、HLSLを除外した配布物にも
                // 残り、実行時は同じsource/entry/targetで索引を引けます。
                std::uint32_t screenEffectManifests{};
                std::uint32_t materialManifests{};
                std::uint32_t computeManifests{};
                for (const auto& entry :
                    std::filesystem::recursive_directory_iterator(
                        assetDirectory))
                {
                    if (!entry.is_regular_file()
                        || IsTemporaryAssetFile(entry.path())
                        || !IsShaderManifestPath(entry.path()))
                    {
                        continue;
                    }

                    const auto manifestRelative =
                        entry.path().lexically_relative(assetDirectory);
                    const auto referenced =
                        manifestShaderRequirements.find(
                            ShaderReferenceKey(manifestRelative));
                    std::uint8_t manifestRequirements{};
                    if (referenced != manifestShaderRequirements.end())
                    {
                        manifestRequirements = referenced->second;
                        manifestShaderRequirements.erase(referenced);
                    }

                    ShaderAssetDesc manifest;
                    std::string manifestError;
                    if (!LoadShaderAssetDesc(
                            exportAssets,
                            entry.path(),
                            manifest,
                            manifestError))
                    {
                        // 壊れたManifestを黙って無視すると、配布物で初めて
                        // 原因が分かります。source同梱時はファイル名と検証
                        // 理由を警告し、source-strip時は実行時fallbackが
                        // 無いため書き出しを止めます。
                        reportShaderPrecompileFailure(
                            "Shader manifest was not precompiled for "
                            "export: " + manifestError);
                        continue;
                    }

                    const auto hasRole =
                        [&manifest](const ShaderPassRole role)
                        {
                            return std::ranges::any_of(
                                manifest.passes,
                                [role](const ShaderPassDesc& pass)
                                {
                                    return pass.role == role;
                                });
                        };
                    std::vector<std::string> compatibilityErrors;
                    if ((manifestRequirements
                            & ManifestShaderScreenEffect) != 0
                        && manifest.type
                            != ShaderAssetType::ScreenEffect)
                    {
                        compatibilityErrors.emplace_back(
                            "referenced by ScreenEffect but does not "
                            "declare type 'screenEffect'");
                    }
                    if ((manifestRequirements
                            & ManifestShaderCompute) != 0
                        && manifest.type != ShaderAssetType::Compute)
                    {
                        compatibilityErrors.emplace_back(
                            "referenced by ComputeEffect but does not "
                            "declare type 'compute'");
                    }
                    const auto materialRequirements =
                        static_cast<std::uint8_t>(
                            manifestRequirements
                            & (ManifestShaderMaterialForward
                                | ManifestShaderMaterialSkinned));
                    if (materialRequirements != 0
                        && manifest.type != ShaderAssetType::Material)
                    {
                        compatibilityErrors.emplace_back(
                            "referenced by a material renderer but does "
                            "not declare type 'material'");
                    }
                    else if (manifest.type == ShaderAssetType::Material)
                    {
                        if ((materialRequirements
                                & ManifestShaderMaterialForward) != 0
                            && !hasRole(ShaderPassRole::Forward))
                        {
                            compatibilityErrors.emplace_back(
                                "referenced by a Forward material "
                                "renderer but has no "
                                "Forward pass");
                        }
                        if ((materialRequirements
                                & ManifestShaderMaterialSkinned) != 0
                            && !hasRole(ShaderPassRole::Skinned))
                        {
                            compatibilityErrors.emplace_back(
                                "referenced by ModelRenderer but has no "
                                "Skinned pass");
                        }
                    }
                    if (!compatibilityErrors.empty())
                    {
                        std::string diagnostic =
                            "Shader manifest is incompatible with its "
                            "consumer: " + PathToUtf8(manifestRelative)
                            + " (";
                        for (std::size_t index = 0;
                            index < compatibilityErrors.size();
                            ++index)
                        {
                            if (index != 0)
                            {
                                diagnostic += "; ";
                            }
                            diagnostic += compatibilityErrors[index];
                        }
                        diagnostic += ").";
                        reportShaderPrecompileFailure(
                            std::move(diagnostic));
                    }

                    const auto sourcePath =
                        exportAssets.ResolvePath(manifest.source);
                    if (!exportAssets.FileExists(sourcePath))
                    {
                        reportShaderPrecompileFailure(
                            "Shader manifest was not "
                            "precompiled because its source file does "
                            "not exist: "
                            + PathToUtf8(entry.path())
                            + " -> " + PathToUtf8(manifest.source));
                        continue;
                    }

                    std::vector<const ShaderPassDesc*> passesToCompile;
                    if (manifest.type == ShaderAssetType::Material)
                    {
                        passesToCompile.reserve(manifest.passes.size());
                        for (const auto& pass : manifest.passes)
                        {
                            passesToCompile.push_back(&pass);
                        }
                    }
                    else if (manifest.type
                        == ShaderAssetType::ScreenEffect)
                    {
                        passesToCompile.push_back(
                            &manifest.passes.front());
                    }
                    else
                    {
                        for (const auto& candidate : manifest.passes)
                        {
                            const auto* compute = FindShaderStage(
                                candidate,
                                ShaderStage::Compute);
                            if (compute != nullptr && !compute->optional)
                            {
                                passesToCompile.push_back(&candidate);
                                break;
                            }
                        }
                        // LoadShaderAssetDescで検証済みですが、将来の
                        // schema変更でも空参照にならないよう守ります。
                        if (passesToCompile.empty())
                        {
                            reportShaderPrecompileFailure(
                                "Compute shader manifest was not "
                                "precompiled because it has no required "
                                "compute stage: "
                                + PathToUtf8(entry.path()));
                            continue;
                        }
                    }

                    bool requiredStagesCompiled = true;
                    for (const auto* const pass : passesToCompile)
                    {
                        for (const auto& stage : pass->stages)
                        {
                            const ShaderEntryPoint manifestEntry{
                                stage.entryPoint.c_str(),
                                stage.target.c_str()
                            };
                            std::string stageCompileError;
                            const auto compiledVariants =
                                manifest.type
                                    == ShaderAssetType::Material
                                ? PrecompileShaderVariants(
                                    exportAssets,
                                    sourcePath,
                                    cacheDirectory,
                                    std::span<const ShaderEntryPoint>{
                                        &manifestEntry,
                                        1 },
                                    {},
                                    options.projectSettings
                                            .stripShaderSourceOnExport
                                        ? nullptr
                                        : &usedKeywords,
                                    &stageCompileError)
                                : PrecompileShader(
                                    exportAssets,
                                    sourcePath,
                                    cacheDirectory,
                                    std::span<const ShaderEntryPoint>{
                                        &manifestEntry,
                                        1 },
                                    {},
                                    &stageCompileError);
                            if (!stage.optional
                                && (compiledVariants == 0
                                    || !stageCompileError.empty()))
                            {
                                requiredStagesCompiled = false;
                                std::string diagnostic =
                                    "Required shader stage was not "
                                    "precompiled for "
                                    + std::string{
                                        manifest.type
                                                == ShaderAssetType::Material
                                            ? "material"
                                            : manifest.type
                                                    == ShaderAssetType::Compute
                                                ? "compute"
                                            : "screen-effect" }
                                    + " manifest: "
                                    + PathToUtf8(entry.path())
                                    + " -> " + PathToUtf8(manifest.source)
                                    + " (pass '" + pass->name
                                    + "', entry '" + stage.entryPoint
                                    + "', target '" + stage.target + "')"
                                    + (stageCompileError.empty()
                                        ? "."
                                        : ": " + stageCompileError);
                                reportShaderPrecompileFailure(
                                    std::move(diagnostic));
                            }
                        }
                    }
                    if (requiredStagesCompiled)
                    {
                        if (manifest.type
                            == ShaderAssetType::Material)
                        {
                            ++materialManifests;
                        }
                        else if (manifest.type
                            == ShaderAssetType::Compute)
                        {
                            ++computeManifests;
                        }
                        else
                        {
                            ++screenEffectManifests;
                        }
                    }
                }
                for (const auto& [missingManifest, required] :
                    manifestShaderRequirements)
                {
                    static_cast<void>(required);
                    reportShaderPrecompileFailure(
                        "A referenced shader manifest does not exist: "
                        + missingManifest);
                }
                // ソースが無いときの引き先になる索引。
                WriteShaderCacheIndex(cacheDirectory);
                // 事前コンパイル済みのDXBCと索引を暗号化します。
                // アーカイブの外に置くファイルなので、ここで
                // 個別に包みます（実行時は中身を見て復号します）。
                const auto sealedShaderFiles =
                    SealFilesInDirectory(
                        cacheDirectory,
                        archiveKey);
                Logger::Instance().Info(
                    "Sealed precompiled shader cache: "
                    + std::to_string(sealedShaderFiles)
                    + " file(s).");
                Logger::Instance().Info(
                    "Precompiled shaders for export: "
                    + std::to_string(shaderFiles)
                    + " file(s), "
                    + std::to_string(screenEffectManifests)
                    + " screen-effect manifest(s), "
                    + std::to_string(materialManifests)
                    + " material manifest(s), "
                    + std::to_string(computeManifests)
                    + " compute manifest(s)."
                    + (options.projectSettings
                            .stripShaderSourceOnExport
                        ? " HLSL sources were excluded from"
                          " the package."
                        : ""));
            }
            catch (const std::exception& exception)
            {
                // Write前に抜けた場合の保留索引も、この出力先だけ
                // 排出して破棄します。Seal途中なら暗号化済み／平文が
                // 混ざり得るため、どちらの場合もcache全体を残しません。
                try
                {
                    WriteShaderCacheIndex(cacheDirectory);
                }
                catch (...)
                {
                    // 直後にdirectoryごと破棄するので、索引書込み失敗は
                    // 元の診断を置き換えません。
                }
                std::error_code cleanupError;
                std::filesystem::remove_all(
                    cacheDirectory,
                    cleanupError);

                const std::string message =
                    (options.projectSettings
                            .stripShaderSourceOnExport
                        ? std::string(
                            "Shaders could not be prepared for a "
                            "source-stripped export: ")
                        : std::string(
                            "Shaders could not be precompiled for "
                            "export; the first launch will compile "
                            "them instead: "))
                    + exception.what();
                Logger::Instance().Warning(message);
                if (options.projectSettings
                        .stripShaderSourceOnExport
                    || cleanupError)
                {
                    if (comInitialized)
                    {
                        CoUninitialize();
                    }
                    if (cleanupError)
                    {
                        throw std::filesystem::filesystem_error(
                            "Could not remove an incomplete shader "
                            "cache after export preparation failed",
                            cacheDirectory,
                            cleanupError);
                    }
                    throw std::runtime_error(message);
                }
            }
            if (comInitialized)
            {
                CoUninitialize();
            }

            const auto settingsPath =
                stagingDirectory / L"LamaPonGame.json";
            SaveProjectSettings(
                settingsPath,
                options.projectSettings,
                ProjectSettingsFileType::GamePackage);
        }
        catch (...)
        {
            std::error_code cleanupError;
            std::filesystem::remove_all(
                stagingDirectory,
                cleanupError);
            throw;
        }

        const bool hadPreviousExport =
            std::filesystem::exists(outputDirectory);
        if (hadPreviousExport)
        {
            std::error_code renameError;
            if (!RenameWithRetry(
                    outputDirectory,
                    backupDirectory,
                    renameError))
            {
                std::error_code cleanupError;
                std::filesystem::remove_all(
                    stagingDirectory,
                    cleanupError);
                throw std::filesystem::filesystem_error(
                    "Could not move the previous game export",
                    outputDirectory,
                    backupDirectory,
                    renameError);
            }
        }

        std::error_code renameError;
        if (!RenameWithRetry(
                stagingDirectory,
                outputDirectory,
                renameError))
        {
            std::error_code cleanupError;
            std::filesystem::remove_all(
                stagingDirectory,
                cleanupError);
            if (hadPreviousExport
                && !std::filesystem::exists(outputDirectory))
            {
                RenameWithRetry(
                    backupDirectory,
                    outputDirectory,
                    cleanupError);
            }
            throw std::filesystem::filesystem_error(
                "Could not publish the game export",
                stagingDirectory,
                outputDirectory,
                renameError);
        }

        if (hadPreviousExport)
        {
            std::error_code cleanupError;
            std::filesystem::remove_all(
                backupDirectory,
                cleanupError);
        }

        GameExportResult result;
        result.outputDirectory = outputDirectory;
        result.executablePath =
            outputDirectory / exportedExecutableName;
        for (const auto& entry :
            std::filesystem::recursive_directory_iterator(
                outputDirectory))
        {
            if (entry.is_regular_file())
            {
                ++result.fileCount;
                result.totalBytes += entry.file_size();
            }
        }

        // 配布用ZIPは完成した出力フォルダーの隣へ作成します。
        if (options.createZipArchive)
        {
            const auto zipPath =
                outputDirectory.parent_path()
                / (outputDirectory.filename().wstring()
                    + L".zip");
            CreateZipWithSystemTar(outputDirectory, zipPath);
            result.zipPath = zipPath;
        }
        return result;
    }
}
