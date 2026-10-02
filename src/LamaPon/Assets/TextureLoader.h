#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <span>
#include <vector>

namespace LamaPon::TextureLoader
{
    // 行間パディング無しのRGBA8画素をCPUに保持する。
    struct CpuImage final
    {
        // 幅（画素）
        std::uint32_t width{};
        // 高さ（画素）
        std::uint32_t height{};
        // 行単位のRGBA8画素列
        std::vector<std::uint8_t> pixels;
    };

    // COMを初期化してWIC画像の先頭フレームをRGBA8へ復号する(bytes: 呼出中に保持する元画像の列)。
    [[nodiscard]] CpuImage DecodeImageBytes(
        std::span<const std::uint8_t> bytes);

    // 入力を先頭に置き、2×2平均で1×1までのミップを作る(base: 正寸法とRGBA8列を持つ元画像)。
    [[nodiscard]] std::vector<CpuImage> GenerateMipChain(
        CpuImage base);

    // アルファが250未満の画素があるか調べる(image: RGBA8画素列の判定対象)。
    [[nodiscard]] bool HasTransparentPixels(
        const CpuImage& image) noexcept;

    // 画像の用途で圧縮形式を選び、法線のZはシェーダーで復元する。
    enum class TextureUsage
    {
        // 基本色・発光、透過判定時はBC3、他はBC1
        Color,
        // 法線、RGをBC5へ保存
        NormalMap,
        // 金属度・粗さ・遮蔽、アルファを使わずBC1
        DataMap,
    };

    // RGBA8画像を4×4の不透明BC1ブロック列へ圧縮する(image: 正寸法と全RGBA画素を持つ画像)。
    [[nodiscard]] std::vector<std::uint8_t> CompressBC1(
        const CpuImage& image);
    // RGBA8画像を4×4のアルファ付きBC3ブロック列へ圧縮する(image: 正寸法と全RGBA画素を持つ画像)。
    [[nodiscard]] std::vector<std::uint8_t> CompressBC3(
        const CpuImage& image);
    // RGBA8のR・Gを4×4のBC5ブロック列へ圧縮する(image: 正寸法と全RGBA画素を持つ画像)。
    [[nodiscard]] std::vector<std::uint8_t> CompressBC5(
        const CpuImage& image);

    // 全ミップを不変テクスチャとビューへ一括転送する(device: 作成先デバイス, mips: 大きい順の有効なRGBA8画像列, compress: BC圧縮を使うか, usage: 画像用途)。
    [[nodiscard]]
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
        CreateTexture(
            ID3D11Device* device,
            const std::vector<CpuImage>& mips,
            bool compress,
            TextureUsage usage = TextureUsage::Color);

    // 先頭の寸法と用途からRGBA8またはBC形式を選ぶ(mips: 大きい順のRGBA8画像列, compress: BC圧縮を使うか, usage: 画像用途)。
    [[nodiscard]] DXGI_FORMAT ChooseTextureFormat(
        const std::vector<CpuImage>& mips,
        bool compress,
        TextureUsage usage = TextureUsage::Color) noexcept;

    // 圧縮済みデータも含む、1ミップ分の転送情報。
    struct PreparedTextureLevel final
    {
        // ミップの幅（画素）
        std::uint32_t width{};
        // ミップの高さ（画素）
        std::uint32_t height{};
        // 1行のバイト数
        std::uint32_t rowPitch{};
        // ミップ全体の転送バイト列
        std::vector<std::uint8_t> bytes;
    };

    // 形式を確定した2Dミップ列の転送情報。
    struct PreparedTextureData final
    {
        // 全ミップの格納形式
        DXGI_FORMAT format{ DXGI_FORMAT_R8G8B8A8_UNORM };
        // 大きい順のミップ転送情報
        std::vector<PreparedTextureLevel> levels;

        // 全ミップの転送バイト数を合計して返す。
        [[nodiscard]] std::size_t TotalBytes() const noexcept
        {
            // 全ミップの転送バイト数
            std::size_t total = 0;
            // 合計する1ミップの転送情報
            for (const auto& level : levels)
            {
                total += level.bytes.size();
            }
            return total;
        }
    };

    enum class PreparedDdsTextureDimension : std::uint8_t
    {
        // 単体の2D画像
        Texture2D,
        // 2D画像の配列
        Texture2DArray,
        // 単体の6面キューブ
        TextureCube,
        // 6面キューブの配列
        TextureCubeArray,
        // 奥行きを持つ3D画像
        Texture3D
    };

    // DDSの次元と転送順を保持する。
    // 2Dは配列要素・面ごとに全ミップを並べ、3Dは各ミップの全奥行きを1要素へまとめる。
    struct PreparedDdsTextureData final
    {
        // 全サブリソースの格納形式
        DXGI_FORMAT format{ DXGI_FORMAT_R8G8B8A8_UNORM };
        // DDSのリソース次元
        PreparedDdsTextureDimension dimension{
            PreparedDdsTextureDimension::Texture2D };
        // 最も細かいミップの幅
        std::uint32_t width{};
        // 最も細かいミップの高さ
        std::uint32_t height{};
        // 3Dの奥行き、2Dは1
        std::uint32_t depth{ 1 };
        // 配列枚数、キューブは個数
        std::uint32_t arraySize{ 1 };
        // 面または配列要素のミップ数
        std::uint32_t mipLevels{ 1 };
        // 面・配列・奥行きの転送情報
        std::vector<PreparedTextureLevel> subresources;
    };

    // DDSの次元を保って面・配列・奥行きとミップを展開する(bytes: 元DDSのバイト列)。
    [[nodiscard]] PreparedDdsTextureData PrepareDdsResourceData(
        std::span<const std::uint8_t> bytes);

    // 単体2D DDSを展開し、他次元は例外で拒否する(bytes: 元DDSのバイト列)。
    [[nodiscard]] PreparedTextureData PrepareDdsTextureData(
        std::span<const std::uint8_t> bytes);

    // DDSのキューブ指定フラグをヘッダーだけで調べる(bytes: 元DDSのバイト列)。
    // フラグ判定後も、面数・寸法・ペイロードの検証はPrepare側で行う。
    [[nodiscard]] bool IsDdsCubeTexture(
        std::span<const std::uint8_t> bytes) noexcept;

    // 単体キューブDDSを6面それぞれのミップ列へ展開する(bytes: 元DDSのバイト列)。
    [[nodiscard]] PreparedTextureData PrepareDdsCubeTextureData(
        std::span<const std::uint8_t> bytes);

    // ミップを用途に応じて圧縮または移動し、転送情報を作る(mips: 大きい順の有効なRGBA8画像列, compress: BC圧縮を使うか, usage: 画像用途)。
    [[nodiscard]] PreparedTextureData PrepareTextureData(
        std::vector<CpuImage> mips,
        bool compress,
        TextureUsage usage = TextureUsage::Color);

    // 準備済みの全ミップを不変テクスチャとビューへ一括転送する(device: 作成先デバイス, data: 有効な2Dミップ転送情報)。
    [[nodiscard]]
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
        CreateTexture(
            ID3D11Device* device,
            const PreparedTextureData& data);

    // 作成はワーカーでも可能だが、UpdateSubresourceによる書き込みはメインスレッドで行う。
    // 初期画素なしの2Dテクスチャを作る(device: 作成先デバイス, data: 寸法・形式・ミップ数の指定元)。
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D11Texture2D>
        CreateUploadableTexture(
            ID3D11Device* device,
            const PreparedTextureData& data);

    // 転送が済んだミップだけを公開するよう、呼出元がmostDetailedMipを選ぶ。
    // 指定ミップ以降を公開する2Dビューを作る(device: 作成先デバイス, texture: 公開する2Dテクスチャ, format: 画像の形式, mostDetailedMip: 最も細かい公開済みミップ番号, mipLevels: 全ミップ数)。
    [[nodiscard]]
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
        CreateTextureView(
            ID3D11Device* device,
            ID3D11Texture2D* texture,
            DXGI_FORMAT format,
            std::uint32_t mostDetailedMip,
            std::uint32_t mipLevels);
}
