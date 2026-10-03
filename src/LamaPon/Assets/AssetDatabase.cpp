#include "LamaPon/Assets/AssetDatabase.h"

#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/PathUtils.h"

#include <Windows.h>
#include <nlohmann/json.hpp>
#include <ufbx.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <functional>
#include <random>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace
{
    using Json = nlohmann::json;

    // 空・絶対パス・親階層を含むパスを拒否する(path: 検証する相対パス)。
    bool IsSafeRelativePath(
        const std::filesystem::path& path)
    {
        if (path.empty() || path.is_absolute())
        {
            return false;
        }
        // 親階層を検証するパス要素
        for (const auto& part : path)
        {
            if (part == L"..")
            {
                return false;
            }
        }
        return true;
    }

    // 内部作業用・移行バックアップをアセット対象から除く(path: 判定するパス)。
    bool IsTemporaryAssetFile(
        const std::filesystem::path& path)
    {
        // 作業ファイル・バックアップの名前
        const auto name = path.filename().wstring();
        return name.find(L".lamapon-delete")
                != std::wstring::npos
            || name.ends_with(L".lamapon-remap.tmp")
            || name.ends_with(L".bak");
    }

    // 文字列を小文字に変換する(value: 変換する文字列)。
    std::string Lowercase(std::string value)
    {
        // 各バイトを小文字にする(character: 符号なしのUTF8バイト)。
        std::ranges::transform(
            value,
            value.begin(),
            [](const unsigned char character)
            {
                return static_cast<char>(
                    std::tolower(character));
            });
        return value;
    }

    // 別名へ書き終えてから元JSONを置換し、失敗は例外にする(path: 正式な保存先, document: 完成したJSON)。
    void WriteJsonAtomically(
        const std::filesystem::path& path,
        const Json& document)
    {
        // 正式保存前の別名JSONパス
        const auto temporaryPath =
            path.wstring() + L".lamapon-remap.tmp";
        {
            // 完成前の別名JSON出力
            std::ofstream output(
                temporaryPath,
                std::ios::binary | std::ios::trunc);
            if (!output)
            {
                throw std::runtime_error(
                    "Could not create asset database file: "
                    + LamaPon::PathToUtf8(path));
            }
            output << document.dump(2) << '\n';
            output.close();
            if (!output)
            {
                // 保存失敗時の候補削除結果
                std::error_code cleanupError;
                std::filesystem::remove(
                    temporaryPath,
                    cleanupError);
                throw std::runtime_error(
                    "Could not write asset database file: "
                    + LamaPon::PathToUtf8(path));
            }
        }

        if (!MoveFileExW(
                temporaryPath.c_str(),
                path.c_str(),
                MOVEFILE_REPLACE_EXISTING
                    | MOVEFILE_WRITE_THROUGH))
        {
            // 置換に失敗したWindows結果
            const DWORD error = GetLastError();
            // 保存失敗時の候補削除結果
            std::error_code cleanupError;
            std::filesystem::remove(
                temporaryPath,
                cleanupError);
            throw std::runtime_error(
                "Could not replace asset database file: "
                + LamaPon::PathToUtf8(path)
                + " (Windows error "
                + std::to_string(error)
                + ")");
        }
    }

    // JSONファイルを読み、開けない場合や構文不正は例外にする(path: 読み込むパス)。
    Json ReadJson(const std::filesystem::path& path)
    {
        // 読み込むJSONファイル
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error(
                "Could not read JSON asset: "
                + LamaPon::PathToUtf8(path));
        }
        // 読み込み・書き換え用JSON
        Json document;
        // 読み込み・書き換え用JSON
        input >> document;
        return document;
    }
}

namespace LamaPon
{
    void AssetDatabase::SetAssetRoot(
        std::filesystem::path assetRoot)
    {
        m_assetRoot = std::filesystem::absolute(
            std::move(assetRoot)).lexically_normal();
        m_assets.clear();
        m_pathToIndex.clear();
        m_guidToIndex.clear();
        m_refreshed = false;
        // 依存キャッシュの保存先はルート別なので、次の走査で読み直す。
        m_fbxDependencyCacheLoaded = false;
        m_fbxDependencyCacheDirty = false;
        m_fbxDependencyCache.clear();
    }

    AssetDatabaseRefreshResult AssetDatabase::Refresh(
        const bool createMissingMeta)
    {
        m_assets.clear();
        m_pathToIndex.clear();
        m_guidToIndex.clear();

        // 走査・置換件数の結果
        AssetDatabaseRefreshResult result;
        if (!std::filesystem::is_directory(
                m_assetRoot))
        {
            return result;
        }

        // 走査したアセットの絶対パス
        std::vector<std::filesystem::path> paths;
        // アセット走査の失敗情報
        std::error_code iteratorError;
        // 権限不足を除外する走査設定
        const auto options =
            std::filesystem::directory_options::
                skip_permission_denied;
        // アセットルートの再帰走査位置
        for (std::filesystem::recursive_directory_iterator iterator{
                m_assetRoot,
                options,
                iteratorError
            };
            iterator
                != std::filesystem::
                    recursive_directory_iterator{};
            iterator.increment(iteratorError))
        {
            if (iteratorError)
            {
                iteratorError.clear();
                continue;
            }
            if (!iterator->is_regular_file(
                    iteratorError)
                || iteratorError
                || IsMetaFile(iterator->path())
                || IsTemporaryAssetFile(iterator->path()))
            {
                iteratorError.clear();
                continue;
            }
            paths.push_back(
                iterator->path().lexically_normal());
        }
        std::ranges::sort(paths);

        // この走査で登録したGUID集合
        std::set<std::string> usedGuids;
        // 処理するアセットの絶対パス
        for (const auto& absolutePath : paths)
        {
            // ルート内の相対アセットパス
            const auto relativePath =
                absolutePath.lexically_relative(
                    m_assetRoot);
            if (!IsSafeRelativePath(relativePath))
            {
                continue;
            }

            // 対応するmetaの絶対パス
            const auto metaAbsolute =
                MetaPathFor(absolutePath);
            // アセットの32桁識別子
            std::string guid;
            if (std::filesystem::is_regular_file(
                    metaAbsolute))
            {

                // metaの形式・解析エラー
                std::string problem;
                try
                {
                    // 既存metaのJSON内容
                    const auto metadata =
                        ReadJson(metaAbsolute);
                    if (metadata.value(
                            "format",
                            std::string{})
                            != "LamaPonAssetMeta"
                        || metadata.value("version", 0)
                            != 1)
                    {
                        problem = "Unsupported asset metadata";
                    }
                    else
                    {
                        guid = metadata.value(
                            "guid",
                            std::string{});
                        if (!IsValidGuid(guid))
                        {
                            problem = "Invalid asset GUID";
                        }
                    }
                }
                // exception: metaの解析失敗
                catch (const std::exception& exception)
                {
                    problem = exception.what();
                }
                // 既存GUIDを無断で変えないため、壊れたmetaは再生成せず読み飛ばす。
                if (!problem.empty())
                {
                    Logger::Instance().Warning(
                        "アセットの.metaが読めないため、この"
                        "アセットを飛ばしました。.metaを直すか"
                        "削除してください（削除すると作り直します"
                        "が、GUIDが変わるので他からの参照は"
                        "切れます）: "
                        + PathToUtf8(metaAbsolute)
                        + " — "
                        + problem);
                    continue;
                }
            }
            else
            {
                do
                {
                    guid = CreateGuid();
                }
                while (usedGuids.contains(guid));

                if (createMissingMeta)
                {
                    try
                    {
                        WriteJsonAtomically(
                            metaAbsolute,
                            {
                                {
                                    "format",
                                    "LamaPonAssetMeta"
                                },
                                { "version", 1 },
                                { "guid", guid },
                                {
                                    "importer",
                                    ImporterFor(
                                        relativePath)
                                }
                            });
                        ++result.createdMetaCount;
                    }
                    catch (const std::exception&)
                    {
                        // metaを保存できなくても、この走査で生成したGUIDをメモリー上で使う。
                    }
                }
            }

            if (!usedGuids.emplace(guid).second)
            {
                throw std::runtime_error(
                    "Duplicate asset GUID detected: "
                    + guid);
            }

            // アセットを追加する一覧位置
            const std::size_t index =
                m_assets.size();
            m_assets.push_back(
                {
                    guid,
                    relativePath,
                    MetaPathFor(relativePath),
                    ImporterFor(relativePath),
                    {},
                    {}
                });
            m_pathToIndex.emplace(
                PathKey(relativePath),
                index);
            m_guidToIndex.emplace(
                guid,
                index);
        }

        BuildDependencies();
        SaveFbxDependencyCache();
        m_refreshed = true;
        result.assetCount = m_assets.size();
        // 処理中のアセットレコード
        for (const auto& asset : m_assets)
        {
            result.dependencyCount +=
                asset.dependencies.size();
        }
        return result;
    }

    const AssetRecord* AssetDatabase::FindByPath(
        const std::filesystem::path& path) const noexcept
    {
        try
        {
            // ルートからの相対パス
            const auto relative = RelativePath(path);
            // パス・GUID索引の検索位置
            const auto found =
                m_pathToIndex.find(PathKey(relative));
            return found != m_pathToIndex.end()
                ? &m_assets[found->second]
                : nullptr;
        }
        catch (...)
        {
            return nullptr;
        }
    }

    const AssetRecord* AssetDatabase::FindByGuid(
        const std::string_view guid) const noexcept
    {
        // パス・GUID索引の検索位置
        const auto found =
            m_guidToIndex.find(std::string(guid));
        return found != m_guidToIndex.end()
            ? &m_assets[found->second]
            : nullptr;
    }

    std::string AssetDatabase::GuidForPath(
        const std::filesystem::path& path) const
    {
        // 索引から借用するレコード
        if (const auto* record = FindByPath(path))
        {
            return record->guid;
        }
        return {};
    }

    std::filesystem::path AssetDatabase::ResolveGuid(
        const std::string_view guid,
        std::filesystem::path fallback) const
    {
        // 索引から借用するレコード
        if (const auto* record = FindByGuid(guid))
        {
            return record->path;
        }
        return std::move(fallback);
    }

    AssetReferenceRemapResult
        AssetDatabase::RemapJsonReferences(
            const std::filesystem::path& oldPath,
            const std::filesystem::path& newPath,
            const bool includeChildren)
    {
        // 置換前のルート内相対パス
        const auto oldRelative =
            RelativePath(oldPath);
        // 置換後のルート内相対パス
        const auto newRelative =
            RelativePath(newPath);
        if (!IsSafeRelativePath(oldRelative)
            || !IsSafeRelativePath(newRelative))
        {
            throw std::invalid_argument(
                "Asset remap paths must be safe and relative.");
        }

        // 走査・置換件数の結果
        AssetReferenceRemapResult result;
        // 一致する相対パス文字列を置換する(value: 元のJSON文字列)。
        const auto remapString =
            [&oldRelative, &newRelative, includeChildren](
                const std::string& value)
                -> std::pair<std::string, bool>
            {
                if (value.empty())
                {
                    return { value, false };
                }
                // 文字列から得た参照先パス
                const auto candidate =
                    PathFromUtf8(value).lexically_normal();
                if (candidate.is_absolute())
                {
                    return { value, false };
                }
                if (PathKey(candidate)
                    == PathKey(oldRelative))
                {
                    return {
                        PathToUtf8(newRelative),
                        true
                    };
                }
                if (!includeChildren)
                {
                    return { value, false };
                }
                // 移動する子要素の相対末尾
                const auto suffix =
                    candidate.lexically_relative(
                        oldRelative);
                if (!IsSafeRelativePath(suffix))
                {
                    return { value, false };
                }
                return {
                    PathToUtf8(
                        (newRelative / suffix)
                            .lexically_normal()),
                    true
                };
            };

        // 処理中のアセットレコード
        for (const auto& asset : m_assets)
        {
            if (Lowercase(LamaPon::PathToUtf8(asset.path.extension()))
                    != ".json")
            {
                continue;
            }
            // 処理するアセットの絶対パス
            const auto absolutePath =
                m_assetRoot / asset.path;
            // 読み込み・書き換え用JSON
            auto document = ReadJson(absolutePath);
            // このJSONで置換した文字列数
            std::size_t changedReferences{};
            // 子要素を再帰走査しパスを置換する(value: 書き換えるJSON値)。
            const std::function<void(Json&)>
                visit = [&](Json& value)
                {
                    if (value.is_string())
                    {
                        // replacement: 置換後の文字列、changed: パス置換の有無
                        const auto [replacement, changed] =
                            remapString(
                                value.get_ref<
                                    const std::string&>());
                        if (changed)
                        {
                            value = replacement;
                            ++changedReferences;
                        }
                        return;
                    }
                    if (value.is_array())
                    {
                        // 再帰処理する子のJSON値
                        for (auto& child : value)
                        {
                            visit(child);
                        }
                        return;
                    }
                    if (value.is_object())
                    {
                        // key: 走査用の項目名、child: 置換対象の値
                        for (auto& [key, child] :
                            value.items())
                        {
                            static_cast<void>(key);
                            visit(child);
                        }
                    }
                };
            visit(document);
            // JSONごとに保存するため、途中失敗でも保存済みの置換は残る。
            if (changedReferences != 0)
            {
                WriteJsonAtomically(
                    absolutePath,
                    document);
                ++result.fileCount;
                result.referenceCount +=
                    changedReferences;
            }
        }
        static_cast<void>(Refresh(true));
        return result;
    }

    bool AssetDatabase::IsMetaFile(
        const std::filesystem::path& path) noexcept
    {
        return Lowercase(
            LamaPon::PathToUtf8(path.extension())) == ".meta";
    }

    std::filesystem::path AssetDatabase::MetaPathFor(
        const std::filesystem::path& assetPath)
    {
        return std::filesystem::path(
            assetPath.wstring() + L".meta");
    }

    bool AssetDatabase::IsValidGuid(
        const std::string_view guid) noexcept
    {
        // 32文字の全てが十六進数字か調べる(character: 検証する各バイト)。
        return guid.size() == 32
            && std::ranges::all_of(
                guid,
                [](const unsigned char character)
                {
                    return std::isxdigit(character) != 0;
                });
    }

    std::filesystem::path AssetDatabase::RelativePath(
        const std::filesystem::path& path) const
    {
        // 字句正規化した入力パス
        const auto normalized =
            path.lexically_normal();
        if (!normalized.is_absolute())
        {
            return normalized;
        }
        // ルートからの相対パス
        const auto relative =
            normalized.lexically_relative(
                m_assetRoot);
        if (!IsSafeRelativePath(relative))
        {
            throw std::invalid_argument(
                "Asset path is outside the asset root.");
        }
        return relative;
    }

    std::wstring AssetDatabase::PathKey(
        const std::filesystem::path& path)
    {
        // 比較・識別用の小文字パス
        auto key = path.lexically_normal()
            .generic_wstring();
        // パスキーの各文字を小文字にする(character: 変換する文字)。
        std::ranges::transform(
            key,
            key.begin(),
            [](const wchar_t character)
            {
                return static_cast<wchar_t>(
                    std::towlower(character));
            });
        return key;
    }

    std::string AssetDatabase::ImporterFor(
        const std::filesystem::path& path)
    {
        // 取り込み種別用の小文字名
        const auto name =
            Lowercase(LamaPon::PathToUtf8(path.filename()));
        if (name.ends_with(".scene.json"))
        {
            return "Scene";
        }
        if (name.ends_with(".prefab.json"))
        {
            return "Prefab";
        }
        if (name.ends_with(".material.json"))
        {
            return "LitMaterial";
        }
        if (name.ends_with(".animation.json"))
        {
            return "AnimationClip";
        }
        if (name.ends_with(".animator.json"))
        {
            return "AnimatorController";
        }
        if (name.ends_with(".asset.json"))
        {
            return "DataAsset";
        }

        // 取り込み形式判定用の拡張子
        const auto extension =
            Lowercase(LamaPon::PathToUtf8(path.extension()));
        if (extension == ".dds"
            || extension == ".png"
            || extension == ".jpg"
            || extension == ".jpeg"
            || extension == ".bmp"
            || extension == ".tif"
            || extension == ".tiff")
        {
            return "Texture";
        }
        if (extension == ".cmo"
            || extension == ".sdkmesh"
            || extension == ".vbo"
            || extension == ".gltf"
            || extension == ".glb"
            || extension == ".fbx")
        {
            return "Model";
        }
        if (extension == ".wav")
        {
            return "Audio";
        }
        if (extension == ".hlsl")
        {
            return "Shader";
        }
        if (extension == ".cpp")
        {
            return "CppScript";
        }
        return "Default";
    }

    std::string AssetDatabase::CreateGuid() const
    {
        // GUIDに変換する16バイト乱数
        std::array<unsigned char, 16> bytes{};
        // GUID用バイトの乱数生成元
        std::random_device random;
        // 生成・変換するGUIDのバイト
        for (auto& byte : bytes)
        {
            byte = static_cast<unsigned char>(
                random());
        }

        // 小文字十六進数の表示文字
        constexpr char digits[] =
            "0123456789abcdef";
        // アセットの32桁識別子
        std::string guid;
        guid.reserve(32);
        // 生成・変換するGUIDのバイト
        for (const auto byte : bytes)
        {
            guid.push_back(digits[byte >> 4]);
            guid.push_back(digits[byte & 0x0f]);
        }
        return guid;
    }

    std::filesystem::path
        AssetDatabase::FbxDependencyCachePath() const
    {

        // 比較・識別用の小文字パス
        std::wstring key = m_assetRoot.native();
        // パスキーの各文字を小文字にする(character: 変換する文字)。
        std::ranges::transform(
            key,
            key.begin(),
            [](const wchar_t character)
            {
                return static_cast<wchar_t>(
                    std::towlower(character));
            });
        // ルート識別用の64ビットハッシュ
        std::uint64_t hash = 1469598103934665603ull;
        // ハッシュへ混ぜるルートの文字
        for (const auto character : key)
        {
            hash ^= static_cast<std::uint64_t>(character);
            hash *= 1099511628211ull;
        }
        // 絶対ルート由来の保存ファイル名
        wchar_t name[32]{};
        std::swprintf(
            name,
            std::size(name),
            L"%016llx.json",
            static_cast<unsigned long long>(hash));

        // LocalAppData内の依存保存先
        std::filesystem::path directory;
        // OSから確保した保存基点の文字列
        wchar_t* localAppData = nullptr;
        // 環境変数の取得領域文字数
        std::size_t length = 0;
        if (_wdupenv_s(&localAppData, &length, L"LOCALAPPDATA") == 0
            && localAppData != nullptr)
        {
            directory = std::filesystem::path{ localAppData }
                / L"LamaPon" / L"asset-dependency-cache";
            std::free(localAppData);
        }
        if (directory.empty())
        {
            return {};
        }
        return directory / name;
    }

    void AssetDatabase::LoadFbxDependencyCache()
    {
        m_fbxDependencyCacheLoaded = true;
        m_fbxDependencyCache.clear();
        // ルート別の依存キャッシュパス
        const auto path = FbxDependencyCachePath();
        if (path.empty())
        {
            return;
        }
        // 依存キャッシュのファイル操作結果
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error))
        {
            return;
        }
        try
        {
            // 読み込む依存キャッシュのJSON
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                return;
            }
            // 読み込み・書き換え用JSON
            Json document;
            // 読み込み・書き換え用JSON
            input >> document;
            if (!document.is_object())
            {
                return;
            }
            // key: 元FBXの相対パス、value: 保存された解析結果
            for (const auto& [key, value] : document.items())
            {
                if (!value.is_object())
                {
                    continue;
                }
                // 依存キャッシュの一件の結果
                FbxDependencyCacheEntry entry;
                entry.writeTime =
                    value.value("writeTime", std::int64_t{});
                entry.size = value.value("size", std::uint64_t{});
                entry.texturePaths = value.value(
                    "textures",
                    std::vector<std::string>{});
                m_fbxDependencyCache.emplace(key, std::move(entry));
            }
        }
        catch (const std::exception&)
        {
            // キャッシュが不正なら、元FBXから再解析できるよう全結果を捨てる。
            m_fbxDependencyCache.clear();
        }
    }

    void AssetDatabase::SaveFbxDependencyCache() const
    {
        if (!m_fbxDependencyCacheDirty)
        {
            return;
        }
        // ルート別の依存キャッシュパス
        const auto path = FbxDependencyCachePath();
        if (path.empty())
        {
            return;
        }
        try
        {
            // 依存キャッシュのファイル操作結果
            std::error_code error;
            std::filesystem::create_directories(
                path.parent_path(),
                error);
            // 読み込み・書き換え用JSON
            Json document = Json::object();
            // key: 元FBXの相対パス、entry: 保存する解析結果
            for (const auto& [key, entry] : m_fbxDependencyCache)
            {
                document[key] = Json{
                    { "writeTime", entry.writeTime },
                    { "size", entry.size },
                    { "textures", entry.texturePaths },
                };
            }
            // 更新した依存キャッシュの出力
            std::ofstream output(path, std::ios::binary);
            if (!output)
            {
                return;
            }
            output << document.dump();
        }
        catch (const std::exception&)
        {
            // 保存できなくても、次回は元FBXを再解析できる。
        }
    }

    void AssetDatabase::BuildDependencies()
    {
        if (!m_fbxDependencyCacheLoaded)
        {
            LoadFbxDependencyCache();
        }

        // 処理中のアセットレコード
        for (auto& asset : m_assets)
        {
            asset.dependencies.clear();
            asset.dependents.clear();
        }

        // 依存を解析するアセット番号
        for (std::size_t assetIndex = 0;
            assetIndex < m_assets.size();
            ++assetIndex)
        {
            // 処理中のアセットレコード
            auto& asset = m_assets[assetIndex];
            // 取り込み形式判定用の拡張子
            const auto extension =
                Lowercase(LamaPon::PathToUtf8(asset.path.extension()));
            // 外部URIを相対化するglTF形式
            const bool isGltf = extension == ".gltf";
            // FBX依存解析を使う形式
            const bool isFbx = extension == ".fbx";
            if (extension != ".json"
                && !isGltf
                && !isFbx)
            {
                continue;
            }


            // 依存を取得できない理由を警告する(exception: 検出した解析失敗)。
            const auto skipUnreadable =
                [&asset](const std::exception& exception)
                {
                    Logger::Instance().Warning(
                        "アセットを読めないため、依存の一覧から"
                        "外しました（他のアセットには影響しません）: "
                        + PathToUtf8(asset.path)
                        + " — "
                        + exception.what());
                };

            // 重複・自己参照を除くGUID集合
            std::set<std::string> dependencies;
            if (isFbx)
            {
                // 処理するアセットの絶対パス
                const auto absolutePath =
                    m_assetRoot / asset.path;
                // 元FBXの相対パス識別子
                const auto cacheKey = PathToUtf8(asset.path);


                // 更新時刻の取得結果
                std::error_code statusError;
                // 元FBXの更新時刻
                const auto writeTime =
                    std::filesystem::last_write_time(
                        absolutePath,
                        statusError);
                // 元FBXのサイズ取得結果
                std::error_code sizeError;
                // 元FBXのバイト数
                const auto fileSize = std::filesystem::file_size(
                    absolutePath,
                    sizeError);
                // 時刻とサイズを取得できたか
                const bool statusKnown = !statusError && !sizeError;
                // 保存用の更新時刻の刻み
                const auto writeTimeTicks =
                    statusKnown
                        ? static_cast<std::int64_t>(
                            writeTime.time_since_epoch().count())
                        : std::int64_t{};

                // 再利用・再解析した画像パス一覧
                std::vector<std::string>* texturePaths = nullptr;
                // 時刻とサイズの両方が一致する場合だけ、保存した画像パス解析を再利用する。
                if (statusKnown)
                {
                    // 元FBXに対応する既存解析結果
                    const auto cached =
                        m_fbxDependencyCache.find(cacheKey);
                    if (cached != m_fbxDependencyCache.end()
                        && cached->second.writeTime == writeTimeTicks
                        && cached->second.size == fileSize)
                    {
                        texturePaths = &cached->second.texturePaths;
                    }
                }

                // 今回解析した依存画像パス
                std::vector<std::string> scanned;
                if (texturePaths == nullptr)
                {
                    // 時刻・サイズ不一致の元FBX入力
                    std::ifstream stream(
                        absolutePath,
                        std::ios::binary | std::ios::ate);
                    if (!stream)
                    {
                        skipUnreadable(std::runtime_error(
                            "Unable to inspect FBX dependencies"));
                        continue;
                    }
                    // FBX全体のバイト数
                    const auto end = stream.tellg();
                    if (end <= 0)
                    {
                        skipUnreadable(std::runtime_error(
                            "Unable to inspect empty FBX"));
                        continue;
                    }
                    // 依存を再解析するFBXの全内容
                    std::vector<unsigned char> bytes(
                        static_cast<std::size_t>(end));
                    stream.seekg(0);
                    stream.read(
                        reinterpret_cast<char*>(bytes.data()),
                        static_cast<std::streamsize>(
                            bytes.size()));
                    if (!stream)
                    {
                        throw std::runtime_error(
                            "Unable to inspect FBX dependencies: "
                            + PathToUtf8(asset.path));
                    }

                    // 幾何・アニメ・埋込画像を除く設定
                    ufbx_load_opts options{};
                    options.ignore_geometry = true;
                    options.ignore_animation = true;
                    options.ignore_embedded = true;
                    // FBX依存解析の失敗情報
                    ufbx_error error{};
                    // ufbx_free_sceneで解放する結果
                    ufbx_scene* scene = ufbx_load_memory(
                        bytes.data(),
                        bytes.size(),
                        &options,
                        &error);
                    if (scene == nullptr)
                    {
                        skipUnreadable(std::runtime_error(
                            "Unable to inspect FBX dependencies"));
                        continue;
                    }
                    // 参照画像を調べる番号
                    for (std::size_t textureIndex = 0;
                        textureIndex < scene->textures.count;
                        ++textureIndex)
                    {
                        // FBXの参照画像情報
                        const auto* texture =
                            scene->textures.data[textureIndex];
                        // 相対名を優先した画像名
                        const ufbx_string source =
                            texture->relative_filename.length > 0
                                ? texture->relative_filename
                                : texture->filename;
                        if (source.data == nullptr
                            || source.length == 0)
                        {
                            continue;
                        }
                        // FBXの参照画像名のUTF8文字列
                        std::string filename(
                            source.data,
                            source.length);
                        std::ranges::replace(
                            filename,
                            '\\',
                            '/');
                        // 文字列から得た参照先パス
                        auto candidate =
                            PathFromUtf8(filename);
                        if (candidate.is_absolute())
                        {
                            candidate =
                                candidate.lexically_relative(
                                    m_assetRoot);
                        }
                        else
                        {
                            candidate =
                                asset.path.parent_path()
                                / candidate;
                        }
                        scanned.push_back(PathToUtf8(
                            candidate.lexically_normal()));
                    }
                    ufbx_free_scene(scene);

                    if (statusKnown)
                    {
                        // 依存キャッシュの一件の結果
                        FbxDependencyCacheEntry entry;
                        entry.writeTime = writeTimeTicks;
                        entry.size = fileSize;
                        entry.texturePaths = scanned;
                        m_fbxDependencyCache.insert_or_assign(
                            cacheKey,
                            std::move(entry));
                        m_fbxDependencyCacheDirty = true;
                    }
                    texturePaths = &scanned;
                }

                // 画像パスだけを再利用し、参照GUIDは現在の索引から引き直す。
                // 現在GUIDを検索する画像パス
                for (const auto& texturePath : *texturePaths)
                {
                    // パス・GUID索引の検索位置
                    const auto found = m_pathToIndex.find(
                        PathKey(PathFromUtf8(texturePath)));
                    if (found != m_pathToIndex.end()
                        && found->second != assetIndex)
                    {
                        dependencies.emplace(
                            m_assets[found->second].guid);
                    }
                }
                asset.dependencies.assign(
                    dependencies.begin(),
                    dependencies.end());
                continue;
            }

            // 読み込み・書き換え用JSON
            Json document;
            try
            {
                document = ReadJson(m_assetRoot / asset.path);
            }
            // exception: JSONの読取失敗
            catch (const std::exception& exception)
            {
                skipUnreadable(exception);
                continue;
            }
            // 子要素を再帰走査し依存を集める(value: 参照を調べるJSON値)。
            const std::function<void(const Json&)>
                visit = [&](const Json& value)
                {
                    if (value.is_string())
                    {
                        // GUID・パス候補のJSON文字列
                        const auto& text =
                            value.get_ref<
                                const std::string&>();
                        if (text.empty())
                        {
                            return;
                        }
                        if (IsValidGuid(text))
                        {
                            // GUID参照先の索引位置
                            const auto guidFound =
                                m_guidToIndex.find(text);
                            if (guidFound
                                    != m_guidToIndex.end()
                                && guidFound->second
                                    != assetIndex)
                            {
                                dependencies.emplace(text);
                            }
                            return;
                        }
                        // 文字列から得た参照先パス
                        const auto candidate =
                            (isGltf
                                ? asset.path.parent_path()
                                    / PathFromUtf8(text)
                                : PathFromUtf8(text))
                            .lexically_normal();
                        if (candidate.is_absolute())
                        {
                            return;
                        }
                        // パス・GUID索引の検索位置
                        const auto found =
                            m_pathToIndex.find(
                                PathKey(candidate));
                        if (found != m_pathToIndex.end()
                            && found->second != assetIndex)
                        {
                            dependencies.emplace(
                                m_assets[
                                    found->second].guid);
                        }
                        return;
                    }
                    if (value.is_array())
                    {
                        // 再帰処理する子のJSON値
                        for (const auto& child : value)
                        {
                            visit(child);
                        }
                        return;
                    }
                    if (value.is_object())
                    {
                        // key: 走査用の項目名、child: 参照を探す値
                        for (const auto& [key, child] :
                            value.items())
                        {
                            static_cast<void>(key);
                            visit(child);
                        }
                    }
                };
            visit(document);
            asset.dependencies.assign(
                dependencies.begin(),
                dependencies.end());
        }

        // 処理中のアセットレコード
        for (const auto& asset : m_assets)
        {
            // 逆向き参照を登録するGUID
            for (const auto& dependency :
                asset.dependencies)
            {
                // パス・GUID索引の検索位置
                const auto found =
                    m_guidToIndex.find(dependency);
                if (found != m_guidToIndex.end())
                {
                    m_assets[found->second]
                        .dependents.push_back(asset.guid);
                }
            }
        }
        // 処理中のアセットレコード
        for (auto& asset : m_assets)
        {
            std::ranges::sort(asset.dependents);
        }
    }
}
