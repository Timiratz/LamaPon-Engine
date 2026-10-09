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
#include "LamaPon/Editor/PackageNativeDependencies.h"
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
    // ファイル全体を読み、サイズ取得や読込の失敗は例外にします(path: 読込対象のパス)。
    std::vector<std::uint8_t> ReadAllBytes(
        const std::filesystem::path& path)
    {
        // ファイル全体の入力ストリーム
        std::ifstream input(
            path,
            std::ios::binary | std::ios::ate);
        if (!input)
        {
            throw std::runtime_error(
                "Could not open the exported file: "
                + LamaPon::PathToUtf8(path));
        }

        // 読込サイズまたはDLL内容の終端
        const auto end = input.tellg();
        if (end < 0)
        {
            throw std::runtime_error(
                "Could not determine the size of the exported"
                " file: "
                + LamaPon::PathToUtf8(path));
        }
        // ファイル全体の読込先
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

    // バイト列を書き込み、flushとcloseの成功を確認します(path: 書込対象のパス, bytes: 書き込む内容)。
    void WriteAllBytes(
        const std::filesystem::path& path,
        const std::vector<std::uint8_t>& bytes)
    {
        // ファイル全体の出力ストリーム
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


    // 唯一の鍵スロットを更新し、Runtimeの更新時刻を復元します(runtimeLibrary: 出力したRuntimeのDLL, key: この配布物用の暗号鍵)。
    void EmbedArchiveKey(
        const std::filesystem::path& runtimeLibrary,
        const LamaPon::Crypto::AesKey& key)
    {
        // Game Moduleの鮮度判定を変えないため、鍵の埋込前のRuntime更新時刻を復元します。
        // 鍵埋込前のRuntime更新時刻
        const auto buildTime = std::filesystem::last_write_time(runtimeLibrary);
        // 読込済みのファイル全体
        auto bytes = ReadAllBytes(runtimeLibrary);
        // 鍵スロットの検出用バイト列
        const auto marker =
            LamaPon::Crypto::ExpectedKeySlotMarker();
        // Runtimeバイト列の先頭
        const auto begin = bytes.begin();
        // 読込サイズまたはDLL内容の終端
        const auto end = bytes.end();

        // 検出した鍵スロットの位置
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
        // 誤った領域を書き換えないよう、鍵スロットが複数あるRuntimeは拒否します。
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

        // 新しい鍵を含むスロット内容
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


    // 未暗号化ファイルを暗号化して読み直し、復号可能か検証します(directory: 暗号化する基準パス, key: この配布物用の暗号鍵)。
    std::size_t SealFilesInDirectory(
        const std::filesystem::path& directory,
        const LamaPon::Crypto::AesKey& key)
    {
        // 検証またはファイル操作の失敗
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error))
        {
            return 0;
        }
        // 暗号化して検証済みのファイル数
        std::size_t sealed{};
        // 走査するファイルまたはJSON要素
        for (const auto& entry :
            std::filesystem::recursive_directory_iterator(
                directory))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            // 読込済みのファイル全体
            const auto bytes = ReadAllBytes(entry.path());
            if (LamaPon::Crypto::IsSealed(
                    bytes.data(),
                    bytes.size()))
            {
                continue;
            }
            // 同梱ファイルの暗号化内容
            const auto sealedBytes = LamaPon::Crypto::Seal(
                bytes.data(),
                bytes.size(),
                key);
            WriteAllBytes(entry.path(), sealedBytes);
            // 暗号化後に読み直した内容
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

    // 絶対指定と親への遡りを含まない相対パスか判定します(path: 検証するパス)。
    bool IsRelativePathSafe(const std::filesystem::path& path)
    {
        if (path.empty()
            || path.has_root_name()
            || path.has_root_directory()
            || path.is_absolute())
        {
            return false;
        }

        // 相対パスの構成要素
        for (const auto& part : path)
        {
            if (part == L"..")
            {
                return false;
            }
        }
        return true;
    }

    // 字句的に基準内の相対パスとなるか判定します(root: 基準パス, candidate: 判定するパス)。
    bool IsPathWithin(
        const std::filesystem::path& root,
        const std::filesystem::path& candidate)
    {
        // 基準パスからの相対位置
        const auto relative = candidate.lexically_relative(root);
        return !relative.empty()
            && IsRelativePathSafe(relative);
    }

    // 出力先の隣に時刻由来の作業パスを作ります(output: 完成物の出力先, label: 作業パスの用途名)。
    std::filesystem::path MakeSiblingWorkingPath(
        const std::filesystem::path& output,
        const std::wstring_view label)
    {
        // 作業パス名の時刻識別子
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

    // アクセス拒否を再試行して移動し、エラーでも完了済みなら成功にします(from: 移動元パス, to: 移動先パス, error: 最終の失敗状態の出力先)。
    bool RenameWithRetry(
        const std::filesystem::path& from,
        const std::filesystem::path& to,
        std::error_code& error)
    {
        // renameの最大試行回数
        constexpr int MaxAttempts = 6;
        // renameの試行回数
        for (int attempt = 0;
            // renameの最大試行回数
            attempt < MaxAttempts;
            ++attempt)
        {
            error.clear();
            std::filesystem::rename(from, to, error);
            if (!error)
            {
                return true;
            }

            // WebDAVで移動完了後にエラーが返る場合は、元が無く移動先だけ存在することを確認します。
            // renameが返した失敗状態
            const auto renameError = error;
            // 移動元存在確認の失敗状態
            std::error_code sourceError;
            // 移動先存在確認の失敗状態
            std::error_code destinationError;
            // 移動元が存在するか
            const bool sourceExists =
                std::filesystem::exists(from, sourceError);
            // 移動先が存在するか
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

            // アクセス拒否は短く待って再試行し、他のエラーは直ちに返します。
            if (error.value() != ERROR_ACCESS_DENIED
                && error != std::errc::permission_denied)
            {
                return false;
            }
            Sleep(25u * (1u << attempt));
        }
        return false;
    }

    // 対象パスを含む書き出しエラーを作ります(message: 失敗内容, path: 失敗した対象パス)。
    std::runtime_error ExportError(
        const std::string_view message,
        const std::filesystem::path& path)
    {
        return std::runtime_error(
            std::string(message)
            + ": "
            + LamaPon::PathToUtf8(path));
    }


    // 同梱するVC++ RuntimeのDLL名
    constexpr std::array<std::wstring_view, 5>
        RuntimeCrtLibraries{
            L"vcruntime140.dll",
            L"vcruntime140_1.dll",
            L"msvcp140.dll",
            L"msvcp140_1.dll",
            L"msvcp140_2.dll"
        };


    // 入れ子のJSONからshaderKeywordsの文字列を集めます(node: 調べるJSON値, keywords: キーワードの追加先)。
    void CollectShaderKeywords(
        const nlohmann::json& node,
        std::vector<std::string>& keywords)
    {
        if (node.is_object())
        {
            // JSON項目名と検査する値
            for (const auto& [key, value] : node.items())
            {
                if (key == "shaderKeywords"
                    && value.is_array())
                {
                    // JSONで指定されたキーワード
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
            // 再帰検査するJSON値
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
        // 直接参照の必要シェーダー種別
        std::uint8_t direct{};
        // manifestの必要シェーダー種別
        std::uint8_t manifest{};
    };
    using MaterialAssetRequirements =
        std::unordered_map<std::string, ShaderConsumerRequirements>;
    using AssetGuidPaths =
        std::unordered_map<std::string, std::filesystem::path>;

    struct ModelRendererShaderRequirements final
    {
        // モデルの必要な直接参照種別
        std::uint8_t direct =
            DirectShaderVertex | DirectShaderPixel;
        // モデルの必要なmanifest種別
        std::uint8_t manifest =
            ManifestShaderMaterialForward;
        // 検証またはファイル操作の失敗
        std::string error;
    };
    using ModelRendererRequirementCache =
        std::unordered_map<
            std::string,
            ModelRendererShaderRequirements>;

    // 字句正規化と区切り・大小文字の統一で参照キーを作ります(path: キーへ変換するパス)。
    std::string ShaderReferenceKey(
        const std::filesystem::path& path)
    {
        // 正規化した参照パスかJSONキー
        auto key = LamaPon::PathToUtf8(path.lexically_normal());
        std::replace(key.begin(), key.end(), '\\', '/');
        // 参照パスを小文字へ揃えます(value: 変換する文字)。
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

    // 資産の削除・再対応付け用ファイルとバックアップ名か判定します(path: 検査する資産パス)。
    bool IsTemporaryAssetFile(
        const std::filesystem::path& path)
    {
        // 一時資産名または比較用ファイル名
        const auto name = path.filename().wstring();
        return name.find(L".lamapon-delete")
                != std::wstring::npos
            || name.ends_with(L".lamapon-remap.tmp")
            || name.ends_with(L".bak");
    }

    // リンクと開発・資格情報ファイルを拒否し、配布ファイル一覧を返します(stagingDirectory: 配布内容の検証先)。
    std::string InspectExportFiles(
        const std::filesystem::path& stagingDirectory)
    {
        // 検証済みの配布ファイル相対パス
        std::vector<std::filesystem::path> files;
        // 走査するファイルまたはJSON要素
        for (const auto& entry :
            std::filesystem::recursive_directory_iterator(
                stagingDirectory))
        {
            // 基準パスからの相対位置
            const auto relative = entry.path().lexically_relative(
                stagingDirectory);
            if (entry.is_symlink())
            {
                throw std::runtime_error(
                    "Export contains a symbolic link: "
                    + LamaPon::PathToUtf8(relative));
            }
            if (!entry.is_regular_file())
            {
                continue;
            }
            // 一時資産名または比較用ファイル名
            const auto name = ShaderReferenceKey(
                entry.path().filename());
            // 小文字化したファイル拡張子
            const auto extension = ShaderReferenceKey(
                entry.path().extension());
            if (extension == ".c" || extension == ".cc"
                || extension == ".cpp" || extension == ".cxx"
                || extension == ".h" || extension == ".hpp"
                || extension == ".hxx" || extension == ".pdb"
                || extension == ".pem" || extension == ".pfx"
                || extension == ".p12" || extension == ".p8"
                || extension == ".key"
                || name == ".env" || name.starts_with(".env.")
                || name == "credentials.json"
                || name == "secrets.json")
            {
                throw std::runtime_error(
                    "Export contains a development or credential file: "
                    + LamaPon::PathToUtf8(relative));
            }
            files.push_back(relative);
        }
        std::sort(files.begin(), files.end());
        // 配布ファイルの一覧通知文
        std::string inventory = "Exported loose-file inventory ("
            + std::to_string(files.size()) + " files):";
        // 一覧へ追加する相対パス
        for (const auto& path : files)
        {
            inventory += "\n  + " + LamaPon::PathToUtf8(path);
        }
        return inventory;
    }


    // 元資産を変更せず既存metaから一意なGUIDの現在パスを集めます(assetDirectory: 読込対象のassets基準パス)。
    AssetGuidPaths ReadAssetGuidPaths(
        const std::filesystem::path& assetDirectory)
    {
        // 一意なGUIDと現在パスの対応表
        AssetGuidPaths paths;
        // 走査するファイルまたはJSON要素
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
                // 既存metaの入力ストリーム
                std::ifstream input(entry.path(), std::ios::binary);
                if (!input)
                {
                    continue;
                }
                // 既存metaのJSON
                nlohmann::json metadata;
                // 既存metaのJSON
                input >> metadata;
                // 既存metaの資産識別子
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
                // metaに対応する資産パス
                auto assetPath = entry.path();
                assetPath.replace_extension();
                if (!std::filesystem::is_regular_file(assetPath)
                    || IsTemporaryAssetFile(assetPath))
                {
                    continue;
                }
                // 基準パスからの相対位置
                const auto relative =
                    assetPath.lexically_relative(assetDirectory);
                if (IsRelativePathSafe(relative))
                {
                    // 登録先の位置とGUIDが初回登録か
                    const auto [existing, inserted] =
                        paths.try_emplace(guid, relative);
                    if (!inserted)
                    {
                        // 重複GUIDは表から除外し、走査順で解決先を決めません。
                        existing->second.clear();
                    }
                }
            }
            catch (const std::exception&)
            {
                // 壊れた.metaは通常のAssetDatabaseと同じく無視し、JSONに保存されたfallback pathを使います。
            }
        }
        // 重複して解決先が不明なGUIDを除外します(entry: GUIDと現在パスの組)。
        std::erase_if(
            paths,
            [](const auto& entry)
            {
                return entry.second.empty();
            });
        return paths;
    }

    // GUIDを解決できる項目だけ現在パスへ更新し、変更数を返します(node: 変換するJSON, guidPaths: GUIDと現在パスの対応表)。
    std::size_t RewriteJsonGuidReferences(
        nlohmann::json& node,
        const AssetGuidPaths& guidPaths)
    {
        if (node.is_array())
        {
            // GUID参照のパス変更数
            std::size_t changes{};
            // 再帰検査するJSON値
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

        // 更新するJSON項目と現在パス
        std::vector<std::pair<std::string, std::string>> replacements;
        // JSON項目名とGUIDを表す値
        for (const auto& [key, value] : node.items())
        {
            // 作業パス名の時刻識別子
            constexpr std::string_view suffix = "Guid";
            if (key.size() <= suffix.size()
                || !key.ends_with(suffix)
                || !value.is_string())
            {
                continue;
            }
            // GUIDから解決した現在パス
            const auto resolved = guidPaths.find(
                value.get<std::string>());
            if (resolved == guidPaths.end())
            {
                continue;
            }

            // GUIDに対応するパスの項目名
            const auto field = key.substr(0, key.size() - suffix.size());
            // JSONの元パスの検索結果
            const auto fallback = node.find(field);
            if (fallback != node.end() && !fallback->is_string())
            {
                // 型が壊れたJSONをexportだけで黙って修復しません。
                continue;
            }
            // GUIDが示す現在のUTF-8パス
            const auto currentPath =
                LamaPon::PathToUtf8(resolved->second);
            if (fallback == node.end()
                || fallback->get_ref<const std::string&>() != currentPath)
            {
                replacements.emplace_back(field, currentPath);
            }
        }

        // GUID参照のパス変更数
        std::size_t changes = replacements.size();
        // 更新するパス項目名と現在の資産パス
        for (const auto& [field, path] : replacements)
        {
            node[field] = path;
        }
        // 再帰検査するJSON値
        for (auto& value : node)
        {
            changes += RewriteJsonGuidReferences(value, guidPaths);
        }
        return changes;
    }


    // 配布先でGUID表を作れないため、暗号化前の対象JSONだけ現在パスへ変換します(relativePath: 対象資産の相対パス, contents: packする内容の更新先, guidPaths: GUIDと現在パスの対応表)。
    void RewritePackedJsonGuidReferences(
        const std::filesystem::path& relativePath,
        std::vector<std::uint8_t>& contents,
        const AssetGuidPaths& guidPaths)
    {
        // 比較用に小文字化した資産名
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
            // 暗号化前に変換するJSON
            auto document = nlohmann::json::parse(
                contents.begin(),
                contents.end());
            if (RewriteJsonGuidReferences(document, guidPaths) == 0)
            {
                return;
            }
            // パス変更を反映したJSON文字列
            const auto serialized = document.dump();
            contents.assign(serialized.begin(), serialized.end());
        }
        catch (const std::exception&)
        {
            // 変換に失敗したJSONは元のバイト列でpackし、妥当性の診断は各loaderへ任せます。
        }
    }

    // GUIDを優先して資産パスを解決し、参照型の不正を通知します(node: 参照を持つJSON, field: パスの項目名, guidPaths: GUIDと現在パスの対応表, malformed: 不正参照かの出力先)。
    std::filesystem::path ResolveJsonAssetReference(
        const nlohmann::json& node,
        const std::string_view field,
        const AssetGuidPaths& guidPaths,
        bool& malformed)
    {
        malformed = false;
        // JSONで照合する資産項目名
        const std::string fieldName(field);
        // GUID未解決時に使う元パス
        std::filesystem::path fallback;
        // JSONの参照パスの検索結果
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

        // JSONの参照GUIDの検索結果
        const auto guid = node.find(fieldName + "Guid");
        if (guid != node.end() && !guid->is_null())
        {
            if (!guid->is_string())
            {
                malformed = true;
                return {};
            }
            // GUID表またはモデル要件の検索結果
            const auto found = guidPaths.find(
                guid->get<std::string>());
            if (found != guidPaths.end())
            {
                return found->second;
            }
        }
        return fallback;
    }

    // モデル形状から材質用途を判定し、要件と診断をキャッシュします(node: モデル参照を持つJSON, guidPaths: GUIDと現在パスの対応表, assets: モデルの読込先, cache: モデル要件の保存先, invalidReferences: 不正参照の追加先)。
    ModelRendererShaderRequirements
        ResolveModelRendererShaderRequirements(
            const nlohmann::json& node,
            const AssetGuidPaths& guidPaths,
            LamaPon::AssetManager& assets,
            ModelRendererRequirementCache& cache,
            std::vector<std::string>& invalidReferences)
    {
        // モデルが必要とするシェーダー種別
        ModelRendererShaderRequirements result;
        // 参照の型または変換が不正か
        bool malformed{};
        // GUIDを優先して解決したモデル
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
        // モデル未指定時は実行時と同じ通常材質の要件を使います。
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

        // 正規化した資産参照キー
        const auto key = ShaderReferenceKey(modelPath);
        // GUID表またはモデル要件の検索結果
        if (const auto found = cache.find(key);
            found != cache.end())
        {
            if (!found->second.error.empty())
            {
                invalidReferences.push_back(found->second.error);
            }
            return found->second;
        }

        // モデルの小文字化した拡張子
        const auto extension =
            ShaderReferenceKey(modelPath.extension());
        // 資産管理で解決したモデルパス
        const auto resolvedPath = assets.ResolvePath(modelPath);
        if (!assets.FileExists(resolvedPath))
        {
            result.error = LamaPon::PathToUtf8(modelPath)
                + " (referenced ModelRenderer model does not exist)";
        }
        else if (extension == ".gltf" || extension == ".glb")
        {
            // glTFとFBXの直接HLSLはスキニング用とし、manifestの用途はprimitiveごとに判断します。
            result.direct = DirectShaderSkinnedVertex
                | DirectShaderSkinnedPixel;
            try
            {
                // 通常材質を使うprimitiveがあるか
                bool requiresForwardRole{};
                // スキニング材質が必要か
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
            // モデル検査の失敗を要件の診断に残します(exception: 検査の失敗理由)。
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
                // 通常材質を使うprimitiveがあるか
                bool requiresForwardRole{};
                // スキニング材質が必要か
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
            // モデル検査の失敗を要件の診断に残します(exception: 検査の失敗理由)。
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

        // 保存したモデル要件の位置と新規登録できたか
        const auto [stored, inserted] = cache.emplace(key, result);
        static_cast<void>(inserted);
        if (!stored->second.error.empty())
        {
            invalidReferences.push_back(stored->second.error);
        }
        return stored->second;
    }

    // 材質資産の参照要件を累積し、参照が指定されていればtrueを返します(node: 参照を持つJSON, field: パスの項目名, expectedSuffix: 許可するファイル名末尾, required: 消費側の必要用途, guidPaths: GUIDと現在パスの対応表, requirements: 要件の追加先, invalidReferences: 不正参照の追加先)。
    bool AddReferencedAssetRequirement(
        const nlohmann::json& node,
        const std::string_view field,
        const std::string_view expectedSuffix,
        const ShaderConsumerRequirements required,
        const AssetGuidPaths& guidPaths,
        MaterialAssetRequirements& requirements,
        std::vector<std::string>& invalidReferences)
    {
        // 参照の型または変換が不正か
        bool malformed{};
        // GUIDを優先して解決した資産
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
        // 正規化した資産参照キー
        const auto key = ShaderReferenceKey(path);
        if (key.ends_with(expectedSuffix))
        {
            // 累積する既存の資産用途要件
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

    // HLSLかmanifestの参照に必要用途を追加します(path: 相対シェーダーパス, directRequired: 直接HLSLの必要種別, manifestRequired: manifestの必要用途, directRequirements: 直接参照要件の追加先, manifestRequirements: manifest要件の追加先, invalidReferences: 不正参照の追加先)。
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

        // 正規化した資産参照キー
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

    // JSONのシェーダー参照を解決して必要用途を追加します(node: 参照を持つJSON, directRequired: 直接HLSLの必要種別, manifestRequired: manifestの必要用途, guidPaths: GUIDと現在パスの対応表, directRequirements: 直接参照要件の追加先, manifestRequirements: manifest要件の追加先, invalidReferences: 不正参照の追加先)。
    void AddReferencedShaderRequirement(
        const nlohmann::json& node,
        const std::uint8_t directRequired,
        const std::uint8_t manifestRequired,
        const AssetGuidPaths& guidPaths,
        DirectShaderRequirements& directRequirements,
        ManifestShaderRequirements& manifestRequirements,
        std::vector<std::string>& invalidReferences)
    {
        // 参照の型または変換が不正か
        bool malformed{};
        // GUIDを優先して解決した資産
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


    // SceneとPrefabの用途別シェーダー・材質要件を再帰収集します(node: 調べるJSON, guidPaths: GUIDと現在パスの対応表, assets: モデルの読込先, modelRequirementCache: モデル要件の保存先, shaderRequirements: 直接HLSL要件の追加先, manifestRequirements: manifest要件の追加先, materialAssetRequirements: 材質資産要件の追加先, invalidReferences: 不正参照の追加先)。
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
            // 直接HLSLの必要種別マスク
            std::uint8_t shaderRequired{};
            // manifestの必要用途マスク
            std::uint8_t manifestRequired{};
            // 材質資産の必要用途
            ShaderConsumerRequirements materialRequired{};
            // JSONのコンポーネント型検索結果
            const auto type = node.find("type");
            // コンポーネントの型名
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
                // モデル形状から得た必要用途
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

            // 材質資産の参照が指定済みか
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
            // 材質資産が設定されている場合は直接指定した古いシェーダーを要件に含めません。
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

            // 再帰検査するJSON値
            // JSONの参照パスの検索結果
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
            // 再帰検査するJSON値
            // JSONの参照パスの検索結果
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

    // フォルダー階層を含むZIPを非表示で作成し、終了コードとサイズを検証します(folder: ZIPへ含めるフォルダー, zipPath: 出力するZIPパス)。
    void RunSystemTar(
        const std::filesystem::path& folder,
        const std::filesystem::path& zipPath)
    {
        // 未完成ZIPの削除失敗状態
        std::error_code removeError;
        std::filesystem::remove(zipPath, removeError);

        // Windowsのシステムディレクトリ
        wchar_t systemDirectory[MAX_PATH]{};
        if (GetSystemDirectoryW(
                systemDirectory,
                MAX_PATH) == 0)
        {
            throw std::runtime_error(
                "Could not locate the Windows system directory.");
        }
        // システムのtar実行ファイル
        const auto tarPath =
            std::filesystem::path(systemDirectory)
            / L"tar.exe";
        if (!std::filesystem::is_regular_file(tarPath))
        {
            throw ExportError(
                "tar.exe was not found (required for zip export, bundled with Windows 10 and later)",
                tarPath);
        }


        // tarまたはsigntoolの起動引数
        std::wstring commandLine =
            L"\"" + tarPath.wstring() + L"\" -a -c -f \""
            + zipPath.wstring() + L"\" -C \""
            + folder.parent_path().wstring() + L"\" \""
            + folder.filename().wstring() + L"\"";

        // 非表示プロセスの起動情報
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        // 起動したプロセスのハンドル
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
        // 外部ツールの終了コード
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

        // ZIPサイズ取得の失敗状態
        std::error_code sizeError;
        // 作成したZIPのサイズ・byte
        const auto zipSize =
            std::filesystem::file_size(zipPath, sizeError);
        // WebDAVで終了コード0の空ZIPが作られる場合もあるため、最小ヘッダーだけの22byte以下は拒否します。
        if (sizeError || zipSize <= 22)
        {
            std::filesystem::remove(zipPath, removeError);
            throw ExportError(
                "Zip archive was empty after creation",
                zipPath);
        }
    }

    // Windowsの引数用に引用符と末尾のバックスラッシュをエスケープします(argument: 引用する引数)。
    std::wstring QuoteSigningArgument(
        const std::wstring_view argument)
    {
        // 引用符で囲った起動引数
        std::wstring quoted{ L'"' };
        // 連続するバックスラッシュの数
        std::size_t backslashes = 0;
        // 引用または名前調整する文字
        for (const wchar_t character : argument)
        {
            if (character == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (character == L'"')
            {
                quoted.append(backslashes * 2 + 1, L'\\');
                quoted.push_back(character);
                backslashes = 0;
                continue;
            }
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(character);
        }
        quoted.append(backslashes * 2, L'\\');
        quoted.push_back(L'"');
        return quoted;
    }

    // 署名ツールを最大五分待ち、失敗を例外にします(signing: 署名ツールの設定, arguments: signかverifyの非空引数列, binary: 処理するバイナリのパス)。
    void RunSignTool(
        const LamaPon::GameSigningOptions& signing,
        const std::vector<std::wstring>& arguments,
        const std::filesystem::path& binary)
    {
        // 署名ツールの正規化した絶対パス
        const auto signTool = std::filesystem::absolute(
            signing.signToolPath).lexically_normal();
        // tarまたはsigntoolの起動引数
        std::wstring commandLine =
            QuoteSigningArgument(signTool.wstring());
        // 署名ツールへ渡す引数
        for (const auto& argument : arguments)
        {
            commandLine += L" " + QuoteSigningArgument(argument);
        }
        commandLine += L" " + QuoteSigningArgument(binary.wstring());

        // 非表示プロセスの起動情報
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        // 起動したプロセスのハンドル
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(
                signTool.c_str(), commandLine.data(),
                nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                nullptr, nullptr, &startup, &process))
        {
            throw ExportError(
                "Could not start SignTool", signTool);
        }
        // 署名ツールの待機結果
        const DWORD waitResult = WaitForSingleObject(
            process.hProcess, 300000);
        if (waitResult == WAIT_TIMEOUT)
        {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, INFINITE);
        }
        // 外部ツールの終了コード
        DWORD exitCode = 1;
        if (waitResult == WAIT_OBJECT_0)
        {
            GetExitCodeProcess(process.hProcess, &exitCode);
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        if (waitResult != WAIT_OBJECT_0 || exitCode != 0)
        {
            throw ExportError(
                arguments.front() == L"sign"
                    ? "SignTool signing failed or timed out (exit code "
                        + std::to_string(exitCode) + ")"
                    : "SignTool signature verification failed or timed out (exit code "
                        + std::to_string(exitCode) + ")",
                binary);
        }
    }

    // 有効な署名設定で各バイナリに署名し、その署名を検証します(signing: 検証済みの署名設定, binaries: 署名するバイナリの一覧)。
    void SignExportedBinaries(
        const LamaPon::GameSigningOptions& signing,
        const std::vector<std::filesystem::path>& binaries)
    {
        if (!signing.enabled)
        {
            return;
        }
        // 証明書のSHA-1を表す文字列
        const std::wstring thumbprint(
            signing.certificateSha1.begin(),
            signing.certificateSha1.end());
        // タイムスタンプ認証のURL
        const std::wstring timestampUrl(
            signing.timestampUrl.begin(),
            signing.timestampUrl.end());
        // 署名と検証の対象ファイル
        for (const auto& binary : binaries)
        {
            RunSignTool(signing,
                { L"sign", L"/sha1", thumbprint,
                  L"/fd", L"SHA256", L"/tr", timestampUrl,
                  L"/td", L"SHA256" },
                binary);
            RunSignTool(signing,
                { L"verify", L"/pa", L"/all", L"/tw" },
                binary);
            LamaPon::Logger::Instance().Info(
                "Signed and verified: "
                + LamaPon::PathToUtf8(binary.filename()));
        }
    }

    // コピーしたZIPのサイズを検証し、既存を退避してから公開します(sourceZip: 完成した元ZIP, destinationZip: 公開するZIPパス)。
    void PublishZipArchive(
        const std::filesystem::path& sourceZip,
        const std::filesystem::path& destinationZip)
    {
        // 検証する新ZIPの公開前パス
        const auto stagingZip = MakeSiblingWorkingPath(
            destinationZip,
            L"staging");
        // 既存ZIPの一時退避パス
        const auto backupZip = MakeSiblingWorkingPath(
            destinationZip,
            L"backup");

        // 配布内容かZIPのコピー失敗状態
        std::error_code copyError;
        std::filesystem::copy_file(
            sourceZip,
            stagingZip,
            std::filesystem::copy_options::none,
            copyError);

        // 元ZIPのサイズ取得失敗状態
        std::error_code sourceSizeError;
        // コピー先ZIPのサイズ取得失敗
        std::error_code stagingSizeError;
        // 元ZIPのサイズ・byte
        const auto sourceSize = std::filesystem::file_size(
            sourceZip,
            sourceSizeError);
        // コピー先ZIPのサイズ・byte
        const auto stagingSize = std::filesystem::file_size(
            stagingZip,
            stagingSizeError);
        // WebDAVはcopy完了後に失敗コードを返すこともあるため、最終サイズが一致していれば成功として扱います。
        if (sourceSizeError
            || stagingSizeError
            || sourceSize != stagingSize)
        {
            // 未完成物や退避物の削除失敗
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

        // 既存の配布ZIPがあるか
        const bool hadPreviousZip =
            std::filesystem::exists(destinationZip);
        if (hadPreviousZip)
        {
            // ZIPの配置または退避の失敗
            std::error_code renameError;
            if (!RenameWithRetry(
                    destinationZip,
                    backupZip,
                    renameError))
            {
                // 未完成物や退避物の削除失敗
                std::error_code cleanupError;
                std::filesystem::remove(stagingZip, cleanupError);
                throw std::filesystem::filesystem_error(
                    "Could not move the previous zip archive",
                    destinationZip,
                    backupZip,
                    renameError);
            }
        }

        // ZIPの配置または退避の失敗
        std::error_code renameError;
        if (!RenameWithRetry(
                stagingZip,
                destinationZip,
                renameError))
        {
            // 未完成物や退避物の削除失敗
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
            // 未完成物や退避物の削除失敗
            std::error_code cleanupError;
            std::filesystem::remove(backupZip, cleanupError);
        }
    }


    // 必要に応じて入力をローカルへ複製し、完成ZIPだけを配布先へ公開します(folder: 完成したゲームフォルダー, zipPath: 公開するZIPパス)。
    void CreateZipWithSystemTar(
        const std::filesystem::path& folder,
        const std::filesystem::path& zipPath)
    {
        // ZIP作成用のローカル作業パス
        const auto temporaryRoot =
            std::filesystem::temp_directory_path()
            / (L"LamaPonExportZip-"
                + std::to_wstring(
                    std::chrono::steady_clock::now()
                        .time_since_epoch()
                        .count()));
        // ZIP作業パス作成の失敗状態
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

        // 入力内容をローカルへ複製するか
        const bool useLocalInput =
            LamaPon::ShouldUseLocalGameModuleBuildCache(folder);
        // ZIPへ含めるゲームフォルダー
        const auto archiveFolder = useLocalInput
            ? temporaryRoot / folder.filename()
            : folder;
        // ローカルに作成する完成ZIP
        const auto localZip = temporaryRoot
            / (folder.filename().wstring() + L".zip");
        try
        {
            if (useLocalInput)
            {
                // 配布内容かZIPのコピー失敗状態
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
            // 未完成物や退避物の削除失敗
            std::error_code cleanupError;
            std::filesystem::remove_all(
                temporaryRoot,
                cleanupError);
            throw;
        }

        // 未完成物や退避物の削除失敗
        std::error_code cleanupError;
        std::filesystem::remove_all(
            temporaryRoot,
            cleanupError);
    }
}

namespace LamaPon
{
    // 署名が有効な場合だけツール・証明書・認証URLを検証します。
    void ValidateGameSigningOptions(
        const GameSigningOptions& options)
    {
        if (!options.enabled)
        {
            return;
        }
        if (!options.signToolPath.is_absolute()
            || !std::filesystem::is_regular_file(
                options.signToolPath))
        {
            throw std::invalid_argument(
                "Signing requires an absolute path to Windows SDK signtool.exe.");
        }
        // 小文字化する署名ツール名
        auto toolName = options.signToolPath.filename().wstring();
        // 署名ツール名を小文字へ揃えます(character: 変換する文字)。
        std::transform(
            toolName.begin(), toolName.end(), toolName.begin(),
            [](const wchar_t character)
            {
                return static_cast<wchar_t>(std::towlower(character));
            });
        if (toolName != L"signtool.exe")
        {
            throw std::invalid_argument(
                "The signing tool must be signtool.exe.");
        }
        // 証明書のSHA-1の長さと全ての文字を検証します(character: 検証する文字)。
        if (options.certificateSha1.size() != 40
            || !std::all_of(
                options.certificateSha1.begin(),
                options.certificateSha1.end(),
                [](const unsigned char character)
                {
                    return std::isxdigit(character) != 0;
                }))
        {
            throw std::invalid_argument(
                "Signing requires a 40-character hexadecimal certificate SHA-1 thumbprint.");
        }
        // URLのスキームと許可文字を検証します(character: 検証する文字)。
        if (!options.timestampUrl.starts_with("https://")
            || options.timestampUrl.size() <= 8
            || !std::all_of(
                options.timestampUrl.begin(),
                options.timestampUrl.end(),
                [](const unsigned char character)
                {
                    return character >= 0x21 && character <= 0x7e
                        && character != '"';
                }))
        {
            throw std::invalid_argument(
                "Signing requires an HTTPS RFC 3161 timestamp URL.");
        }
    }

    // 禁止文字を置換し、空白・ドットと予約デバイス名を調整します。
    std::wstring SanitizeGameFileName(
        const std::string& gameName)
    {
        // Windowsのファイル名禁止文字
        constexpr std::wstring_view invalidCharacters =
            LR"(\/:*?"<>|)";
        // 調整中のWindows用ファイル名
        std::wstring result;
        // 引用または名前調整する文字
        for (const wchar_t character : Utf8ToWide(gameName))
        {
            // ファイル名に使えない文字か
            const bool invalid = character < 0x20
                || invalidCharacters.find(character)
                    != std::wstring_view::npos;
            result.push_back(invalid ? L'_' : character);
        }

        // 先頭・末尾の空白とドットはWindowsのファイル名で使えないため取り除きます。
        // 先頭の有効なファイル名文字位置
        const auto first = result.find_first_not_of(L" .");
        // 末尾の有効なファイル名文字位置
        const auto last = result.find_last_not_of(L" .");
        result = first == std::wstring::npos
            ? std::wstring{}
            : result.substr(first, last - first + 1);
        if (result.empty())
        {
            return L"LamaPonGame";
        }

        // CONやNULなどの予約デバイス名はそのまま使えないため先頭へ「_」を付けます。
        // 予約名照合用の大文字表現
        std::wstring upper = result;
        // 予約デバイス名との比較用に大文字へ揃えます(value: 変換する文字)。
        std::transform(
            upper.begin(),
            upper.end(),
            upper.begin(),
            [](const wchar_t value)
            {
                return static_cast<wchar_t>(
                    std::towupper(value));
            });
        // Windowsの予約デバイス名
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

    // 資産とシェーダーを準備して署名を検証し、完成物の公開後に任意のZIPを作成します。
    GameExportResult ExportGamePackage(
        const GameExportOptions& options)
    {
        ValidateGameSigningOptions(options.signing);
        // 書き出し元Runtimeの基準パス
        const auto runtimeDirectory = std::filesystem::weakly_canonical(
            options.runtimeDirectory);
        // 書き出し元assetsの基準パス
        const auto assetDirectory = std::filesystem::weakly_canonical(
            options.assetDirectory);
        // 完成した配布フォルダーのパス
        const auto outputDirectory = std::filesystem::absolute(
            options.outputDirectory).lexically_normal();
        // junction・symlink・SUBST別名を含む出力先の実体パス
        const auto canonicalOutputDirectory =
            std::filesystem::weakly_canonical(outputDirectory);

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
        if (canonicalOutputDirectory == assetDirectory
            || IsPathWithin(
                assetDirectory,
                canonicalOutputDirectory)
            || IsPathWithin(
                canonicalOutputDirectory,
                assetDirectory))
        {
            throw ExportError(
                "Export directory cannot contain or be inside the asset directory",
                outputDirectory);
        }
        if (canonicalOutputDirectory == runtimeDirectory
            || IsPathWithin(
                canonicalOutputDirectory,
                runtimeDirectory))
        {
            throw ExportError(
                "Export directory cannot contain the runtime directory",
                outputDirectory);
        }
        // 配布用設定の不正は作業フォルダーの作成前に拒否します。
        ValidateProjectSettings(
            options.projectSettings,
            ProjectSettingsFileType::GamePackage);

        // 作業開始前にnative宣言とDLL名を検証し、SDK未配置の依存は警告して除外します。
        // native宣言の走査結果
        const auto packageScan =
            ScanPackageNativeDependencies(assetDirectory);
        if (!packageScan.errors.empty())
        {
            // 検証失敗の通知文
            std::string message =
                "パッケージのnative設定を読めません:";
            // native宣言の個別失敗理由
            for (const auto& failure : packageScan.errors)
            {
                message += "\n  - " + failure;
            }
            throw std::runtime_error(message);
        }
        // 使用可能な依存と不足説明
        const auto nativeSelection =
            SelectAvailablePackageNativeDependencies(
                packageScan.packages);
        // 除外したnative依存の理由
        for (const auto& missing : nativeSelection.missing)
        {
            Logger::Instance().Warning(missing);
        }
        // 同梱するnative DLLの一覧
        const auto packageRuntimeFiles =
            CollectPackageRuntimeFiles(nativeSelection.available);

        // 元のゲーム実行ファイル
        const auto gameExecutable =
            runtimeDirectory / L"LamaPonGame.exe";
        // 元のRuntime DLL
        const auto runtimeLibrary =
            runtimeDirectory / L"LamaPonRuntime.dll";
        // 同梱するXAudio DLL
        const auto audioRuntime =
            runtimeDirectory / L"xaudio2_9redist.dll";
        // 同梱候補のGame Module DLL
        const auto gameModule = options.gameModulePath.empty()
            ? runtimeDirectory / L"LamaPonGameModule.dll"
            : std::filesystem::absolute(
                options.gameModulePath).lexically_normal();
        // 起動シーンの元ファイル
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

        // C++ソースを含む場合はGame Moduleの欠落・古い更新時刻を配布前に拒否します。
        // Game Moduleの再ビルド判定
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
            // Module更新時刻の取得失敗
            std::error_code moduleTimeError;
            // Runtime更新時刻の取得失敗
            std::error_code runtimeTimeError;
            // Moduleの更新時刻
            const auto moduleTime = std::filesystem::last_write_time(
                gameModule,
                moduleTimeError);
            // Runtimeの更新時刻
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
        // ゲームアイコンの元資産
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

        // 配布フォルダーの親パス
        const auto outputParent = outputDirectory.parent_path();
        // 配布先の親パス作成失敗
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
        // 公開前のゲーム作成パス
        const auto stagingDirectory = MakeSiblingWorkingPath(
            outputDirectory,
            L"staging");
        // 既存配布先の一時退避パス
        const auto backupDirectory = MakeSiblingWorkingPath(
            outputDirectory,
            L"backup");


        // ゲーム名を反映したexe名
        const std::wstring exportedExecutableName =
            SanitizeGameFileName(
                options.projectSettings.gameName)
            + L".exe";

        try
        {
            // 公開前パスの作成失敗状態
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
            // 同梱候補のEOS Runtime DLL
            const auto eosRuntime = runtimeDirectory / "EOSSDK-Win64-Shipping.dll";
            if (options.projectSettings.network.backend == NetworkBackend::EpicOnlineServices
                && (!HasEpicNetworkBackend() || !std::filesystem::is_regular_file(eosRuntime)))
                throw std::runtime_error("EOS runtime DLL is missing. Build LamaPon with the official EOS SDK before exporting an EOS game.");
            if (std::filesystem::is_regular_file(eosRuntime))
                std::filesystem::copy_file(eosRuntime, stagingDirectory / eosRuntime.filename());
            // 実行コードに伴う通知を配布先にも残します。
            // 欠けたSDKから不完全な配布物を作らないよう、コピー失敗時は中断します。
            // Runtimeのライセンス一覧パス
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


            // この配布物用に生成した暗号鍵
            const auto archiveKey = LamaPon::Crypto::RandomKey();
            EmbedArchiveKey(
                stagingDirectory / runtimeLibrary.filename(),
                archiveKey);

            // ゲームアイコンを実行ファイルへ埋め込みます（ExplorerのファイルアイコンとウィンドウのLamaPon標準アイコンが差し替わります）。
            if (!gameIconSource.empty())
            {
                ReplaceExecutableIcon(
                    stagingDirectory / exportedExecutableName,
                    BuildIcoFromImageFile(gameIconSource));
            }

            // 存在するVC++ランタイムDLLを同梱します。
            // 同梱候補のVC++ Runtime名
            for (const auto crtLibrary : RuntimeCrtLibraries)
            {
                // 同梱候補のVC++ DLLパス
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

                // Game Moduleと同じディレクトリのDLLを、既存の配布ファイルを上書きせずに同梱します。
                // 走査するDLL・資産・出力ファイル
                for (const auto& entry :
                    std::filesystem::directory_iterator(
                        gameModule.parent_path()))
                {
                    if (!entry.is_regular_file())
                    {
                        continue;
                    }
                    // 小文字化するファイル拡張子
                    auto extension = entry.path().extension().wstring();
                    // 拡張子を小文字へ揃えます(value: 変換する文字)。
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

                    // 同梱ファイルの配置先
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

            // 宣言されたnative DLLを実行ファイルの隣へ配置し、同名の既存ファイルは拒否します。
            // パッケージが宣言したDLL
            for (const auto& runtimeFile : packageRuntimeFiles)
            {
                // 同梱ファイルの配置先
                const auto destination =
                    stagingDirectory / runtimeFile.fileName;
                if (std::filesystem::exists(destination))
                {
                    throw std::runtime_error(
                        "パッケージ " + runtimeFile.packageName
                        + " が同梱するDLLと同じ名前のファイルが"
                          "すでにあります: "
                        + LamaPon::PathToUtf8(
                            std::filesystem::path{
                                runtimeFile.fileName }));
                }
                std::filesystem::copy_file(
                    runtimeFile.source,
                    destination);
            }

            // アーカイブから除外する拡張子
            const std::vector<std::wstring> skippedExtensions =
                options.projectSettings.stripShaderSourceOnExport
                    ? std::vector<std::wstring>{
                        L".hlsl",
                        L".hlsli" }
                    : std::vector<std::wstring>{};
            // GUIDと現在の資産パスの対応表
            const auto assetGuidPaths =
                ReadAssetGuidPaths(assetDirectory);
            // GUID解決を反映したアーカイブの生成結果(relativePath: 対象資産の相対パス, contents: packする内容の更新先)。
            const auto packResult = PackAssets(
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
                });
            // アーカイブ内のファイル名索引は暗号化されたままにし、配布内容の一覧はエディターのログで確認できるようにします。
            // 同梱資産の一覧通知文
            std::string assetInventory =
                "Exported asset inventory ("
                + std::to_string(packResult.fileCount) + " files):";
            // 同梱または除外した相対パス
            for (const auto& path : packResult.includedFiles)
            {
                assetInventory += "\n  + " + PathToUtf8(path);
            }
            Logger::Instance().Info(std::move(assetInventory));
            if (!packResult.excludedFiles.empty())
            {
                // 除外資産の一覧通知文
                std::string excludedInventory =
                    "Excluded development files from asset export:";
                // 同梱または除外した相対パス
                for (const auto& path : packResult.excludedFiles)
                {
                    excludedInventory += "\n  - " + PathToUtf8(path);
                }
                Logger::Instance().Warning(std::move(excludedInventory));
            }


            // AssetManagerのWICとD2Dに必要なCOMを初期化し、成功した呼び出しにだけ終了処理を対応させます。
            // この処理のCOM初期化結果
            const HRESULT comResult = CoInitializeEx(
                nullptr,
                COINIT_APARTMENTTHREADED);
            // 対応するCOM終了処理が必要か
            const bool comInitialized = SUCCEEDED(comResult);
            // 暗号化するシェーダーcacheパス
            const auto cacheDirectory =
                stagingDirectory / L"shader-cache";
            try
            {
                // 元資産を変更しない読込管理
                AssetManager exportAssets{ nullptr, nullptr };
                // metaを生成せず、欠落資産の一時GUIDはこの資産管理内だけで使います。
                exportAssets.SetAssetRoot(
                    assetDirectory,
                    false);

                // ソース同梱時は警告し、ソース除外時は例外で止めます(message: コンパイル失敗の説明)。
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


                // 全資産の使用キーワードの和
                std::vector<std::string> usedKeywords;
                // 直接HLSL参照の必要用途
                DirectShaderRequirements directShaderRequirements;
                // manifest参照の必要用途
                ManifestShaderRequirements manifestShaderRequirements;
                // 材質資産参照の必要用途
                MaterialAssetRequirements materialAssetRequirements;
                // モデル形状別の要件cache
                ModelRendererRequirementCache modelRequirementCache;
                // 不正な資産参照の診断一覧
                std::vector<std::string> invalidShaderReferences;
                // 走査するDLL・資産・出力ファイル
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
                        // キーワードと要件を読むJSON入力
                        std::ifstream input(
                            entry.path(),
                            std::ios::binary);
                        if (!input)
                        {
                            continue;
                        }
                        // 要件を収集するJSON文書
                        nlohmann::json document;
                        // 要件を収集するJSON文書
                        input >> document;
                        CollectShaderKeywords(
                            document,
                            usedKeywords);
                        // 今回追加する参照診断の先頭
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
                        // 参照診断または互換性理由の添字
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
                        // 読めないJSONは要件収集から除外します。
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

                // 実行時と同じくGUIDを元パスより優先し、材質資産から必要なシェーダーへ辿ります。
                // 材質資産の参照キーと必要な用途マスク
                for (const auto& [materialKey, required] :
                    materialAssetRequirements)
                {
                    // 参照する材質資産の相対パス
                    const auto materialRelative =
                        PathFromUtf8(materialKey);
                    // 読込対象の材質資産パス
                    const auto materialPath =
                        exportAssets.ResolvePath(materialRelative);
                    // 材質資産読込の失敗理由
                    std::string materialError;
                    try
                    {
                        // 読込済みの材質設定
                        const auto material = LoadLitMaterialAsset(
                            materialPath,
                            &exportAssets.Database(),
                            &exportAssets);
                        // 今回追加する参照診断の先頭
                        const auto invalidBegin =
                            invalidShaderReferences.size();
                        AddShaderPathRequirement(
                            material.Shader(),
                            required.direct,
                            required.manifest,
                            directShaderRequirements,
                            manifestShaderRequirements,
                            invalidShaderReferences);
                        // 参照診断または互換性理由の添字
                        for (auto index = invalidBegin;
                            index < invalidShaderReferences.size();
                            ++index)
                        {
                            invalidShaderReferences[index] =
                                PathToUtf8(materialRelative) + " -> "
                                + invalidShaderReferences[index];
                        }
                    }
                    // 準備失敗の理由を診断へ渡します(exception: 準備の失敗理由)。
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

                // 不正な参照の診断文
                for (const auto& reference :
                    invalidShaderReferences)
                {
                    reportShaderPrecompileFailure(
                        "A persisted shader, material, or model "
                        "reference is "
                        "invalid for export: "
                        + reference);
                }

                // コンパイル対象のHLSL数
                std::uint32_t shaderFiles{};
                // 走査するDLL・資産・出力ファイル
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
                    // 小文字化するファイル拡張子
                    auto extension =
                        entry.path().extension().wstring();
                    // 拡張子を小文字へ揃えます(value: 変換する文字)。
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
                    // ソースを除く配布では実行時に再生成できないため、キーワードで絞らず全バリアントを作成します。
                    static_cast<void>(PrecompileShader(
                        exportAssets,
                        entry.path(),
                        cacheDirectory,
                        {},
                        options.projectSettings
                                .stripShaderSourceOnExport
                            ? nullptr
                            : &usedKeywords));

                    // 通常の総当たり結果とは別に、参照用途とソースの宣言から必要な入口の成功を検証します。
                    // 用途と宣言から必要な入口一覧
                    std::vector<ShaderEntryPoint> requiredEntries;
                    // 重複しない必須入口を追加します(entryPoint: 入口名, target: シェーダーのtarget名)。
                    const auto addRequiredEntry =
                        [&](const char* const entryPoint,
                            const char* const target)
                        {
                            // 登録済みの同じ入口を検索します(candidate: 比較する登録済み入口)。
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

                    // コンパイルするHLSLの相対パス
                    const auto relativePath =
                        entry.path().lexically_relative(assetDirectory);
                    // 保存された用途要件の検索結果
                    const auto referenced =
                        directShaderRequirements.find(
                            ShaderReferenceKey(relativePath));
                    // 参照元が必要とする入口種別
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
                        // 入口を検査するHLSLのバイト列
                        const auto source =
                            exportAssets.ReadFileBytes(entry.path());
                        // HLSLで宣言された入口種別
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
                    // 準備失敗の理由を診断へ渡します(exception: 準備の失敗理由)。
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

                    // 用途と宣言から必要な入口
                    for (const auto& required : requiredEntries)
                    {
                        // 必須入口コンパイルの失敗理由
                        std::string compileError;
                        // コンパイルできたバリアント数
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

                // 未発見のHLSL参照キーと必要な用途マスク
                for (const auto& [missingShader, required] :
                    directShaderRequirements)
                {
                    static_cast<void>(required);
                    reportShaderPrecompileFailure(
                        "A referenced direct HLSL file does not exist: "
                        + missingShader);
                }


                // 必須stageを作成済みの画面効果数
                std::uint32_t screenEffectManifests{};
                // 必須stageを作成済みの材質数
                std::uint32_t materialManifests{};
                // 必須stageを作成済みのcompute数
                std::uint32_t computeManifests{};
                // 走査するDLL・資産・出力ファイル
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
                    // 検証するmanifestの相対パス
                    const auto manifestRelative =
                        entry.path().lexically_relative(assetDirectory);
                    // 保存された用途要件の検索結果
                    const auto referenced =
                        manifestShaderRequirements.find(
                            ShaderReferenceKey(manifestRelative));
                    // 参照元が必要とする用途マスク
                    std::uint8_t manifestRequirements{};
                    if (referenced != manifestShaderRequirements.end())
                    {
                        manifestRequirements = referenced->second;
                        manifestShaderRequirements.erase(referenced);
                    }

                    // 読込済みシェーダーmanifest
                    ShaderAssetDesc manifest;
                    // manifest読込の失敗理由
                    std::string manifestError;
                    if (!LoadShaderAssetDesc(
                            exportAssets,
                            entry.path(),
                            manifest,
                            manifestError))
                    {
                        // 不正なmanifestはソース同梱時は警告し、ソースを除く場合は書き出しを中止します。
                        reportShaderPrecompileFailure(
                            "Shader manifest was not precompiled for "
                            "export: "
                            + PathToUtf8(manifestRelative)
                            + ": " + manifestError);
                        continue;
                    }

                    // 指定用途のpassがあるか判定します(role: 調べるpassの用途)。
                    const auto hasRole =
                        [&manifest](const ShaderPassRole role)
                        {
                            // 指定用途に合うpassを検索します(pass: 比較するpass)。
                            return std::ranges::any_of(
                                manifest.passes,
                                [role](const ShaderPassDesc& pass)
                                {
                                    return pass.role == role;
                                });
                        };
                    // 参照元とmanifestの用途不一致
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
                    // 参照元の通常・スキニング用途
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
                        // 用途またはstageの失敗通知文
                        std::string diagnostic =
                            "Shader manifest is incompatible with its "
                            "consumer: " + PathToUtf8(manifestRelative)
                            + " (";
                        // 参照診断または互換性理由の添字
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

                    // manifestが示すHLSLの読込パス
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

                    // 材質は全pass、画面効果は先頭pass、computeは最初の必須compute stageを含むpassを使います。
                    // 用途に応じてコンパイルするpass
                    std::vector<const ShaderPassDesc*> passesToCompile;
                    if (manifest.type == ShaderAssetType::Material)
                    {
                        passesToCompile.reserve(manifest.passes.size());
                        // コンパイル候補または対象のpass
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
                        // 必須computeを探す候補pass
                        for (const auto& candidate : manifest.passes)
                        {
                            // 候補passのcompute stage
                            const auto* compute = FindShaderStage(
                                candidate,
                                ShaderStage::Compute);
                            if (compute != nullptr && !compute->optional)
                            {
                                passesToCompile.push_back(&candidate);
                                break;
                            }
                        }
                        // 必須compute stageを持つpassが無ければコンパイルせず診断します。
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

                    // 全必須stageが作成できたか
                    bool requiredStagesCompiled = true;
                    // コンパイル候補または対象のpass
                    for (const auto* const pass : passesToCompile)
                    {
                        // コンパイルするstageの宣言
                        for (const auto& stage : pass->stages)
                        {
                            // manifestの入口名とtarget
                            const ShaderEntryPoint manifestEntry{
                                stage.entryPoint.c_str(),
                                stage.target.c_str()
                            };
                            // stageコンパイルの失敗理由
                            std::string stageCompileError;
                            // 作成できたstageバリアント数
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
                                // 用途またはstageの失敗通知文
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
                // 未発見のmanifest参照キーと必要な用途マスク
                for (const auto& [missingManifest, required] :
                    manifestShaderRequirements)
                {
                    static_cast<void>(required);
                    reportShaderPrecompileFailure(
                        "A referenced shader manifest does not exist: "
                        + missingManifest);
                }
                // ソース無しで入口を解決する索引を保存します。
                WriteShaderCacheIndex(cacheDirectory);

                // 暗号化して検証済みのcache数
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
            // キャッシュ準備の失敗を処理します(exception: 準備の失敗理由)。
            catch (const std::exception& exception)
            {
                // この出力先の保留索引を排出し、暗号化済みと平文が混ざり得るキャッシュ全体を削除します。
                try
                {
                    WriteShaderCacheIndex(cacheDirectory);
                }
                catch (...)
                {
                    // 直後にdirectoryごと破棄するので、索引書込み失敗は元の診断を置き換えません。
                }
                // 未完成物や退避物の削除失敗
                std::error_code cleanupError;
                std::filesystem::remove_all(
                    cacheDirectory,
                    cleanupError);

                // 検証失敗の通知文
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

            // 配布用設定JSONの保存パス
            const auto settingsPath =
                stagingDirectory / L"LamaPonGame.json";
            SaveProjectSettings(
                settingsPath,
                options.projectSettings,
                ProjectSettingsFileType::GamePackage);
            Logger::Instance().Info(
                InspectExportFiles(stagingDirectory));
            // 署名するエンジン所有バイナリ
            std::vector<std::filesystem::path> ownedBinaries{
                stagingDirectory / exportedExecutableName,
                stagingDirectory / runtimeLibrary.filename()
            };
            if (std::filesystem::is_regular_file(gameModule))
            {
                ownedBinaries.push_back(
                    stagingDirectory / gameModule.filename());
            }
            // 鍵とアイコンの変更・配置後に署名を検証し、成功するまで既存の配布先を置き換えません。
            SignExportedBinaries(options.signing, ownedBinaries);
        }
        catch (...)
        {
            // 未完成物や退避物の削除失敗
            std::error_code cleanupError;
            std::filesystem::remove_all(
                stagingDirectory,
                cleanupError);
            throw;
        }

        // 既存の配布フォルダーがあるか
        const bool hadPreviousExport =
            std::filesystem::exists(outputDirectory);
        if (hadPreviousExport)
        {
            // 配布先の退避または公開の失敗
            std::error_code renameError;
            if (!RenameWithRetry(
                    outputDirectory,
                    backupDirectory,
                    renameError))
            {
                // 未完成物や退避物の削除失敗
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

        // 配布先の退避または公開の失敗
        std::error_code renameError;
        if (!RenameWithRetry(
                stagingDirectory,
                outputDirectory,
                renameError))
        {
            // 未完成物や退避物の削除失敗
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
            // 未完成物や退避物の削除失敗
            std::error_code cleanupError;
            std::filesystem::remove_all(
                backupDirectory,
                cleanupError);
        }

        // 完成した配布物の情報
        GameExportResult result;
        result.outputDirectory = outputDirectory;
        result.executablePath =
            outputDirectory / exportedExecutableName;
        // 走査するDLL・資産・出力ファイル
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
            // 完成した配布物のZIPパス
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
