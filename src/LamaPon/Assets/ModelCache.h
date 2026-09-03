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
    // モデルのインポートキャッシュ。
    //
    // FBX/glTFの読み込みは、パース（ufbx/cgltf）→頂点の組み立て→
    // 静的パーツの結合→GPUリソース生成、と続く長い仕事で、結果は
    // 毎回同じなのに開くたびにやり直していました。ここでは
    // 「GPUへ渡す直前のCPU側の材料」を丸ごとファイルへ残し、
    // 2回目以降はパースと組み立てを全部飛ばします。
    // shader-cache／texture-cacheと同じ構図のモデル版です。
    //
    // 外部依存（.gltfが参照する.bin、外部テクスチャ、.meta）は
    // パス＋内容ハッシュで一緒に記録し、読み込み時に照合します。
    // どれか1つでも変わっていたら丸ごと作り直しへ落ちるので、
    // 「テクスチャだけ差し替えたのに古い絵が出る」ことはありません。

    // インポーターが読み込みの過程で「組み立て直しに必要な材料」を
    // 登録していく記録器。登録は追加式なので、既存のインポーターの
    // 流れを変えずに差し込めます。
    class Recorder final
    {
    public:
        // 画像。viewは後でプリミティブのスロットから引くための鍵です
        // （インポーターはSRVしか持ち歩かないため）。
        void RegisterEmbeddedImage(
            ID3D11ShaderResourceView* view,
            std::span<const std::uint8_t> bytes,
            bool isDds,
            bool hasTransparency,
            TextureLoader::TextureUsage usage
                = TextureLoader::TextureUsage::Color);
        // 外部ファイルの画像はバイト列を保存せず、パス＋ハッシュだけ
        // 記録します（読み込み時にどうせ検証のため読み直すので、
        // キャッシュへ複製すると容量が倍になるだけです）。
        void RegisterExternalImage(
            ID3D11ShaderResourceView* view,
            const std::filesystem::path& path,
            std::span<const std::uint8_t> bytes,
            bool isDds,
            bool hasTransparency,
            TextureLoader::TextureUsage usage
                = TextureLoader::TextureUsage::Color);
        // 画像以外の依存ファイル（.metaや.bin）。「存在しない」ことも
        // 記録します（後からファイルが現れたら作り直すため）。
        void RegisterDependency(
            const std::filesystem::path& path,
            bool exists,
            std::span<const std::uint8_t> bytes);
        // プリミティブの幾何。model->primitivesへemplace_backするのと
        // 同じ順で呼びます。indicesがnullなら0..N-1の連番（FBXは
        // 展開済みなので添字列を持ちません）。
        void AddGeometry(
            const void* vertices,
            std::size_t vertexCount,
            std::size_t vertexStride,
            const std::uint32_t* indices,
            std::size_t indexCount);

        // ここから下はStore/TryLoadが読む内部表現です。
        struct Image final
        {
            std::vector<std::uint8_t> bytes;
            std::filesystem::path externalPath;
            std::uint64_t externalHash{};
            bool external{};
            bool isDds{};
            bool hasTransparency{};
            // 用途。圧縮フォーマットが変わるので、キャッシュから
            // 組み立て直すときも同じ値で作らないと絵が変わります。
            TextureLoader::TextureUsage usage{
                TextureLoader::TextureUsage::Color };
        };
        struct Geometry final
        {
            std::vector<std::uint8_t> vertexBytes;
            std::uint32_t vertexStride{};
            std::vector<std::uint32_t> indices;
        };
        struct Dependency final
        {
            std::filesystem::path path;
            bool exists{};
            std::uint64_t hash{};
        };

        [[nodiscard]] std::int32_t ImageIndexFor(
            ID3D11ShaderResourceView* view) const noexcept;

        std::vector<Image> images;
        std::vector<Geometry> geometries;
        std::vector<Dependency> dependencies;

    private:
        std::unordered_map<
            ID3D11ShaderResourceView*,
            std::int32_t> m_imageBySrv;
    };

    // 置き場所（既定は%LOCALAPPDATA%\LamaPon\model-cache）。
    [[nodiscard]] std::filesystem::path CacheDirectory();

    // テスト用の差し替え。空で既定へ戻ります。
    void SetCacheDirectoryOverride(std::filesystem::path directory);

    // 鍵はモデルファイルの内容ハッシュ。importerKindはFBXとglTFで
    // 別の値を渡します（同じバイト列でも別形式なら別物のため）。
    [[nodiscard]] std::uint64_t ComputeKey(
        std::span<const std::uint8_t> sourceBytes,
        std::uint32_t importerKind) noexcept;

    // 読み込み。無い・壊れている・依存が変わっているときはnullptr
    // （呼ぶ側は普通にインポートすればよい）。
    [[nodiscard]] std::shared_ptr<SkeletalModel> TryLoad(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        AssetManager& assets,
        std::uint64_t key);

    // 保存。失敗しても何も起きません（キャッシュはあくまで高速化）。
    void Store(
        std::uint64_t key,
        const SkeletalModel& model,
        const Recorder& recorder) noexcept;
}
