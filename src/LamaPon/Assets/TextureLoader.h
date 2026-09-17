#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <span>
#include <vector>

namespace LamaPon::TextureLoader
{
    // CPU側のRGBA8イメージ。
    struct CpuImage final
    {
        std::uint32_t width{};
        std::uint32_t height{};
        std::vector<std::uint8_t> pixels;
    };

    // WICでPNG/JPG等をRGBA8へデコードします（COM初期化を内包し、
    // ワーカースレッドから呼べます）。失敗時は例外。
    [[nodiscard]] CpuImage DecodeImageBytes(
        std::span<const std::uint8_t> bytes);

    // ボックスフィルタで1x1までのミップ列を生成します
    // （先頭は入力そのもの）。
    [[nodiscard]] std::vector<CpuImage> GenerateMipChain(
        CpuImage base);

    [[nodiscard]] bool HasTransparentPixels(
        const CpuImage& image) noexcept;

    // テクスチャの用途。圧縮フォーマットの選び方が変わります。
    //
    // 色以外のマップまで色と同じBC1へ入れると壊れます。特に法線は
    // RGB565の量子化が方向の誤差になって、陰影に帯が出ます。BC5は
    // RGの2チャンネルをそれぞれ8ビット相当で持つので法線に向きます
    // （Zはシェーダーで復元します）。
    enum class TextureUsage
    {
        // baseColor / emissive。透過があればBC3、無ければBC1。
        Color,
        // 法線マップ。BC5（RG）。
        NormalMap,
        // metallicRoughness / occlusion。アルファに意味が無いのでBC1。
        DataMap,
    };

    // 4x4ブロックのBC1（不透明）/BC3（アルファ付き）圧縮。
    [[nodiscard]] std::vector<std::uint8_t> CompressBC1(
        const CpuImage& image);
    [[nodiscard]] std::vector<std::uint8_t> CompressBC3(
        const CpuImage& image);
    // BC5（RGの2チャンネル）。中身はBC4ブロック2つで、BC3の
    // アルファ部と同じ符号化をR・Gそれぞれに掛けたものです。
    [[nodiscard]] std::vector<std::uint8_t> CompressBC5(
        const CpuImage& image);

    // ミップ列からimmutableテクスチャとSRVを作成します。
    // ID3D11Deviceのリソース生成はフリースレッドなので
    // ワーカースレッドから呼べます。compressでBC圧縮を使用。
    [[nodiscard]]
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
        CreateTexture(
            ID3D11Device* device,
            const std::vector<CpuImage>& mips,
            bool compress,
            TextureUsage usage = TextureUsage::Color);

    // ミップ列と圧縮設定から実際に使うフォーマットを決めます
    // （BCはトップレベルが4の倍数の場合のみ）。
    [[nodiscard]] DXGI_FORMAT ChooseTextureFormat(
        const std::vector<CpuImage>& mips,
        bool compress,
        TextureUsage usage = TextureUsage::Color) noexcept;

    // GPUへ渡す直前の1ミップ分（必要ならBC圧縮済み）。
    struct PreparedTextureLevel final
    {
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint32_t rowPitch{};
        std::vector<std::uint8_t> bytes;
    };

    // フォーマット確定済みの全ミップ転送データ。
    struct PreparedTextureData final
    {
        DXGI_FORMAT format{ DXGI_FORMAT_R8G8B8A8_UNORM };
        std::vector<PreparedTextureLevel> levels;

        [[nodiscard]] std::size_t TotalBytes() const noexcept
        {
            std::size_t total = 0;
            for (const auto& level : levels)
            {
                total += level.bytes.size();
            }
            return total;
        }
    };

    enum class PreparedDdsTextureDimension : std::uint8_t
    {
        Texture2D,
        Texture2DArray,
        TextureCube,
        TextureCubeArray,
        Texture3D
    };

    // DDSが持つresource次元を保った、D3D11／D3D12共通の転送表現です。
    // 2D系のsubresourcesはarray sliceごとに全mip、3Dはmipごとに
    // 全depth sliceを1要素へまとめます。
    struct PreparedDdsTextureData final
    {
        DXGI_FORMAT format{ DXGI_FORMAT_R8G8B8A8_UNORM };
        PreparedDdsTextureDimension dimension{
            PreparedDdsTextureDimension::Texture2D };
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint32_t depth{ 1 };
        // Texture2DArrayはslice数、TextureCubeArrayはcube数です。
        std::uint32_t arraySize{ 1 };
        std::uint32_t mipLevels{ 1 };
        std::vector<PreparedTextureLevel> subresources;
    };

    // 2D／array／cube／cube array／volumeをresource次元付きで展開します。
    [[nodiscard]] PreparedDdsTextureData PrepareDdsResourceData(
        std::span<const std::uint8_t> bytes);

    // 2D DDSのヘッダーとミップ列をAPI非依存な転送データへ展開します。
    // 共通Backendが直接uploadできるUNORM／SNORM／sRGB、BC、float、
    // packed形式、typeless storageの既定viewとDirectXTK互換のlegacy
    // headerに対応し、
    // 配列・キューブ・volumeや未対応formatは誤ったtextureとして作らず
    // 例外にします。
    [[nodiscard]] PreparedTextureData PrepareDdsTextureData(
        std::span<const std::uint8_t> bytes);

    // 6面を持つcube DDSかをヘッダーだけで判定します。壊れたヘッダーは
    // falseで、読み込み時にPrepare側が理由付きで拒否します。
    [[nodiscard]] bool IsDdsCubeTexture(
        std::span<const std::uint8_t> bytes) noexcept;

    // cube DDSを面ごと・ミップごとの転送データへ展開します。levelsは
    // +X、-X、+Y、-Y、+Z、-Zの順に各面の全ミップを並べ（面数×ミップ数）、
    // 対応formatは2D DDSと同じです。cubeの配列、欠けた面、正方形でない
    // 面は例外にします。
    [[nodiscard]] PreparedTextureData PrepareDdsCubeTextureData(
        std::span<const std::uint8_t> bytes);

    // ミップ列を段階アップロード用の転送データへ変換します
    // （CPU側のBC圧縮もここで行います）。ワーカースレッド可。
    [[nodiscard]] PreparedTextureData PrepareTextureData(
        std::vector<CpuImage> mips,
        bool compress,
        TextureUsage usage = TextureUsage::Color);

    // 転送データからimmutableテクスチャとSRVを一括作成します
    // （小さいテクスチャ向けの従来経路）。
    [[nodiscard]]
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
        CreateTexture(
            ID3D11Device* device,
            const PreparedTextureData& data);

    // 初期データなしのDEFAULTテクスチャを作成します。後から
    // UpdateSubresourceでミップ単位に書き込みます（作成自体は
    // フリースレッド、書き込みはメインスレッド専用）。
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D11Texture2D>
        CreateUploadableTexture(
            ID3D11Device* device,
            const PreparedTextureData& data);

    // mostDetailedMip以降のミップだけを参照するSRVを作成します。
    // アップロード済みの粗いミップから順に見せるために使います。
    [[nodiscard]]
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
        CreateTextureView(
            ID3D11Device* device,
            ID3D11Texture2D* texture,
            DXGI_FORMAT format,
            std::uint32_t mostDetailedMip,
            std::uint32_t mipLevels);
}
