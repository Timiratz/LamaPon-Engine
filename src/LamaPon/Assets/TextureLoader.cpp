#include "LamaPon/Assets/TextureLoader.h"
#include "LamaPon/Graphics/DxgiTextureLayout.h"

#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
    // 4文字を下位から順に32ビット識別子へ詰める(a: 第1文字, b: 第2文字, c: 第3文字, d: 第4文字)。
    constexpr std::uint32_t MakeFourCc(
        const char a,
        const char b,
        const char c,
        const char d) noexcept
    {
        return static_cast<std::uint8_t>(a)
            | static_cast<std::uint32_t>(
                static_cast<std::uint8_t>(b)) << 8u
            | static_cast<std::uint32_t>(
                static_cast<std::uint8_t>(c)) << 16u
            | static_cast<std::uint32_t>(
                static_cast<std::uint8_t>(d)) << 24u;
    }

    struct DdsPixelFormat final
    {
        // ピクセル形式情報のバイト数
        std::uint32_t size{};
        // 格納形式のフラグ
        std::uint32_t flags{};
        // 4文字またはD3D9形式の値
        std::uint32_t fourCc{};
        // 1画素のビット数
        std::uint32_t rgbBitCount{};
        // 赤成分のビットマスク
        std::uint32_t redMask{};
        // 緑成分のビットマスク
        std::uint32_t greenMask{};
        // 青成分のビットマスク
        std::uint32_t blueMask{};
        // アルファのビットマスク
        std::uint32_t alphaMask{};
    };

    struct DdsHeader final
    {
        // 旧ヘッダーのバイト数
        std::uint32_t size{};
        // 旧ヘッダーの有効項目フラグ
        std::uint32_t flags{};
        // 最も細かいミップの高さ
        std::uint32_t height{};
        // 最も細かいミップの幅
        std::uint32_t width{};
        // 元の行幅または圧縮サイズ
        std::uint32_t pitchOrLinearSize{};
        // 元の3D画像の奥行き
        std::uint32_t depth{};
        // 記録されたミップ数
        std::uint32_t mipMapCount{};
        // 旧形式の予約領域
        std::array<std::uint32_t, 11> reserved{};
        // 旧形式の画素記述
        DdsPixelFormat pixelFormat{};
        // 元の画像機能フラグ
        std::uint32_t caps{};
        // キューブ・3D等の機能フラグ
        std::uint32_t caps2{};
        // 元の追加機能フラグ
        std::uint32_t caps3{};
        // 元の追加機能フラグ
        std::uint32_t caps4{};
        // 旧形式の追加予約領域
        std::uint32_t reserved2{};
    };

    struct DdsHeaderDx10 final
    {
        // 元のDXGI格納形式
        std::uint32_t format{};
        // 2D・3D等の次元の値
        std::uint32_t resourceDimension{};
        // キューブ等の機能フラグ
        std::uint32_t miscFlag{};
        // 配列枚数、キューブは個数
        std::uint32_t arraySize{};
        // 元のアルファモード等の情報
        std::uint32_t miscFlags2{};
    };

    static_assert(sizeof(DdsPixelFormat) == 32u);
    static_assert(sizeof(DdsHeader) == 124u);
    static_assert(sizeof(DdsHeaderDx10) == 20u);

    // 形式と寸法から最小行バイト数と行数を返す(format: 元の格納形式, width: ミップ幅（画素）, height: ミップ高（画素）)。
    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t>
        DdsLevelLayout(
            const DXGI_FORMAT format,
            const std::uint32_t width,
            const std::uint32_t height)
    {
        // 形式と寸法に対応する行配置
        const auto layout = LamaPon::Detail::RequiredTextureLayout(
            format,
            width,
            height);
        return { layout.minimumRowBytes, layout.rowCount };
    }

    // HRESULTが失敗なら操作名付き例外を送出する(result: APIの結果, operation: 失敗した操作名)。
    void ThrowIfFailed(
        const HRESULT result,
        const char* operation)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(
                std::string{ operation }
                + " failed with HRESULT "
                + std::to_string(
                    static_cast<unsigned long>(result)));
        }
    }

    // 呼び出しスレッドのCOMを初期化するRAII。
    struct ComScope final
    {
        // このスコープでCOM解放するか
        bool uninitialize{};

        // 現在のスレッドでCOM初期化を試み、成功した分だけ解放を予約する。
        ComScope()
        {
            // COM初期化の結果
            const HRESULT result = CoInitializeEx(
                nullptr,
                COINIT_MULTITHREADED);
            uninitialize = SUCCEEDED(result);
        }

        // このスコープが成功させたCOM初期化だけを解放する。
        ~ComScope()
        {
            if (uninitialize)
            {
                CoUninitialize();
            }
        }
    };

    // 8ビットRGBをRGB565へ量子化する(red: 赤成分, green: 緑成分, blue: 青成分)。
    [[nodiscard]] std::uint16_t To565(
        const std::uint8_t red,
        const std::uint8_t green,
        const std::uint8_t blue) noexcept
    {
        return static_cast<std::uint16_t>(
            ((red >> 3) << 11)
            | ((green >> 2) << 5)
            | (blue >> 3));
    }

    // 画像端を繰り返して4×4のRGBA画素を取り出す(image: 正寸法と全画素を持つ元画像, blockX: ブロック列番号, blockY: ブロック行番号, block: 64バイトの返却先)。
    void ExtractBlock(
        const LamaPon::TextureLoader::CpuImage& image,
        const std::uint32_t blockX,
        const std::uint32_t blockY,
        std::array<std::uint8_t, 64>& block) noexcept
    {
        // 4×4ブロック内の行番号
        for (std::uint32_t row = 0; row < 4; ++row)
        {
            // 読み書きする画素の行番号
            const std::uint32_t y = std::min(
                blockY * 4 + row,
                image.height - 1);
            // 4×4ブロック内の列番号
            for (std::uint32_t column = 0;
                column < 4;
                ++column)
            {
                // 読み書きする画素の列番号
                const std::uint32_t x = std::min(
                    blockX * 4 + column,
                    image.width - 1);
                // コピー元の画素列の位置
                const std::size_t source =
                    (static_cast<std::size_t>(y)
                        * image.width
                        + x) * 4;
                // 書き込み先の画素列の位置
                const std::size_t destination =
                    (row * 4 + column) * 4;
                block[destination] =
                    image.pixels[source];
                block[destination + 1] =
                    image.pixels[source + 1];
                block[destination + 2] =
                    image.pixels[source + 2];
                block[destination + 3] =
                    image.pixels[source + 3];
            }
        }
    }

    // RGB合計の最小・最大を端点にBC色ブロックを作る(block: 4×4のRGBA画素, destination: 8バイト以上の返却先)。
    void EncodeColorBlock(
        const std::array<std::uint8_t, 64>& block,
        std::uint8_t* destination) noexcept
    {
        // ブロック内の最大RGB合計
        int brightest = -1;
        // ブロック内の最小RGB合計
        int darkest = 256 * 3 + 1;
        // RGB合計が最大の端点色
        std::array<std::uint8_t, 3> endpointHigh{};
        // RGB合計が最小の端点色
        std::array<std::uint8_t, 3> endpointLow{};
        // ブロック内の画素番号
        for (int pixel = 0; pixel < 16; ++pixel)
        {
            // 画素のRGB成分の合計
            const int luminance =
                block[pixel * 4]
                + block[pixel * 4 + 1]
                + block[pixel * 4 + 2];
            if (luminance > brightest)
            {
                brightest = luminance;
                endpointHigh = {
                    block[pixel * 4],
                    block[pixel * 4 + 1],
                    block[pixel * 4 + 2] };
            }
            if (luminance < darkest)
            {
                darkest = luminance;
                endpointLow = {
                    block[pixel * 4],
                    block[pixel * 4 + 1],
                    block[pixel * 4 + 2] };
            }
        }

        // RGB565の第1端点
        std::uint16_t color0 = To565(
            endpointHigh[0],
            endpointHigh[1],
            endpointHigh[2]);
        // RGB565の第2端点
        std::uint16_t color1 = To565(
            endpointLow[0],
            endpointLow[1],
            endpointLow[2]);
        // BC1はcolor0 > color1で4色モードになります。
        if (color0 < color1)
        {
            std::swap(color0, color1);
            std::swap(endpointHigh, endpointLow);
        }


        // 成分を補間した選択肢
        std::array<std::array<int, 3>, 4> palette{};
        // RGBA成分の番号
        for (int channel = 0; channel < 3; ++channel)
        {
            palette[0][channel] = endpointHigh[channel];
            palette[1][channel] = endpointLow[channel];
            palette[2][channel] =
                (2 * endpointHigh[channel]
                    + endpointLow[channel]) / 3;
            palette[3][channel] =
                (endpointHigh[channel]
                    + 2 * endpointLow[channel]) / 3;
        }

        // 画素ごとのパレット番号列
        std::uint32_t indices = 0;
        // 端点が等しいと透明モードになるため、全画素を不透明な端点0へ割り当てる。
        if (color0 != color1)
        {
            // ブロック内の画素番号
            for (int pixel = 15; pixel >= 0; --pixel)
            {
                // 最も近いパレット番号
                int bestIndex = 0;
                // 最小の成分誤差の二乗和
                int bestDistance =
                    std::numeric_limits<int>::max();
                // 比較するパレット番号
                for (int candidate = 0;
                    candidate < 4;
                    ++candidate)
                {
                    // 成分誤差の二乗和
                    int distance = 0;
                    // RGBA成分の番号
                    for (int channel = 0;
                        channel < 3;
                        ++channel)
                    {
                        // 元成分と候補の差
                        const int delta =
                            block[pixel * 4 + channel]
                            - palette[candidate][channel];
                        distance += delta * delta;
                    }
                    if (distance < bestDistance)
                    {
                        bestDistance = distance;
                        bestIndex = candidate;
                    }
                }
                indices = (indices << 2)
                    | static_cast<std::uint32_t>(
                        bestIndex);
            }
        }

        std::memcpy(destination, &color0, 2);
        std::memcpy(destination + 2, &color1, 2);
        std::memcpy(destination + 4, &indices, 4);
    }

    // 1成分の最小・最大からBC4ブロックを作る(block: 4×4のRGBA画素, channel: RGBAの0〜3の成分番号, destination: 8バイト以上の返却先)。
    void EncodeChannelBlock(
        const std::array<std::uint8_t, 64>& block,
        const int channel,
        std::uint8_t* destination) noexcept
    {
        // 選択成分の最大値
        std::uint8_t alphaHigh = 0;
        // 選択成分の最小値
        std::uint8_t alphaLow = 255;
        // ブロック内の画素番号
        for (int pixel = 0; pixel < 16; ++pixel)
        {
            // 選択成分の画素値
            const std::uint8_t alpha =
                block[pixel * 4 + channel];
            alphaHigh = std::max(alphaHigh, alpha);
            alphaLow = std::min(alphaLow, alpha);
        }
        destination[0] = alphaHigh;
        destination[1] = alphaLow;


        // 成分を補間した選択肢
        std::array<int, 8> palette{};
        palette[0] = alphaHigh;
        palette[1] = alphaLow;
        // 端点間の補間位置
        for (int step = 1; step <= 6; ++step)
        {
            palette[static_cast<std::size_t>(step) + 1] =
                ((7 - step) * alphaHigh
                    + step * alphaLow) / 7;
        }

        // 画素ごとのパレット番号列
        std::uint64_t indices = 0;
        // ブロック内の画素番号
        for (int pixel = 15; pixel >= 0; --pixel)
        {
            // 選択成分の画素値
            const int alpha = block[pixel * 4 + channel];
            // 最も近いパレット番号
            int bestIndex = 0;
            // 最小の成分誤差の二乗和
            int bestDistance =
                std::numeric_limits<int>::max();
            // 比較するパレット番号
            for (int candidate = 0;
                candidate < 8;
                ++candidate)
            {
                // 元成分と候補の差
                const int delta =
                    alpha - palette[candidate];
                // 成分誤差の二乗和
                const int distance = delta * delta;
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    bestIndex = candidate;
                }
            }
            indices = (indices << 3)
                | static_cast<std::uint64_t>(bestIndex);
        }
        // パレット番号列のバイト位置
        for (int byte = 0; byte < 6; ++byte)
        {
            destination[2 + byte] =
                static_cast<std::uint8_t>(
                    (indices >> (byte * 8)) & 0xffu);
        }
    }

    // 1辺に必要な4画素ブロック数を最低1で返す(size: 画像の幅または高さ)。
    [[nodiscard]] std::uint32_t BlockCount(
        const std::uint32_t size) noexcept
    {
        return std::max((size + 3) / 4, 1u);
    }

    // DDSの形式識別子
    constexpr std::uint32_t DdsMagic = MakeFourCc('D', 'D', 'S', ' ');
    // FourCC格納のフラグ
    constexpr std::uint32_t PixelFormatFourCc = 0x4u;

    // キューブ指定と6面のフラグ
    constexpr std::uint32_t Caps2CubeMapMask = 0xfe00u;
    // DX10のキューブ指定フラグ
    constexpr std::uint32_t ResourceMiscTextureCube = 0x4u;

    // 対応するDDSを検査し、次元付き転送情報へ展開する(bytes: 元DDSのバイト列)。
    [[nodiscard]] LamaPon::TextureLoader::PreparedDdsTextureData
        ParseDdsResourceData(const std::span<const std::uint8_t> bytes)
    {
        // RGB格納のフラグ
        constexpr std::uint32_t PixelFormatRgb = 0x40u;
        // 輝度格納のフラグ
        constexpr std::uint32_t PixelFormatLuminance = 0x20000u;
        // アルファ格納のフラグ
        constexpr std::uint32_t PixelFormatAlpha = 0x2u;
        // 符号付き凹凸成分のフラグ
        constexpr std::uint32_t PixelFormatBumpDuDv = 0x80000u;
        // 3D画像指定のフラグ
        constexpr std::uint32_t Caps2Volume = 0x200000u;
        // DX10の2D画像次元の値
        constexpr std::uint32_t ResourceDimensionTexture2D = 3u;
        // DX10の3D画像次元の値
        constexpr std::uint32_t ResourceDimensionTexture3D = 4u;
        // DDSの1辺の最大画素数
        constexpr std::uint32_t MaximumTextureDimension = 16384u;

        if (bytes.size() < sizeof(std::uint32_t) + sizeof(DdsHeader))
        {
            throw std::invalid_argument("The DDS header is incomplete.");
        }
        // 読み取ったDDS識別子
        std::uint32_t magic{};
        // 読み取った旧DDSヘッダー
        DdsHeader header{};
        std::memcpy(&magic, bytes.data(), sizeof(magic));
        std::memcpy(
            &header,
            bytes.data() + sizeof(magic),
            sizeof(header));
        if (magic != DdsMagic
            || header.size != sizeof(DdsHeader)
            || header.pixelFormat.size != sizeof(DdsPixelFormat)
            || header.width == 0u
            || header.height == 0u
            || header.width > MaximumTextureDimension
            || header.height > MaximumTextureDimension)
        {
            throw std::invalid_argument("The DDS header is invalid.");
        }
        // 旧形式のキューブは6面の指定が揃わなければ拒否する。
        // キューブ指定の有無
        bool fileCube = (header.caps2 & Caps2CubeMapMask) != 0u;
        // 3D画像指定の有無
        bool fileVolume = (header.caps2 & Caps2Volume) != 0u;
        // 配列枚数、キューブは個数
        std::uint32_t arraySize = 1u;
        if (fileCube
            && (header.caps2 & Caps2CubeMapMask) != Caps2CubeMapMask)
        {
            throw std::invalid_argument(
                "DDS cubes without all six faces are not supported.");
        }

        // 画像データの開始バイト位置
        std::size_t payloadOffset = sizeof(magic) + sizeof(header);
        // 確定する画像の格納形式
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        if ((header.pixelFormat.flags & PixelFormatFourCc) != 0u)
        {
            switch (header.pixelFormat.fourCc)
            {
            case MakeFourCc('D', 'X', 'T', '1'):
                format = DXGI_FORMAT_BC1_UNORM;
                break;
            case MakeFourCc('D', 'X', 'T', '2'):
            case MakeFourCc('D', 'X', 'T', '3'):
                format = DXGI_FORMAT_BC2_UNORM;
                break;
            case MakeFourCc('D', 'X', 'T', '4'):
            case MakeFourCc('D', 'X', 'T', '5'):
                format = DXGI_FORMAT_BC3_UNORM;
                break;
            case MakeFourCc('A', 'T', 'I', '1'):
            case MakeFourCc('B', 'C', '4', 'U'):
                format = DXGI_FORMAT_BC4_UNORM;
                break;
            case MakeFourCc('B', 'C', '4', 'S'):
                format = DXGI_FORMAT_BC4_SNORM;
                break;
            case MakeFourCc('A', 'T', 'I', '2'):
            case MakeFourCc('B', 'C', '5', 'U'):
                format = DXGI_FORMAT_BC5_UNORM;
                break;
            case MakeFourCc('B', 'C', '5', 'S'):
                format = DXGI_FORMAT_BC5_SNORM;
                break;
            case MakeFourCc('R', 'G', 'B', 'G'):
                format = DXGI_FORMAT_R8G8_B8G8_UNORM;
                break;
            case MakeFourCc('G', 'R', 'G', 'B'):
                format = DXGI_FORMAT_G8R8_G8B8_UNORM;
                break;
            case MakeFourCc('Y', 'U', 'Y', '2'):
                format = DXGI_FORMAT_YUY2;
                break;
            // D3D9のD3DFORMAT値をFourCC欄へ直接保存する旧DDSです。
            case 36u:
                format = DXGI_FORMAT_R16G16B16A16_UNORM;
                break;
            case 110u:
                format = DXGI_FORMAT_R16G16B16A16_SNORM;
                break;
            case 111u:
                format = DXGI_FORMAT_R16_FLOAT;
                break;
            case 112u:
                format = DXGI_FORMAT_R16G16_FLOAT;
                break;
            case 113u:
                format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                break;
            case 114u:
                format = DXGI_FORMAT_R32_FLOAT;
                break;
            case 115u:
                format = DXGI_FORMAT_R32G32_FLOAT;
                break;
            case 116u:
                format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                break;
            case MakeFourCc('D', 'X', '1', '0'):
            {
                if (bytes.size() < payloadOffset + sizeof(DdsHeaderDx10))
                {
                    throw std::invalid_argument(
                        "The DDS DX10 header is incomplete.");
                }
                // 読み取ったDX10拡張情報
                DdsHeaderDx10 dx10{};
                std::memcpy(
                    &dx10,
                    bytes.data() + payloadOffset,
                    sizeof(dx10));
                payloadOffset += sizeof(dx10);
                if (dx10.arraySize == 0u
                    || (dx10.resourceDimension
                            != ResourceDimensionTexture2D
                        && dx10.resourceDimension
                            != ResourceDimensionTexture3D))
                {
                    throw std::invalid_argument(
                        "The DDS resource dimension is not supported.");
                }
                fileVolume = dx10.resourceDimension
                    == ResourceDimensionTexture3D;
                if (fileVolume && dx10.arraySize != 1u)
                {
                    throw std::invalid_argument(
                        "DDS volume texture arrays are invalid.");
                }
                arraySize = dx10.arraySize;
                if ((dx10.miscFlag & ResourceMiscTextureCube) != 0u)
                {
                    if (fileVolume)
                    {
                        throw std::invalid_argument(
                            "A DDS volume cannot also be a cube texture.");
                    }
                    fileCube = true;
                }
                format = static_cast<DXGI_FORMAT>(dx10.format);
                break;
            }
            default:
                break;
            }
        }
        else if ((header.pixelFormat.flags & PixelFormatRgb) != 0u)
        {
            if (header.pixelFormat.rgbBitCount == 32u)
            {
                if (header.pixelFormat.redMask == 0x000000ffu
                    && header.pixelFormat.greenMask == 0x0000ff00u
                    && header.pixelFormat.blueMask == 0x00ff0000u
                    && header.pixelFormat.alphaMask == 0xff000000u)
                {
                    format = DXGI_FORMAT_R8G8B8A8_UNORM;
                }
                else if (header.pixelFormat.redMask == 0x00ff0000u
                    && header.pixelFormat.greenMask == 0x0000ff00u
                    && header.pixelFormat.blueMask == 0x000000ffu)
                {
                    format = header.pixelFormat.alphaMask == 0xff000000u
                        ? DXGI_FORMAT_B8G8R8A8_UNORM
                        : header.pixelFormat.alphaMask == 0u
                            ? DXGI_FORMAT_B8G8R8X8_UNORM
                            : DXGI_FORMAT_UNKNOWN;
                }
                else if (header.pixelFormat.redMask == 0x3ff00000u
                    && header.pixelFormat.greenMask == 0x000ffc00u
                    && header.pixelFormat.blueMask == 0x000003ffu
                    && header.pixelFormat.alphaMask == 0xc0000000u)
                {
                    format = DXGI_FORMAT_R10G10B10A2_UNORM;
                }
                else if (header.pixelFormat.redMask == 0x0000ffffu
                    && header.pixelFormat.greenMask == 0xffff0000u
                    && header.pixelFormat.blueMask == 0u
                    && header.pixelFormat.alphaMask == 0u)
                {
                    format = DXGI_FORMAT_R16G16_UNORM;
                }
                else if (header.pixelFormat.redMask == 0xffffffffu
                    && header.pixelFormat.greenMask == 0u
                    && header.pixelFormat.blueMask == 0u
                    && header.pixelFormat.alphaMask == 0u)
                {
                    format = DXGI_FORMAT_R32_FLOAT;
                }
            }
            else if (header.pixelFormat.rgbBitCount == 16u)
            {
                if (header.pixelFormat.redMask == 0x7c00u
                    && header.pixelFormat.greenMask == 0x03e0u
                    && header.pixelFormat.blueMask == 0x001fu
                    && header.pixelFormat.alphaMask == 0x8000u)
                {
                    format = DXGI_FORMAT_B5G5R5A1_UNORM;
                }
                else if (header.pixelFormat.redMask == 0xf800u
                    && header.pixelFormat.greenMask == 0x07e0u
                    && header.pixelFormat.blueMask == 0x001fu
                    && header.pixelFormat.alphaMask == 0u)
                {
                    format = DXGI_FORMAT_B5G6R5_UNORM;
                }
                else if (header.pixelFormat.redMask == 0x0f00u
                    && header.pixelFormat.greenMask == 0x00f0u
                    && header.pixelFormat.blueMask == 0x000fu
                    && header.pixelFormat.alphaMask == 0xf000u)
                {
                    format = DXGI_FORMAT_B4G4R4A4_UNORM;
                }
                else if (header.pixelFormat.redMask == 0x00ffu
                    && header.pixelFormat.greenMask == 0u
                    && header.pixelFormat.blueMask == 0u
                    && header.pixelFormat.alphaMask == 0xff00u)
                {
                    format = DXGI_FORMAT_R8G8_UNORM;
                }
                else if (header.pixelFormat.redMask == 0xffffu
                    && header.pixelFormat.greenMask == 0u
                    && header.pixelFormat.blueMask == 0u
                    && header.pixelFormat.alphaMask == 0u)
                {
                    format = DXGI_FORMAT_R16_UNORM;
                }
            }
            else if (header.pixelFormat.rgbBitCount == 8u
                && header.pixelFormat.redMask == 0xffu
                && header.pixelFormat.greenMask == 0u
                && header.pixelFormat.blueMask == 0u
                && header.pixelFormat.alphaMask == 0u)
            {
                format = DXGI_FORMAT_R8_UNORM;
            }
        }
        else if ((header.pixelFormat.flags & PixelFormatLuminance) != 0u)
        {
            if (header.pixelFormat.rgbBitCount == 8u
                && header.pixelFormat.redMask == 0xffu)
            {
                format = DXGI_FORMAT_R8_UNORM;
            }
            else if (header.pixelFormat.rgbBitCount == 16u
                && header.pixelFormat.redMask == 0xffffu
                && header.pixelFormat.alphaMask == 0u)
            {
                format = DXGI_FORMAT_R16_UNORM;
            }
            else if ((header.pixelFormat.rgbBitCount == 16u
                    || header.pixelFormat.rgbBitCount == 8u)
                && header.pixelFormat.redMask == 0x00ffu
                && header.pixelFormat.alphaMask == 0xff00u)
            {
                format = DXGI_FORMAT_R8G8_UNORM;
            }
        }
        else if ((header.pixelFormat.flags & PixelFormatAlpha) != 0u
            && header.pixelFormat.rgbBitCount == 8u)
        {
            format = DXGI_FORMAT_A8_UNORM;
        }
        else if ((header.pixelFormat.flags & PixelFormatBumpDuDv) != 0u)
        {
            if (header.pixelFormat.rgbBitCount == 16u
                && header.pixelFormat.redMask == 0x00ffu
                && header.pixelFormat.greenMask == 0xff00u)
            {
                format = DXGI_FORMAT_R8G8_SNORM;
            }
            else if (header.pixelFormat.rgbBitCount == 32u
                && header.pixelFormat.redMask == 0x000000ffu
                && header.pixelFormat.greenMask == 0x0000ff00u
                && header.pixelFormat.blueMask == 0x00ff0000u
                && header.pixelFormat.alphaMask == 0xff000000u)
            {
                format = DXGI_FORMAT_R8G8B8A8_SNORM;
            }
            else if (header.pixelFormat.rgbBitCount == 32u
                && header.pixelFormat.redMask == 0x0000ffffu
                && header.pixelFormat.greenMask == 0xffff0000u
                && header.pixelFormat.blueMask == 0u
                && header.pixelFormat.alphaMask == 0u)
            {
                format = DXGI_FORMAT_R16G16_SNORM;
            }
        }
        // typeless形式はDirectXTK互換の既定typed形式へ確定し、リソースとビューに同じ形式を使う。
        switch (format)
        {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
            format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            break;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            format = DXGI_FORMAT_R16G16B16A16_UNORM;
            break;
        case DXGI_FORMAT_R32G32_TYPELESS:
            format = DXGI_FORMAT_R32G32_FLOAT;
            break;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            format = DXGI_FORMAT_R10G10B10A2_UNORM;
            break;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            format = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;
        case DXGI_FORMAT_R16G16_TYPELESS:
            format = DXGI_FORMAT_R16G16_UNORM;
            break;
        case DXGI_FORMAT_R32_TYPELESS:
            format = DXGI_FORMAT_R32_FLOAT;
            break;
        case DXGI_FORMAT_R8G8_TYPELESS:
            format = DXGI_FORMAT_R8G8_UNORM;
            break;
        case DXGI_FORMAT_R16_TYPELESS:
            format = DXGI_FORMAT_R16_UNORM;
            break;
        case DXGI_FORMAT_R8_TYPELESS:
            format = DXGI_FORMAT_R8_UNORM;
            break;
        case DXGI_FORMAT_BC1_TYPELESS:
            format = DXGI_FORMAT_BC1_UNORM;
            break;
        case DXGI_FORMAT_BC2_TYPELESS:
            format = DXGI_FORMAT_BC2_UNORM;
            break;
        case DXGI_FORMAT_BC3_TYPELESS:
            format = DXGI_FORMAT_BC3_UNORM;
            break;
        case DXGI_FORMAT_BC4_TYPELESS:
            format = DXGI_FORMAT_BC4_UNORM;
            break;
        case DXGI_FORMAT_BC5_TYPELESS:
            format = DXGI_FORMAT_BC5_UNORM;
            break;
        case DXGI_FORMAT_BC6H_TYPELESS:
            format = DXGI_FORMAT_BC6H_UF16;
            break;
        case DXGI_FORMAT_BC7_TYPELESS:
            format = DXGI_FORMAT_BC7_UNORM;
            break;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            format = DXGI_FORMAT_B8G8R8A8_UNORM;
            break;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
            format = DXGI_FORMAT_B8G8R8X8_UNORM;
            break;
        default:
            break;
        }
        switch (format)
        {
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_B5G5R5A1_UNORM:
        case DXGI_FORMAT_B5G6R5_UNORM:
        case DXGI_FORMAT_B4G4R4A4_UNORM:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_A8_UNORM:
        case DXGI_FORMAT_R8G8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R16G16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R8G8_B8G8_UNORM:
        case DXGI_FORMAT_G8R8_G8B8_UNORM:
        case DXGI_FORMAT_R8_SNORM:
        case DXGI_FORMAT_R16_SNORM:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
        case DXGI_FORMAT_YUY2:
            break;
        default:
            throw std::invalid_argument(
                "The DDS texture format is not supported by the active "
                "graphics backends.");
        }
        if (fileVolume && (header.depth == 0u
            || header.depth > MaximumTextureDimension))
        {
            throw std::invalid_argument(
                "The DDS volume depth is invalid.");
        }
        if (fileCube && header.width != header.height)
        {
            throw std::invalid_argument(
                "The DDS cube faces must be square.");
        }

        // 1面あたりのミップ数
        const std::uint32_t mipCount = std::max(header.mipMapCount, 1u);
        // 寸法から決まる最大ミップ数
        std::uint32_t maximumMipCount = 1u;
        // 最大ミップ数を求める1辺
        for (std::uint32_t size = std::max({
                header.width,
                header.height,
                fileVolume ? header.depth : 1u });
            size > 1u;
            size >>= 1u)
        {
            ++maximumMipCount;
        }
        if (mipCount > maximumMipCount)
        {
            throw std::invalid_argument(
                "The DDS texture has too many mip levels.");
        }

        if ((!fileCube && arraySize > 2048u)
            || (fileCube && arraySize > 2048u / 6u))
        {
            throw std::invalid_argument(
                "The DDS texture has too many array slices.");
        }
        // 格納する配列要素と面の総数
        const std::uint32_t sliceCount = fileVolume
            ? 1u
            : fileCube
                ? arraySize * 6u
                : arraySize;
        if (sliceCount == 0u || sliceCount > 2048u)
        {
            throw std::invalid_argument(
                "The DDS texture has too many array slices.");
        }
        // 元のDDSの格納形式
        const auto sourceFormat = format;
        // YUY2をRGBA8へ変換するか
        const bool convertYuy2 = sourceFormat == DXGI_FORMAT_YUY2;
        // 次元付きDDSの転送情報
        LamaPon::TextureLoader::PreparedDdsTextureData result;
        // YUY2は共通描画で扱えるRGBA8へCPUで展開する。
        result.format = convertYuy2
            ? DXGI_FORMAT_R8G8B8A8_UNORM
            : sourceFormat;
        result.dimension = fileVolume
            ? LamaPon::TextureLoader::PreparedDdsTextureDimension::Texture3D
            : fileCube
                ? arraySize == 1u
                    ? LamaPon::TextureLoader::PreparedDdsTextureDimension::
                        TextureCube
                    : LamaPon::TextureLoader::PreparedDdsTextureDimension::
                        TextureCubeArray
                : arraySize == 1u
                    ? LamaPon::TextureLoader::PreparedDdsTextureDimension::
                        Texture2D
                    : LamaPon::TextureLoader::PreparedDdsTextureDimension::
                        Texture2DArray;
        result.width = header.width;
        result.height = header.height;
        result.depth = fileVolume ? header.depth : 1u;
        result.arraySize = arraySize;
        result.mipLevels = mipCount;
        result.subresources.reserve(
            static_cast<std::size_t>(mipCount) * sliceCount);
        // 次に読むDDSのバイト位置
        std::size_t offset = payloadOffset;
        // DDSのpitch指定は使わず、各形式の最小行幅で連続したデータとして読む。
        // 配列要素または面の番号
        for (std::uint32_t slice{}; slice < sliceCount; ++slice)
        {
            // 処理するミップ番号または画像
            for (std::uint32_t mip{}; mip < mipCount; ++mip)
            {
                // このミップの幅（画素）
                const auto width = std::max(header.width >> mip, 1u);
                // このミップの高さ（画素）
                const auto height = std::max(header.height >> mip, 1u);
                // rowPitch: 元の1行のバイト数, rowCount: ミップの行数
                const auto [rowPitch, rowCount] =
                    DdsLevelLayout(sourceFormat, width, height);
                // このミップの奥行き
                const auto depth = fileVolume
                    ? std::max(header.depth >> mip, 1u)
                    : 1u;
                // 奥行きを含むミップのバイト数
                const std::uint64_t levelBytes64 =
                    static_cast<std::uint64_t>(rowPitch) * rowCount * depth;
                if (offset > bytes.size()
                    || levelBytes64 > std::numeric_limits<std::size_t>::max()
                    || levelBytes64 > bytes.size() - offset)
                {
                    throw std::invalid_argument(
                        "The DDS mip data is incomplete.");
                }
                // 奥行きを含むミップのバイト数
                const auto levelBytes =
                    static_cast<std::size_t>(levelBytes64);
                // ミップの番号または転送情報
                LamaPon::TextureLoader::PreparedTextureLevel level;
                level.width = width;
                level.height = height;
                if (convertYuy2)
                {
                    level.rowPitch = width * 4u;
                    level.bytes.resize(
                        static_cast<std::size_t>(level.rowPitch)
                            * height * depth);
                    // 変換した整数を8ビット範囲へ制限する(value: 変換した色成分)。
                    const auto clampByte = [](const int value) noexcept
                    {
                        return static_cast<std::uint8_t>(
                            std::clamp(value, 0, 255));
                    };
                    // YUVをRGBA8へ変換する(destination: 4バイトの返却先, y: 輝度, u: 青差成分, v: 赤差成分)。
                    const auto writePixel = [&clampByte](
                        std::uint8_t* const destination,
                        const int y,
                        const int u,
                        const int v)
                    {
                        // 16を差し引いた輝度
                        const int c = std::max(y - 16, 0);
                        // 128を差し引いた青差成分
                        const int d = u - 128;
                        // 128を差し引いた赤差成分
                        const int e = v - 128;
                        destination[0] = clampByte(
                            (298 * c + 409 * e + 128) >> 8);
                        destination[1] = clampByte(
                            (298 * c - 100 * d - 208 * e + 128) >> 8);
                        destination[2] = clampByte(
                            (298 * c + 516 * d + 128) >> 8);
                        destination[3] = 255u;
                    };
                    // 元ミップのバイト先頭
                    const auto* const sourceBase = bytes.data() + offset;
                    // 奥行きの層番号
                    for (std::uint32_t z{}; z < depth; ++z)
                    {
                        // 読み書きする画素の行番号
                        for (std::uint32_t y{}; y < height; ++y)
                        {
                            // コピー元の画素列の位置
                            const auto* source = sourceBase
                                + (static_cast<std::size_t>(z) * rowCount + y)
                                    * rowPitch;
                            // 書き込み先の画素列の位置
                            auto* destination = level.bytes.data()
                                + (static_cast<std::size_t>(z) * height + y)
                                    * level.rowPitch;
                            // 読み書きする画素の列番号
                            for (std::uint32_t x{}; x < width; x += 2u)
                            {
                                writePixel(
                                    destination + x * 4u,
                                    source[0],
                                    source[1],
                                    source[3]);
                                if (x + 1u < width)
                                {
                                    writePixel(
                                        destination + (x + 1u) * 4u,
                                        source[2],
                                        source[1],
                                        source[3]);
                                }
                                source += 4u;
                            }
                        }
                    }
                }
                else
                {
                    level.rowPitch = rowPitch;
                    level.bytes.assign(
                        bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                        bytes.begin()
                            + static_cast<std::ptrdiff_t>(offset + levelBytes));
                }
                result.subresources.push_back(std::move(level));
                offset += levelBytes;
            }
        }
        return result;
    }
}

namespace LamaPon::TextureLoader
{
    PreparedDdsTextureData PrepareDdsResourceData(
        const std::span<const std::uint8_t> bytes)
    {
        return ParseDdsResourceData(bytes);
    }

    PreparedTextureData PrepareDdsTextureData(
        const std::span<const std::uint8_t> bytes)
    {
        // 展開した次元付きDDS
        auto resource = ParseDdsResourceData(bytes);
        if (resource.dimension != PreparedDdsTextureDimension::Texture2D)
        {
            throw std::invalid_argument(
                "The DDS is not a single two-dimensional texture.");
        }
        return { resource.format, std::move(resource.subresources) };
    }

    bool IsDdsCubeTexture(
        const std::span<const std::uint8_t> bytes) noexcept
    {
        // 旧DDSヘッダーの開始位置
        const std::size_t headerOffset = sizeof(std::uint32_t);
        if (bytes.size() < headerOffset + sizeof(DdsHeader))
        {
            return false;
        }
        // 読み取ったDDS識別子
        std::uint32_t magic{};
        // 読み取った旧DDSヘッダー
        DdsHeader header{};
        std::memcpy(&magic, bytes.data(), sizeof(magic));
        std::memcpy(
            &header,
            bytes.data() + headerOffset,
            sizeof(header));
        if (magic != DdsMagic || header.size != sizeof(DdsHeader))
        {
            return false;
        }
        if ((header.caps2 & Caps2CubeMapMask) != 0u)
        {
            return true;
        }
        // DX10拡張情報の開始位置
        const std::size_t extendedOffset = headerOffset + sizeof(header);
        if ((header.pixelFormat.flags & PixelFormatFourCc) == 0u
            || header.pixelFormat.fourCc != MakeFourCc('D', 'X', '1', '0')
            || bytes.size() < extendedOffset + sizeof(DdsHeaderDx10))
        {
            return false;
        }
        // 読み取ったDX10拡張情報
        DdsHeaderDx10 dx10{};
        std::memcpy(
            &dx10,
            bytes.data() + extendedOffset,
            sizeof(dx10));
        return (dx10.miscFlag & ResourceMiscTextureCube) != 0u;
    }

    PreparedTextureData PrepareDdsCubeTextureData(
        const std::span<const std::uint8_t> bytes)
    {
        // 展開した次元付きDDS
        auto resource = ParseDdsResourceData(bytes);
        if (resource.dimension != PreparedDdsTextureDimension::TextureCube)
        {
            throw std::invalid_argument(
                "The DDS is not a single cube texture.");
        }
        return { resource.format, std::move(resource.subresources) };
    }

    CpuImage DecodeImageBytes(
        const std::span<const std::uint8_t> bytes)
    {
        if (bytes.empty())
        {
            throw std::runtime_error(
                "Texture bytes are empty.");
        }
        // このスレッドのCOM解放管理
        const ComScope comScope;

        // WIC画像処理の生成元
        Microsoft::WRL::ComPtr<IWICImagingFactory>
            factory;
        ThrowIfFailed(
            CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(
                    factory.ReleaseAndGetAddressOf())),
            "CoCreateInstance(WICImagingFactory)");

        // WICストリームは元バイト列を借用し、復号をこの呼出し内で終える。
        // 元画像を借用するWIC列
        Microsoft::WRL::ComPtr<IWICStream> stream;
        ThrowIfFailed(
            factory->CreateStream(
                stream.ReleaseAndGetAddressOf()),
            "IWICImagingFactory::CreateStream");
        ThrowIfFailed(
            stream->InitializeFromMemory(
                const_cast<std::uint8_t*>(bytes.data()),
                static_cast<DWORD>(bytes.size())),
            "IWICStream::InitializeFromMemory");

        // 元画像のWICデコーダー
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        ThrowIfFailed(
            factory->CreateDecoderFromStream(
                stream.Get(),
                nullptr,
                WICDecodeMetadataCacheOnDemand,
                decoder.ReleaseAndGetAddressOf()),
            "IWICImagingFactory::CreateDecoderFromStream");

        // 復号する先頭フレーム
        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode>
            frame;
        ThrowIfFailed(
            decoder->GetFrame(
                0,
                frame.ReleaseAndGetAddressOf()),
            "IWICBitmapDecoder::GetFrame");

        // RGBA8画素への変換器
        Microsoft::WRL::ComPtr<IWICFormatConverter>
            converter;
        ThrowIfFailed(
            factory->CreateFormatConverter(
                converter.ReleaseAndGetAddressOf()),
            "IWICImagingFactory::CreateFormatConverter");
        ThrowIfFailed(
            converter->Initialize(
                frame.Get(),
                GUID_WICPixelFormat32bppRGBA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeCustom),
            "IWICFormatConverter::Initialize");

        // 復号したCPU画像
        CpuImage image;
        ThrowIfFailed(
            converter->GetSize(
                &image.width,
                &image.height),
            "IWICFormatConverter::GetSize");
        if (image.width == 0 || image.height == 0)
        {
            throw std::runtime_error(
                "Decoded image is empty.");
        }
        image.pixels.resize(
            static_cast<std::size_t>(image.width)
            * image.height
            * 4);
        ThrowIfFailed(
            converter->CopyPixels(
                nullptr,
                image.width * 4,
                static_cast<UINT>(image.pixels.size()),
                image.pixels.data()),
            "IWICFormatConverter::CopyPixels");
        return image;
    }

    std::vector<CpuImage> GenerateMipChain(CpuImage base)
    {
        // 生成するミップの一覧
        std::vector<CpuImage> mips;
        mips.push_back(std::move(base));
        while (mips.back().width > 1
            || mips.back().height > 1)
        {
            // 縮小元のミップ画像
            const auto& previous = mips.back();
            // 生成する1段粗いミップ画像
            CpuImage next;
            next.width =
                std::max(previous.width / 2, 1u);
            next.height =
                std::max(previous.height / 2, 1u);
            next.pixels.resize(
                static_cast<std::size_t>(next.width)
                * next.height
                * 4);
            // 読み書きする画素の行番号
            for (std::uint32_t y = 0;
                y < next.height;
                ++y)
            {
                // 平均する元画素の上の行
                const std::uint32_t sourceY0 =
                    std::min(
                        y * 2,
                        previous.height - 1);
                // 平均する元画素の下の行
                const std::uint32_t sourceY1 =
                    std::min(
                        y * 2 + 1,
                        previous.height - 1);
                // 読み書きする画素の列番号
                for (std::uint32_t x = 0;
                    x < next.width;
                    ++x)
                {
                    // 平均する元画素の左の列
                    const std::uint32_t sourceX0 =
                        std::min(
                            x * 2,
                            previous.width - 1);
                    // 平均する元画素の右の列
                    const std::uint32_t sourceX1 =
                        std::min(
                            x * 2 + 1,
                            previous.width - 1);
                    // RGBA成分の番号
                    for (int channel = 0;
                        channel < 4;
                        ++channel)
                    {
                        // 元ミップの1成分を読む(sampleX: 元画素の列, sampleY: 元画素の行)。
                        const auto sample =
                            [&previous, channel](
                                const std::uint32_t
                                    sampleX,
                                const std::uint32_t
                                    sampleY)
                        {
                            return static_cast<int>(
                                previous.pixels[
                                    (static_cast<
                                        std::size_t>(
                                            sampleY)
                                        * previous.width
                                        + sampleX) * 4
                                    + channel]);
                        };
                        // 4元画素の成分値の合計
                        const int sum =
                            sample(sourceX0, sourceY0)
                            + sample(sourceX1, sourceY0)
                            + sample(sourceX0, sourceY1)
                            + sample(sourceX1, sourceY1);
                        next.pixels[
                            (static_cast<std::size_t>(y)
                                * next.width
                                + x) * 4
                            + channel] =
                            static_cast<std::uint8_t>(
                                (sum + 2) / 4);
                    }
                }
            }
            mips.push_back(std::move(next));
        }
        return mips;
    }

    bool HasTransparentPixels(
        const CpuImage& image) noexcept
    {
        // 透過を調べるアルファの位置
        for (std::size_t index = 3;
            index < image.pixels.size();
            index += 4)
        {
            if (image.pixels[index] < 250)
            {
                return true;
            }
        }
        return false;
    }

    std::vector<std::uint8_t> CompressBC1(
        const CpuImage& image)
    {
        // 横方向の4画素ブロック数
        const std::uint32_t blocksX =
            BlockCount(image.width);
        // 縦方向の4画素ブロック数
        const std::uint32_t blocksY =
            BlockCount(image.height);
        // 圧縮したブロックのバイト列
        std::vector<std::uint8_t> output(
            static_cast<std::size_t>(blocksX)
            * blocksY
            * 8);
        // 4×4のRGBA画素列
        std::array<std::uint8_t, 64> block{};
        // 圧縮するブロック行番号
        for (std::uint32_t blockY = 0;
            // 縦方向の4画素ブロック数
            blockY < blocksY;
            ++blockY)
        {
            // 圧縮するブロック列番号
            for (std::uint32_t blockX = 0;
                // 横方向の4画素ブロック数
                blockX < blocksX;
                ++blockX)
            {
                ExtractBlock(
                    image,
                    blockX,
                    blockY,
                    block);
                EncodeColorBlock(
                    block,
                    output.data()
                        + (static_cast<std::size_t>(
                            blockY) * blocksX
                            + blockX) * 8);
            }
        }
        return output;
    }

    std::vector<std::uint8_t> CompressBC3(
        const CpuImage& image)
    {
        // 横方向の4画素ブロック数
        const std::uint32_t blocksX =
            BlockCount(image.width);
        // 縦方向の4画素ブロック数
        const std::uint32_t blocksY =
            BlockCount(image.height);
        // 圧縮したブロックのバイト列
        std::vector<std::uint8_t> output(
            static_cast<std::size_t>(blocksX)
            * blocksY
            * 16);
        // 4×4のRGBA画素列
        std::array<std::uint8_t, 64> block{};
        // 圧縮するブロック行番号
        for (std::uint32_t blockY = 0;
            // 縦方向の4画素ブロック数
            blockY < blocksY;
            ++blockY)
        {
            // 圧縮するブロック列番号
            for (std::uint32_t blockX = 0;
                // 横方向の4画素ブロック数
                blockX < blocksX;
                ++blockX)
            {
                ExtractBlock(
                    image,
                    blockX,
                    blockY,
                    block);
                // 書き込み先の画素列の位置
                auto* destination =
                    output.data()
                    + (static_cast<std::size_t>(blockY)
                        * blocksX
                        + blockX) * 16;
                EncodeChannelBlock(block, 3, destination);
                EncodeColorBlock(
                    block,
                    destination + 8);
            }
        }
        return output;
    }

    std::vector<std::uint8_t> CompressBC5(
        const CpuImage& image)
    {
        // 横方向の4画素ブロック数
        const std::uint32_t blocksX =
            BlockCount(image.width);
        // 縦方向の4画素ブロック数
        const std::uint32_t blocksY =
            BlockCount(image.height);
        // 圧縮したブロックのバイト列
        std::vector<std::uint8_t> output(
            static_cast<std::size_t>(blocksX)
            * blocksY
            * 16);
        // 4×4のRGBA画素列
        std::array<std::uint8_t, 64> block{};
        // 圧縮するブロック行番号
        for (std::uint32_t blockY = 0;
            // 縦方向の4画素ブロック数
            blockY < blocksY;
            ++blockY)
        {
            // 圧縮するブロック列番号
            for (std::uint32_t blockX = 0;
                // 横方向の4画素ブロック数
                blockX < blocksX;
                ++blockX)
            {
                ExtractBlock(
                    image,
                    blockX,
                    blockY,
                    block);
                // 書き込み先の画素列の位置
                auto* destination =
                    output.data()
                    + (static_cast<std::size_t>(blockY)
                        * blocksX
                        + blockX) * 16;
                // BC5は前半のBC4にR、後半にGを格納する。
                EncodeChannelBlock(block, 0, destination);
                EncodeChannelBlock(
                    block,
                    1,
                    destination + 8);
            }
        }
        return output;
    }

    DXGI_FORMAT ChooseTextureFormat(
        const std::vector<CpuImage>& mips,
        const bool compress,
        const TextureUsage usage) noexcept
    {

        // 先頭寸法がBC条件を満たすか
        // BC圧縮は先頭ミップの両辺が4の倍数の場合だけ選ぶ。
        const bool canCompress =
            compress
            && !mips.empty()
            && mips[0].width % 4 == 0
            && mips[0].height % 4 == 0;
        if (!canCompress)
        {
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        }
        switch (usage)
        {
        case TextureUsage::NormalMap:
            return DXGI_FORMAT_BC5_UNORM;
        case TextureUsage::DataMap:
            // データ画像はアルファを使わず、透過判定によらずBC1を選ぶ。
            return DXGI_FORMAT_BC1_UNORM;
        case TextureUsage::Color:
        default:
            return HasTransparentPixels(mips[0])
                ? DXGI_FORMAT_BC3_UNORM
                : DXGI_FORMAT_BC1_UNORM;
        }
    }

    // BC1・BC3・BC5のブロック長を返し、他形式には0を返す(format: 画像の形式)。
    [[nodiscard]] std::uint32_t BlockBytesFor(
        const DXGI_FORMAT format) noexcept
    {
        switch (format)
        {
        case DXGI_FORMAT_BC1_UNORM:
            return 8;
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC5_UNORM:
            return 16;
        default:
            return 0;
        }
    }

    // BC1・BC3・BC5に応じて1ミップを圧縮する(mip: 有効なRGBA8画像, format: 選択した圧縮形式)。
    [[nodiscard]] std::vector<std::uint8_t> CompressForFormat(
        const CpuImage& mip,
        const DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_BC1_UNORM:
            return CompressBC1(mip);
        case DXGI_FORMAT_BC3_UNORM:
            return CompressBC3(mip);
        case DXGI_FORMAT_BC5_UNORM:
            return CompressBC5(mip);
        default:
            return {};
        }
    }

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
        CreateTexture(
            ID3D11Device* device,
            const std::vector<CpuImage>& mips,
            const bool compress,
            const TextureUsage usage)
    {
        if (device == nullptr || mips.empty())
        {
            throw std::invalid_argument(
                "CreateTexture requires a device and mips.");
        }

        // 確定する画像の格納形式
        const DXGI_FORMAT format =
            ChooseTextureFormat(mips, compress, usage);
        // 1圧縮ブロックのバイト数
        const std::uint32_t blockBytes =
            BlockBytesFor(format);


        // GPU作成まで保持する圧縮列
        std::vector<std::vector<std::uint8_t>>
            compressedMips;
        // ミップごとのGPU初期データ
        std::vector<D3D11_SUBRESOURCE_DATA> initialData(
            mips.size());
        // ミップの番号または転送情報
        for (std::size_t level = 0;
            level < mips.size();
            ++level)
        {
            // 処理するミップ番号または画像
            const auto& mip = mips[level];
            if (blockBytes != 0)
            {
                compressedMips.push_back(
                    CompressForFormat(mip, format));
                // 横方向の4画素ブロック数
                const std::uint32_t blocksX =
                    BlockCount(mip.width);
                initialData[level].pSysMem =
                    compressedMips.back().data();
                // SysMemPitch: 圧縮mip rowあたりのbyte数。
                initialData[level].SysMemPitch =
                    blocksX * blockBytes;
            }
            else
            {
                initialData[level].pSysMem =
                    mip.pixels.data();
                initialData[level].SysMemPitch =
                    mip.width * 4;
            }
        }

        // テクスチャまたはビューの設定
        D3D11_TEXTURE2D_DESC description{};
        description.Width = mips[0].width;
        description.Height = mips[0].height;
        description.MipLevels =
            static_cast<UINT>(mips.size());
        description.ArraySize = 1;
        description.Format = format;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags =
            D3D11_BIND_SHADER_RESOURCE;

        // 作成した2Dテクスチャ
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        ThrowIfFailed(
            device->CreateTexture2D(
                &description,
                initialData.data(),
                texture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(async texture)");

        // 作成した画像ビュー
        Microsoft::WRL::ComPtr<
            ID3D11ShaderResourceView> view;
        ThrowIfFailed(
            device->CreateShaderResourceView(
                texture.Get(),
                nullptr,
                view.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateShaderResourceView(async texture)");
        return view;
    }

    PreparedTextureData PrepareTextureData(
        std::vector<CpuImage> mips,
        const bool compress,
        const TextureUsage usage)
    {
        if (mips.empty())
        {
            throw std::invalid_argument(
                "PrepareTextureData requires mips.");
        }

        // 用意する2Dミップ転送情報
        PreparedTextureData data;
        data.format =
            ChooseTextureFormat(mips, compress, usage);
        // 1圧縮ブロックのバイト数
        const std::uint32_t blockBytes =
            BlockBytesFor(data.format);
        data.levels.reserve(mips.size());
        // 処理するミップ番号または画像
        for (auto& mip : mips)
        {
            // ミップの番号または転送情報
            PreparedTextureLevel level;
            level.width = mip.width;
            level.height = mip.height;
            if (blockBytes != 0)
            {
                level.bytes =
                    CompressForFormat(mip, data.format);
                level.rowPitch =
                    BlockCount(mip.width) * blockBytes;
            }
            else
            {
                level.bytes = std::move(mip.pixels);
                level.rowPitch = mip.width * 4;
            }
            data.levels.push_back(std::move(level));
        }
        return data;
    }

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
        CreateTexture(
            ID3D11Device* device,
            const PreparedTextureData& data)
    {
        if (device == nullptr || data.levels.empty())
        {
            throw std::invalid_argument(
                "CreateTexture requires a device and levels.");
        }

        // ミップごとのGPU初期データ
        std::vector<D3D11_SUBRESOURCE_DATA> initialData(
            data.levels.size());
        // ミップの番号または転送情報
        for (std::size_t level = 0;
            level < data.levels.size();
            ++level)
        {
            initialData[level].pSysMem =
                data.levels[level].bytes.data();
            initialData[level].SysMemPitch =
                data.levels[level].rowPitch;
        }

        // テクスチャまたはビューの設定
        D3D11_TEXTURE2D_DESC description{};
        description.Width = data.levels[0].width;
        description.Height = data.levels[0].height;
        description.MipLevels =
            static_cast<UINT>(data.levels.size());
        description.ArraySize = 1;
        description.Format = data.format;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags =
            D3D11_BIND_SHADER_RESOURCE;

        // 作成した2Dテクスチャ
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        ThrowIfFailed(
            device->CreateTexture2D(
                &description,
                initialData.data(),
                texture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(prepared texture)");

        // 作成した画像ビュー
        Microsoft::WRL::ComPtr<
            ID3D11ShaderResourceView> view;
        ThrowIfFailed(
            device->CreateShaderResourceView(
                texture.Get(),
                nullptr,
                view.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateShaderResourceView(prepared texture)");
        return view;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D>
        CreateUploadableTexture(
            ID3D11Device* device,
            const PreparedTextureData& data)
    {
        if (device == nullptr || data.levels.empty())
        {
            throw std::invalid_argument(
                "CreateUploadableTexture requires a device and levels.");
        }

        // テクスチャまたはビューの設定
        D3D11_TEXTURE2D_DESC description{};
        description.Width = data.levels[0].width;
        description.Height = data.levels[0].height;
        description.MipLevels =
            static_cast<UINT>(data.levels.size());
        description.ArraySize = 1;
        description.Format = data.format;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags =
            D3D11_BIND_SHADER_RESOURCE;

        // 作成した2Dテクスチャ
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        ThrowIfFailed(
            device->CreateTexture2D(
                &description,
                nullptr,
                texture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(progressive texture)");
        return texture;
    }

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
        CreateTextureView(
            ID3D11Device* device,
            ID3D11Texture2D* texture,
            const DXGI_FORMAT format,
            const std::uint32_t mostDetailedMip,
            const std::uint32_t mipLevels)
    {
        if (device == nullptr
            || texture == nullptr
            || mostDetailedMip >= mipLevels)
        {
            throw std::invalid_argument(
                "CreateTextureView arguments are out of range.");
        }

        // テクスチャまたはビューの設定
        D3D11_SHADER_RESOURCE_VIEW_DESC description{};
        description.Format = format;
        description.ViewDimension =
            D3D11_SRV_DIMENSION_TEXTURE2D;
        description.Texture2D.MostDetailedMip =
            mostDetailedMip;
        description.Texture2D.MipLevels =
            mipLevels - mostDetailedMip;

        // 作成した画像ビュー
        Microsoft::WRL::ComPtr<
            ID3D11ShaderResourceView> view;
        ThrowIfFailed(
            device->CreateShaderResourceView(
                texture,
                &description,
                view.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateShaderResourceView(progressive view)");
        return view;
    }
}
