#pragma once

#include "LamaPon/Assets/TextureLoader.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace LamaPon
{
    class AssetManager;
    class SkeletalModel;
}

namespace LamaPon::ModelCache
{
    // 依存ファイルの存在状態と内容を照合し、FBX/glTFのCPUモデルを再利用する。

    // インポーターからキャッシュ復元に必要な画像・幾何・依存情報を集める。
    class Recorder final
    {
    public:
        // 内蔵画像のバイト列を記録する(view: 画像番号の照合キー, bytes: 元画像のバイト列, isDds: DDS形式か, hasTransparency: 透過の有無, usage: 復元時も使う画像用途)。
        // ビューは番号の照合にだけ使い、COM参照を保持しない。
        void RegisterEmbeddedImage(
            ID3D11ShaderResourceView* view,
            std::span<const std::uint8_t> bytes,
            bool isDds,
            bool hasTransparency,
            TextureLoader::TextureUsage usage
                = TextureLoader::TextureUsage::Color);
        // 外部画像のパスと内容ハッシュを記録する(view: 画像番号の照合キー, path: 元画像のパス, bytes: ハッシュ対象のバイト列, isDds: DDS形式か, hasTransparency: 透過の有無, usage: 復元時も使う画像用途)。
        void RegisterExternalImage(
            ID3D11ShaderResourceView* view,
            const std::filesystem::path& path,
            std::span<const std::uint8_t> bytes,
            bool isDds,
            bool hasTransparency,
            TextureLoader::TextureUsage usage
                = TextureLoader::TextureUsage::Color);
        // 依存先の存在状態と内容ハッシュを記録する(path: 依存ファイルのパス, exists: 記録時に存在するか, bytes: 存在時のファイル内容)。
        void RegisterDependency(
            const std::filesystem::path& path,
            bool exists,
            std::span<const std::uint8_t> bytes);
        // 描画部分と同じ順に幾何をコピーする(vertices: 元の頂点列, vertexCount: 頂点数, vertexStride: 1頂点のバイト数, indices: 任意の索引列, indexCount: 索引数)。
        // indicesが空ポインターなら、復元時は描画部分の索引数に従う連番を使う。
        void AddGeometry(
            const void* vertices,
            std::size_t vertexCount,
            std::size_t vertexStride,
            const std::uint32_t* indices,
            std::size_t indexCount);


        struct Image final
        {
            // 内蔵画像の元バイト列
            std::vector<std::uint8_t> bytes;
            // 外部画像の元パス
            std::filesystem::path externalPath;
            // 外部画像の内容ハッシュ
            std::uint64_t externalHash{};
            // 外部画像参照か
            bool external{};
            // DDS形式か
            bool isDds{};
            // 画像の透過の有無
            bool hasTransparency{};
            // 復元にも使う画像用途
            // 用途によって圧縮形式が変わるため、復元時も記録した用途を使う。
            TextureLoader::TextureUsage usage{
                TextureLoader::TextureUsage::Color };
        };
        struct Geometry final
        {
            // 共通形式の頂点バイト列
            std::vector<std::uint8_t> vertexBytes;
            // 1頂点のバイト数
            std::uint32_t vertexStride{};
            // 索引列、空は連番で復元
            std::vector<std::uint32_t> indices;
        };
        struct Dependency final
        {
            // 照合する依存先のパス
            std::filesystem::path path;
            // 記録時の依存先の存在状態
            bool exists{};
            // 存在時の内容ハッシュ
            std::uint64_t hash{};
        };

        // 照合キーの画像番号を返し、未登録には−1を返す(view: 登録時の画像ビュー)。
        [[nodiscard]] std::int32_t ImageIndexFor(
            ID3D11ShaderResourceView* view) const noexcept;

        // 登録順の画像情報
        std::vector<Image> images;
        // 描画部分順の幾何情報
        std::vector<Geometry> geometries;
        // 照合する依存ファイル情報
        std::vector<Dependency> dependencies;

    private:
        // 画像ビューから最初の画像番号
        std::unordered_map<
            ID3D11ShaderResourceView*,
            std::int32_t> m_imageBySrv;
    };

    // 差し替え先または既定のモデルキャッシュ保存先を返す。
    [[nodiscard]] std::filesystem::path CacheDirectory();

    // キャッシュ保存先を差し替え、空パスなら既定へ戻す(directory: 差し替え先)。
    void SetCacheDirectoryOverride(std::filesystem::path directory);

    // モデル内容・形式・キャッシュ版・頂点サイズからキーを作る(sourceBytes: 元モデルのバイト列, importerKind: インポーターの識別値)。
    [[nodiscard]] std::uint64_t ComputeKey(
        std::span<const std::uint8_t> sourceBytes,
        std::uint32_t importerKind) noexcept;

    // 有効なキャッシュからD3D11モデルを復元し、失敗時は空を返す(device: 復元先の有効なデバイス, context: 互換用の未使用引数, assets: 依存確認と画像復元の取得元, key: モデルのキャッシュキー)。
    [[nodiscard]] std::shared_ptr<SkeletalModel> TryLoad(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        AssetManager& assets,
        std::uint64_t key);

    // CPUモデルと記録した幾何を保存し、保存失敗は無視する(key: モデルのキャッシュキー, model: 保存するモデル, recorder: 同じ順に登録した復元情報)。
    void Store(
        std::uint64_t key,
        const SkeletalModel& model,
        const Recorder& recorder) noexcept;
}
