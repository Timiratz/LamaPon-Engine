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

    bool IsSafeRelativePath(
        const std::filesystem::path& path)
    {
        if (path.empty() || path.is_absolute())
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

    // アセットとして扱わないファイル。内部の一時ファイルと、
    // プロジェクト移行が組み込みアセットを更新するときに退避した
    // バックアップ（`<名前>.bak`）が対象です。バックアップは
    // 読み込む対象ではないため、Asset Browserにも出さず、.metaも
    // 作らず、ゲームの書き出しにも含めません（ファイル自体は
    // 復元できるように残します）。
    bool IsTemporaryAssetFile(
        const std::filesystem::path& path)
    {
        const auto name = path.filename().wstring();
        return name.find(L".lamapon-delete")
                != std::wstring::npos
            || name.ends_with(L".lamapon-remap.tmp")
            || name.ends_with(L".bak");
    }

    std::string Lowercase(std::string value)
    {
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

    void WriteJsonAtomically(
        const std::filesystem::path& path,
        const Json& document)
    {
        const auto temporaryPath =
            path.wstring() + L".lamapon-remap.tmp";
        {
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
            const DWORD error = GetLastError();
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

    Json ReadJson(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error(
                "Could not read JSON asset: "
                + LamaPon::PathToUtf8(path));
        }
        Json document;
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
        // 依存キャッシュはアセットルートごとに別ファイルなので、
        // next Refresh で読み直させます。
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

        AssetDatabaseRefreshResult result;
        if (!std::filesystem::is_directory(
                m_assetRoot))
        {
            return result;
        }

        std::vector<std::filesystem::path> paths;
        std::error_code iteratorError;
        const auto options =
            std::filesystem::directory_options::
                skip_permission_denied;
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

        std::set<std::string> usedGuids;
        for (const auto& absolutePath : paths)
        {
            const auto relativePath =
                absolutePath.lexically_relative(
                    m_assetRoot);
            if (!IsSafeRelativePath(relativePath))
            {
                continue;
            }

            const auto metaAbsolute =
                MetaPathFor(absolutePath);
            std::string guid;
            if (std::filesystem::is_regular_file(
                    metaAbsolute))
            {
                // 壊れた.metaは、そのアセットを**飛ばすだけ**に
                // します。ここで投げると走査ごと失敗し、.meta1つで
                // プロジェクトが開けなくなります（＝直す手段も
                // 無くなります）。
                //
                // 勝手に作り直しはしません。新しいGUIDになると、
                // 他のアセットからの参照が**黙って切れます**。
                // 飛ばしておけば、ユーザーが.metaを直すか消すかを
                // 選べます（消せば次回作り直されます）。
                std::string problem;
                try
                {
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
                catch (const std::exception& exception)
                {
                    problem = exception.what();
                }
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
                        // Read-only game packages can still use
                        // an in-memory GUID table.
                    }
                }
            }

            if (!usedGuids.emplace(guid).second)
            {
                throw std::runtime_error(
                    "Duplicate asset GUID detected: "
                    + guid);
            }

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
            const auto relative = RelativePath(path);
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
        const auto found =
            m_guidToIndex.find(std::string(guid));
        return found != m_guidToIndex.end()
            ? &m_assets[found->second]
            : nullptr;
    }

    std::string AssetDatabase::GuidForPath(
        const std::filesystem::path& path) const
    {
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
        const auto oldRelative =
            RelativePath(oldPath);
        const auto newRelative =
            RelativePath(newPath);
        if (!IsSafeRelativePath(oldRelative)
            || !IsSafeRelativePath(newRelative))
        {
            throw std::invalid_argument(
                "Asset remap paths must be safe and relative.");
        }

        AssetReferenceRemapResult result;
        const auto remapString =
            [&oldRelative, &newRelative, includeChildren](
                const std::string& value)
                -> std::pair<std::string, bool>
            {
                if (value.empty())
                {
                    return { value, false };
                }
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

        for (const auto& asset : m_assets)
        {
            if (Lowercase(LamaPon::PathToUtf8(asset.path.extension()))
                    != ".json")
            {
                continue;
            }
            const auto absolutePath =
                m_assetRoot / asset.path;
            auto document = ReadJson(absolutePath);
            std::size_t changedReferences{};
            const std::function<void(Json&)>
                visit = [&](Json& value)
                {
                    if (value.is_string())
                    {
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
                        for (auto& child : value)
                        {
                            visit(child);
                        }
                        return;
                    }
                    if (value.is_object())
                    {
                        for (auto& [key, child] :
                            value.items())
                        {
                            static_cast<void>(key);
                            visit(child);
                        }
                    }
                };
            visit(document);
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
        const auto normalized =
            path.lexically_normal();
        if (!normalized.is_absolute())
        {
            return normalized;
        }
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
        auto key = path.lexically_normal()
            .generic_wstring();
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
        std::array<unsigned char, 16> bytes{};
        std::random_device random;
        for (auto& byte : bytes)
        {
            byte = static_cast<unsigned char>(
                random());
        }

        constexpr char digits[] =
            "0123456789abcdef";
        std::string guid;
        guid.reserve(32);
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
        // プロジェクトごとに1ファイル。assetRootのフルパスから鍵を作るので、
        // 別プロジェクトや別マウント先のキャッシュと混ざりません。
        std::wstring key = m_assetRoot.native();
        std::ranges::transform(
            key,
            key.begin(),
            [](const wchar_t character)
            {
                return static_cast<wchar_t>(
                    std::towlower(character));
            });
        std::uint64_t hash = 1469598103934665603ull;
        for (const auto character : key)
        {
            hash ^= static_cast<std::uint64_t>(character);
            hash *= 1099511628211ull;
        }
        wchar_t name[32]{};
        std::swprintf(
            name,
            std::size(name),
            L"%016llx.json",
            static_cast<unsigned long long>(hash));

        std::filesystem::path directory;
        wchar_t* localAppData = nullptr;
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
        const auto path = FbxDependencyCachePath();
        if (path.empty())
        {
            return;
        }
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error))
        {
            return;
        }
        try
        {
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                return;
            }
            Json document;
            input >> document;
            if (!document.is_object())
            {
                return;
            }
            for (const auto& [key, value] : document.items())
            {
                if (!value.is_object())
                {
                    continue;
                }
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
            // 壊れたキャッシュは無いものとして扱います。読み直せば
            // 同じ結果になるので、ここで失敗させる理由がありません。
            m_fbxDependencyCache.clear();
        }
    }

    void AssetDatabase::SaveFbxDependencyCache() const
    {
        if (!m_fbxDependencyCacheDirty)
        {
            return;
        }
        const auto path = FbxDependencyCachePath();
        if (path.empty())
        {
            return;
        }
        try
        {
            std::error_code error;
            std::filesystem::create_directories(
                path.parent_path(),
                error);
            Json document = Json::object();
            for (const auto& [key, entry] : m_fbxDependencyCache)
            {
                document[key] = Json{
                    { "writeTime", entry.writeTime },
                    { "size", entry.size },
                    { "textures", entry.texturePaths },
                };
            }
            std::ofstream output(path, std::ios::binary);
            if (!output)
            {
                return;
            }
            output << document.dump();
        }
        catch (const std::exception&)
        {
            // 保存できなくても次回読み直すだけです。
        }
    }

    void AssetDatabase::BuildDependencies()
    {
        if (!m_fbxDependencyCacheLoaded)
        {
            LoadFbxDependencyCache();
        }

        for (auto& asset : m_assets)
        {
            asset.dependencies.clear();
            asset.dependents.clear();
        }

        for (std::size_t assetIndex = 0;
            assetIndex < m_assets.size();
            ++assetIndex)
        {
            auto& asset = m_assets[assetIndex];
            const auto extension =
                Lowercase(LamaPon::PathToUtf8(asset.path.extension()));
            const bool isGltf = extension == ".gltf";
            const bool isFbx = extension == ".fbx";
            if (extension != ".json"
                && !isGltf
                && !isFbx)
            {
                continue;
            }

            // 依存の一覧を作れないアセットは、**飛ばすだけ**にします。
            //
            // ここで投げると、Refreshごと——つまり
            // AssetManager::SetAssetRootごと——失敗し、**壊れた
            // ファイルが1つあるだけでプロジェクトが開けなくなります**。
            // 開けないということは、その1つを直す手段も無いという
            // ことです。依存の一覧は「何を作り直すか」を決めるための
            // もので、1件欠けても他のアセットは動きます。
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

            std::set<std::string> dependencies;
            if (isFbx)
            {
                const auto absolutePath =
                    m_assetRoot / asset.path;
                const auto cacheKey = PathToUtf8(asset.path);

                // 更新時刻とサイズが前回と同じなら、参照している
                // テクスチャも同じです。ネットワークドライブでは
                // ここでのstat 1回と全バイト読みの差が非常に大きいので、
                // 先に安いほうで判定します。
                std::error_code statusError;
                const auto writeTime =
                    std::filesystem::last_write_time(
                        absolutePath,
                        statusError);
                std::error_code sizeError;
                const auto fileSize = std::filesystem::file_size(
                    absolutePath,
                    sizeError);
                const bool statusKnown = !statusError && !sizeError;
                const auto writeTimeTicks =
                    statusKnown
                        ? static_cast<std::int64_t>(
                            writeTime.time_since_epoch().count())
                        : std::int64_t{};

                std::vector<std::string>* texturePaths = nullptr;
                if (statusKnown)
                {
                    const auto cached =
                        m_fbxDependencyCache.find(cacheKey);
                    if (cached != m_fbxDependencyCache.end()
                        && cached->second.writeTime == writeTimeTicks
                        && cached->second.size == fileSize)
                    {
                        texturePaths = &cached->second.texturePaths;
                    }
                }

                std::vector<std::string> scanned;
                if (texturePaths == nullptr)
                {
                    std::ifstream stream(
                        absolutePath,
                        std::ios::binary | std::ios::ate);
                    if (!stream)
                    {
                        skipUnreadable(std::runtime_error(
                            "Unable to inspect FBX dependencies"));
                        continue;
                    }
                    const auto end = stream.tellg();
                    if (end <= 0)
                    {
                        skipUnreadable(std::runtime_error(
                            "Unable to inspect empty FBX"));
                        continue;
                    }
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

                    ufbx_load_opts options{};
                    options.ignore_geometry = true;
                    options.ignore_animation = true;
                    options.ignore_embedded = true;
                    ufbx_error error{};
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
                    for (std::size_t textureIndex = 0;
                        textureIndex < scene->textures.count;
                        ++textureIndex)
                    {
                        const auto* texture =
                            scene->textures.data[textureIndex];
                        const ufbx_string source =
                            texture->relative_filename.length > 0
                                ? texture->relative_filename
                                : texture->filename;
                        if (source.data == nullptr
                            || source.length == 0)
                        {
                            continue;
                        }
                        std::string filename(
                            source.data,
                            source.length);
                        std::ranges::replace(
                            filename,
                            '\\',
                            '/');
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

                // GUIDはデータベースの今の状態で引き直します。覚えて
                // おくのはパスだけにして、アセットを入れ替えたときに
                // 古いGUIDが残らないようにしています。
                for (const auto& texturePath : *texturePaths)
                {
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

            Json document;
            try
            {
                document = ReadJson(m_assetRoot / asset.path);
            }
            catch (const std::exception& exception)
            {
                skipUnreadable(exception);
                continue;
            }
            const std::function<void(const Json&)>
                visit = [&](const Json& value)
                {
                    if (value.is_string())
                    {
                        const auto& text =
                            value.get_ref<
                                const std::string&>();
                        if (text.empty())
                        {
                            return;
                        }
                        if (IsValidGuid(text))
                        {
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
                        for (const auto& child : value)
                        {
                            visit(child);
                        }
                        return;
                    }
                    if (value.is_object())
                    {
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

        for (const auto& asset : m_assets)
        {
            for (const auto& dependency :
                asset.dependencies)
            {
                const auto found =
                    m_guidToIndex.find(dependency);
                if (found != m_guidToIndex.end())
                {
                    m_assets[found->second]
                        .dependents.push_back(asset.guid);
                }
            }
        }
        for (auto& asset : m_assets)
        {
            std::ranges::sort(asset.dependents);
        }
    }
}
