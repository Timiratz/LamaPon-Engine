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
        // 大小を区別する32桁GUID
        std::string guid;
        // アセットルート内の相対パス
        std::filesystem::path path;
        // ルート内の相対metaパス
        std::filesystem::path metaPath;
        // 取り込み形式の識別名
        std::string importer;
        // 参照するアセットのGUID一覧
        std::vector<std::string> dependencies;
        // 参照元アセットのGUID一覧
        std::vector<std::string> dependents;
    };

    struct AssetDatabaseRefreshResult final
    {
        // 走査で登録したアセット数
        std::size_t assetCount{};
        // 新規metaの保存成功数
        std::size_t createdMetaCount{};
        // 登録した依存参照の総数
        std::size_t dependencyCount{};
    };

    struct AssetReferenceRemapResult final
    {
        // 置換内容を保存したJSON数
        std::size_t fileCount{};
        // 一致して置換した文字列数
        std::size_t referenceCount{};
    };

    class AssetDatabase final
    {
    public:
        // 絶対ルートを設定して索引と走査・依存キャッシュ状態を初期化する(assetRoot: 新しいアセットルート)。
        void SetAssetRoot(std::filesystem::path assetRoot);
        // 現在の絶対アセットルートを借用して返す。
        [[nodiscard]] const std::filesystem::path&
            AssetRoot() const noexcept
        {
            return m_assetRoot;
        }

        // 索引と依存を再構築し、meta未保存でも生成GUIDをメモリーに保持する(createMissingMeta: 不在metaの保存指定)。
        [[nodiscard]] AssetDatabaseRefreshResult Refresh(
            bool createMissingMeta = true);
        // 現在ルートでの正常走査完了の記録を返す。
        [[nodiscard]] bool HasRefreshed() const noexcept
        {
            return m_refreshed;
        }
        // 次の走査・ルート変更まで使うアセット一覧を借用して返す。
        [[nodiscard]] const std::vector<AssetRecord>&
            Assets() const noexcept
        {
            return m_assets;
        }
        // 走査・ルート変更まで有効なレコードを借用し、不在はnullptrにする(path: 相対またはルート内絶対パス)。
        [[nodiscard]] const AssetRecord* FindByPath(
            const std::filesystem::path& path) const noexcept;
        // 走査・ルート変更まで有効なレコードを借用し、不在はnullptrにする(guid: 大小を区別する識別子)。
        [[nodiscard]] const AssetRecord* FindByGuid(
            std::string_view guid) const noexcept;
        // パスに対応するGUIDを返し、不在は空文字列にする(path: 相対またはルート内絶対パス)。
        [[nodiscard]] std::string GuidForPath(
            const std::filesystem::path& path) const;
        // GUIDの登録パスを返し、不在は代替パスにする(guid: 大小を区別する識別子, fallback: 不在時に返すパス)。
        [[nodiscard]] std::filesystem::path ResolveGuid(
            std::string_view guid,
            std::filesystem::path fallback = {}) const;

        // 全JSON文字列のパス一致をファイル単位で保存し再走査する(oldPath: 置換前のパス, newPath: 置換後のパス, includeChildren: 配下のパスも置換する指定)。
        [[nodiscard]] AssetReferenceRemapResult
            RemapJsonReferences(
                const std::filesystem::path& oldPath,
                const std::filesystem::path& newPath,
                bool includeChildren = false);

        // 拡張子が.metaか大小を区別せず調べる(path: 判定するパス)。
        [[nodiscard]] static bool IsMetaFile(
            const std::filesystem::path& path) noexcept;
        // 元パスの拡張子を含む末尾に.metaを付ける(assetPath: アセットのパス)。
        [[nodiscard]] static std::filesystem::path MetaPathFor(
            const std::filesystem::path& assetPath);
        // 32文字の十六進数か検証する(guid: 検証する識別子)。
        [[nodiscard]] static bool IsValidGuid(
            std::string_view guid) noexcept;

    private:
        // ルート内絶対パスを相対化し、相対入力は字句正規化する(path: 変換するパス)。
        [[nodiscard]] std::filesystem::path RelativePath(
            const std::filesystem::path& path) const;
        // 大小と区切り形式を統一したパス索引キーを得る(path: 変換する相対パス)。
        [[nodiscard]] static std::wstring PathKey(
            const std::filesystem::path& path);
        // 複合拡張子・通常拡張子から取り込み種別を選ぶ(path: 判定するパス)。
        [[nodiscard]] static std::string ImporterFor(
            const std::filesystem::path& path);
        // 16バイトの乱数から小文字32桁の識別子を作る。
        [[nodiscard]] std::string CreateGuid() const;
        // JSON・glTF・FBXの参照を集め、逆向きの依存関係も構築する。
        void BuildDependencies();

        // FBXの更新時刻とサイズが同じ間は、依存テクスチャーのパス解析を再利用する。
        struct FbxDependencyCacheEntry final
        {
            // 元ファイル更新時刻の刻み
            std::int64_t writeTime{};
            // 元ファイルのバイト数
            std::uint64_t size{};
            // ルート基準の依存画像パス一覧
            std::vector<std::string> texturePaths;
        };
        // 絶対ルート由来のハッシュでプロジェクト別の保存先を得る。
        [[nodiscard]] std::filesystem::path
            FbxDependencyCachePath() const;
        // 依存キャッシュの読み込みを試み、失敗時は空のキャッシュで継続する。
        void LoadFbxDependencyCache();
        // 変更した依存キャッシュを保存し、保存失敗は走査を妨げない。
        void SaveFbxDependencyCache() const;

        // 絶対パスのアセットルート
        std::filesystem::path m_assetRoot;
        // 所有するアセットレコード一覧
        std::vector<AssetRecord> m_assets;
        // 小文字の相対パスから一覧位置
        std::unordered_map<std::wstring, std::size_t>
            m_pathToIndex;
        // GUIDからアセットの一覧位置
        std::unordered_map<std::string, std::size_t>
            m_guidToIndex;
        // 相対FBXパスごとの依存解析結果
        std::unordered_map<std::string, FbxDependencyCacheEntry>
            m_fbxDependencyCache;
        // 現在ルートでの読込試行済み
        bool m_fbxDependencyCacheLoaded{};
        // 保存対象のキャッシュ変更記録
        bool m_fbxDependencyCacheDirty{};
        // 現在ルートでの正常走査完了
        bool m_refreshed{};
    };
}
