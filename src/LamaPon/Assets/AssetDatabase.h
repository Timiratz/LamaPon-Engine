#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace LamaPon
{
    struct AssetRecord final
    {
        std::string guid;
        std::filesystem::path path;
        std::filesystem::path metaPath;
        std::string importer;
        std::vector<std::string> dependencies;
        std::vector<std::string> dependents;
    };

    struct AssetDatabaseRefreshResult final
    {
        std::size_t assetCount{};
        std::size_t createdMetaCount{};
        std::size_t dependencyCount{};
    };

    struct AssetReferenceRemapResult final
    {
        std::size_t fileCount{};
        std::size_t referenceCount{};
    };

    class AssetDatabase final
    {
    public:
        void SetAssetRoot(std::filesystem::path assetRoot);
        [[nodiscard]] const std::filesystem::path&
            AssetRoot() const noexcept
        {
            return m_assetRoot;
        }

        [[nodiscard]] AssetDatabaseRefreshResult Refresh(
            bool createMissingMeta = true);
        // 今のアセットルートに対して既にRefreshが済んでいるかどうか。
        // 起動直後にエディターが同じ走査を繰り返さないための判定です。
        [[nodiscard]] bool HasRefreshed() const noexcept
        {
            return m_refreshed;
        }
        [[nodiscard]] const std::vector<AssetRecord>&
            Assets() const noexcept
        {
            return m_assets;
        }
        [[nodiscard]] const AssetRecord* FindByPath(
            const std::filesystem::path& path) const noexcept;
        [[nodiscard]] const AssetRecord* FindByGuid(
            std::string_view guid) const noexcept;
        [[nodiscard]] std::string GuidForPath(
            const std::filesystem::path& path) const;
        [[nodiscard]] std::filesystem::path ResolveGuid(
            std::string_view guid,
            std::filesystem::path fallback = {}) const;

        [[nodiscard]] AssetReferenceRemapResult
            RemapJsonReferences(
                const std::filesystem::path& oldPath,
                const std::filesystem::path& newPath,
                bool includeChildren = false);

        [[nodiscard]] static bool IsMetaFile(
            const std::filesystem::path& path) noexcept;
        [[nodiscard]] static std::filesystem::path MetaPathFor(
            const std::filesystem::path& assetPath);
        [[nodiscard]] static bool IsValidGuid(
            std::string_view guid) noexcept;

    private:
        [[nodiscard]] std::filesystem::path RelativePath(
            const std::filesystem::path& path) const;
        [[nodiscard]] static std::wstring PathKey(
            const std::filesystem::path& path);
        [[nodiscard]] static std::string ImporterFor(
            const std::filesystem::path& path);
        [[nodiscard]] std::string CreateGuid() const;
        void BuildDependencies();

        // FBXの依存（参照しているテクスチャ）を取り出すにはファイル全体を
        // 読んでパースする必要があります。プロジェクトがネットワーク
        // ドライブにあると、これがRefreshのたびに数百MBの読み込みになり、
        // プロジェクトを開くだけで数十秒かかります。更新時刻とサイズが
        // 変わっていなければ結果は同じなので、ローカルディスクへ覚えて
        // おいて読み直しを省きます。
        struct FbxDependencyCacheEntry final
        {
            std::int64_t writeTime{};
            std::uint64_t size{};
            std::vector<std::string> texturePaths;
        };
        [[nodiscard]] std::filesystem::path
            FbxDependencyCachePath() const;
        void LoadFbxDependencyCache();
        void SaveFbxDependencyCache() const;

        std::filesystem::path m_assetRoot;
        std::vector<AssetRecord> m_assets;
        std::unordered_map<std::wstring, std::size_t>
            m_pathToIndex;
        std::unordered_map<std::string, std::size_t>
            m_guidToIndex;
        std::unordered_map<std::string, FbxDependencyCacheEntry>
            m_fbxDependencyCache;
        bool m_fbxDependencyCacheLoaded{};
        bool m_fbxDependencyCacheDirty{};
        bool m_refreshed{};
    };
}
