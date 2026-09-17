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
        std::uint32_t size{};
        std::uint32_t flags{};
        std::uint32_t fourCc{};
        std::uint32_t rgbBitCount{};
        std::uint32_t redMask{};
        std::uint32_t greenMask{};
        std::uint32_t blueMask{};
        std::uint32_t alphaMask{};
    };

    struct DdsHeader final
    {
        std::uint32_t size{};
        std::uint32_t flags{};
        std::uint32_t height{};
        std::uint32_t width{};
        std::uint32_t pitchOrLinearSize{};
        std::uint32_t depth{};
        std::uint32_t mipMapCount{};
        std::array<std::uint32_t, 11> reserved{};
        DdsPixelFormat pixelFormat{};
        std::uint32_t caps{};
        std::uint32_t caps2{};
        std::uint32_t caps3{};
        std::uint32_t caps4{};
        std::uint32_t reserved2{};
    };

    struct DdsHeaderDx10 final
    {
        std::uint32_t format{};
        std::uint32_t resourceDimension{};
        std::uint32_t miscFlag{};
        std::uint32_t arraySize{};
        std::uint32_t miscFlags2{};
    };

    static_assert(sizeof(DdsPixelFormat) == 32u);
    static_assert(sizeof(DdsHeader) == 124u);
    static_assert(sizeof(DdsHeaderDx10) == 20u);

    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t>
        DdsLevelLayout(
            const DXGI_FORMAT format,
            const std::uint32_t width,
            const std::uint32_t height)
    {
        const auto layout = LamaPon::Detail::RequiredTextureLayout(
            format,
            width,
            height);
        return { layout.minimumRowBytes, layout.rowCount };
    }

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
        bool uninitialize{};

        ComScope()
        {
            const HRESULT result = CoInitializeEx(
                nullptr,
                COINIT_MULTITHREADED);
            uninitialize = SUCCEEDED(result);
        }

        ~ComScope()
        {
            if (uninitialize)
            {
                CoUninitialize();
            }
        }
    };

    // RGB888 → RGB565
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

    // 4x4ブロックをRGBA配列として取り出します（端はクランプ）。
    void ExtractBlock(
        const LamaPon::TextureLoader::CpuImage& image,
        const std::uint32_t blockX,
        const std::uint32_t blockY,
        std::array<std::uint8_t, 64>& block) noexcept
    {
        for (std::uint32_t row = 0; row < 4; ++row)
        {
            const std::uint32_t y = std::min(
                blockY * 4 + row,
                image.height - 1);
            for (std::uint32_t column = 0;
                column < 4;
                ++column)
            {
                const std::uint32_t x = std::min(
                    blockX * 4 + column,
                    image.width - 1);
                const std::size_t source =
                    (static_cast<std::size_t>(y)
                        * image.width
                        + x) * 4;
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

    // BC1のカラー部（8バイト）をエンコードします。
    // 端点はブロック内の輝度最小/最大ピクセルです。
    void EncodeColorBlock(
        const std::array<std::uint8_t, 64>& block,
        std::uint8_t* destination) noexcept
    {
        int brightest = -1;
        int darkest = 256 * 3 + 1;
        std::array<std::uint8_t, 3> endpointHigh{};
        std::array<std::uint8_t, 3> endpointLow{};
        for (int pixel = 0; pixel < 16; ++pixel)
        {
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

        std::uint16_t color0 = To565(
            endpointHigh[0],
            endpointHigh[1],
            endpointHigh[2]);
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

        // パレット4色（2端点＋2内分点）
        std::array<std::array<int, 3>, 4> palette{};
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

        std::uint32_t indices = 0;
        // color0 == color1は3色＋透明モードになり、インデックス
        // 3が透明黒として描画されてしまうため、全ピクセルを
        // 端点0（indices=0のまま）にします。
        if (color0 != color1)
        {
            for (int pixel = 15; pixel >= 0; --pixel)
            {
                int bestIndex = 0;
                int bestDistance =
                    std::numeric_limits<int>::max();
                for (int candidate = 0;
                    candidate < 4;
                    ++candidate)
                {
                    int distance = 0;
                    for (int channel = 0;
                        channel < 3;
                        ++channel)
                    {
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

    // 1チャンネル分のBC4ブロック（8バイト）をエンコードします。
    // BC3のアルファ部とBC5のR/G部は同じ符号化なので、見るチャンネルを
    // 引数にして共用します（channelは0=R, 1=G, 2=B, 3=A）。
    void EncodeChannelBlock(
        const std::array<std::uint8_t, 64>& block,
        const int channel,
        std::uint8_t* destination) noexcept
    {
        std::uint8_t alphaHigh = 0;
        std::uint8_t alphaLow = 255;
        for (int pixel = 0; pixel < 16; ++pixel)
        {
            const std::uint8_t alpha =
                block[pixel * 4 + channel];
            alphaHigh = std::max(alphaHigh, alpha);
            alphaLow = std::min(alphaLow, alpha);
        }
        destination[0] = alphaHigh;
        destination[1] = alphaLow;

        // 8段階の補間パレット（alpha0 > alpha1モード）
        std::array<int, 8> palette{};
        palette[0] = alphaHigh;
        palette[1] = alphaLow;
        for (int step = 1; step <= 6; ++step)
        {
            palette[static_cast<std::size_t>(step) + 1] =
                ((7 - step) * alphaHigh
                    + step * alphaLow) / 7;
        }

        std::uint64_t indices = 0;
        for (int pixel = 15; pixel >= 0; --pixel)
        {
            const int alpha = block[pixel * 4 + channel];
            int bestIndex = 0;
            int bestDistance =
                std::numeric_limits<int>::max();
            for (int candidate = 0;
                candidate < 8;
                ++candidate)
            {
                const int delta =
                    alpha - palette[candidate];
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
        for (int byte = 0; byte < 6; ++byte)
        {
            destination[2 + byte] =
                static_cast<std::uint8_t>(
                    (indices >> (byte * 8)) & 0xffu);
        }
    }

    [[nodiscard]] std::uint32_t BlockCount(
        const std::uint32_t size) noexcept
    {
        return std::max((size + 3) / 4, 1u);
    }

    constexpr std::uint32_t DdsMagic = MakeFourCc('D', 'D', 'S', ' ');
    constexpr std::uint32_t PixelFormatFourCc = 0x4u;
    // caps2のDDSCAPS2_CUBEMAPと6面すべての印です。
    constexpr std::uint32_t Caps2CubeMapMask = 0xfe00u;
    constexpr std::uint32_t ResourceMiscTextureCube = 0x4u;

    // DDSのresource次元を保ち、D3D11／D3D12のsubresource順へ展開します。
    [[nodiscard]] LamaPon::TextureLoader::PreparedDdsTextureData
        ParseDdsResourceData(const std::span<const std::uint8_t> bytes)
    {
        constexpr std::uint32_t PixelFormatRgb = 0x40u;
        constexpr std::uint32_t PixelFormatLuminance = 0x20000u;
        constexpr std::uint32_t Caps2Volume = 0x200000u;
        constexpr std::uint32_t ResourceDimensionTexture2D = 3u;
        constexpr std::uint32_t ResourceDimensionTexture3D = 4u;
        constexpr std::uint32_t MaximumTextureDimension = 16384u;

        if (bytes.size() < sizeof(std::uint32_t) + sizeof(DdsHeader))
        {
            throw std::invalid_argument("The DDS header is incomplete.");
        }
        std::uint32_t magic{};
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
        // 古い形式のcubeはcaps2へDDSCAPS2_CUBEMAPと6面の印を持ちます。
        // 面が欠けたcubeはD3D11のDirectXTKと同じく読みません。
        bool fileCube = (header.caps2 & Caps2CubeMapMask) != 0u;
        bool fileVolume = (header.caps2 & Caps2Volume) != 0u;
        std::uint32_t arraySize = 1u;
        if (fileCube
            && (header.caps2 & Caps2CubeMapMask) != Caps2CubeMapMask)
        {
            throw std::invalid_argument(
                "DDS cubes without all six faces are not supported.");
        }

        std::size_t payloadOffset = sizeof(magic) + sizeof(header);
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
            // D3D9のD3DFORMAT値をFourCC欄へ直接保存する旧DDSです。
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
        else if ((header.pixelFormat.flags & PixelFormatRgb) != 0u
            && header.pixelFormat.rgbBitCount == 32u
            && header.pixelFormat.greenMask == 0x0000ff00u)
        {
            if (header.pixelFormat.redMask == 0x000000ffu
                && header.pixelFormat.blueMask == 0x00ff0000u
                && header.pixelFormat.alphaMask == 0xff000000u)
            {
                format = DXGI_FORMAT_R8G8B8A8_UNORM;
            }
            else if (header.pixelFormat.redMask == 0x00ff0000u
                && header.pixelFormat.blueMask == 0x000000ffu)
            {
                format = header.pixelFormat.alphaMask == 0xff000000u
                    ? DXGI_FORMAT_B8G8R8A8_UNORM
                    : header.pixelFormat.alphaMask == 0u
                        ? DXGI_FORMAT_B8G8R8X8_UNORM
                        : DXGI_FORMAT_UNKNOWN;
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
                && header.pixelFormat.redMask == 0x00ffu
                && header.pixelFormat.alphaMask == 0xff00u)
            {
                format = DXGI_FORMAT_R8G8_UNORM;
            }
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

        const std::uint32_t mipCount = std::max(header.mipMapCount, 1u);
        std::uint32_t maximumMipCount = 1u;
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
        LamaPon::TextureLoader::PreparedDdsTextureData result;
        result.format = format;
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
        std::size_t offset = payloadOffset;
        for (std::uint32_t slice{}; slice < sliceCount; ++slice)
        {
            for (std::uint32_t mip{}; mip < mipCount; ++mip)
            {
                const auto width = std::max(header.width >> mip, 1u);
                const auto height = std::max(header.height >> mip, 1u);
                const auto [rowPitch, rowCount] =
                    DdsLevelLayout(format, width, height);
                const auto depth = fileVolume
                    ? std::max(header.depth >> mip, 1u)
                    : 1u;
                const std::uint64_t levelBytes64 =
                    static_cast<std::uint64_t>(rowPitch) * rowCount * depth;
                if (offset > bytes.size()
                    || levelBytes64 > std::numeric_limits<std::size_t>::max()
                    || levelBytes64 > bytes.size() - offset)
                {
                    throw std::invalid_argument(
                        "The DDS mip data is incomplete.");
                }
                const auto levelBytes =
                    static_cast<std::size_t>(levelBytes64);
                LamaPon::TextureLoader::PreparedTextureLevel level;
                level.width = width;
                level.height = height;
                level.rowPitch = rowPitch;
                level.bytes.assign(
                    bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                    bytes.begin()
                        + static_cast<std::ptrdiff_t>(offset + levelBytes));
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
        const std::size_t headerOffset = sizeof(std::uint32_t);
        if (bytes.size() < headerOffset + sizeof(DdsHeader))
        {
            return false;
        }
        std::uint32_t magic{};
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
        const std::size_t extendedOffset = headerOffset + sizeof(header);
        if ((header.pixelFormat.flags & PixelFormatFourCc) == 0u
            || header.pixelFormat.fourCc != MakeFourCc('D', 'X', '1', '0')
            || bytes.size() < extendedOffset + sizeof(DdsHeaderDx10))
        {
            return false;
        }
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
        const ComScope comScope;

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

        // 入力バイト列をコピーせず参照するWICストリームを作ります。
        // デコードはこの関数内で完了するため、参照先は処理中存続します。
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

        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        ThrowIfFailed(
            factory->CreateDecoderFromStream(
                stream.Get(),
                nullptr,
                WICDecodeMetadataCacheOnDemand,
                decoder.ReleaseAndGetAddressOf()),
            "IWICImagingFactory::CreateDecoderFromStream");

        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode>
            frame;
        ThrowIfFailed(
            decoder->GetFrame(
                0,
                frame.ReleaseAndGetAddressOf()),
            "IWICBitmapDecoder::GetFrame");

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
        std::vector<CpuImage> mips;
        mips.push_back(std::move(base));
        while (mips.back().width > 1
            || mips.back().height > 1)
        {
            const auto& previous = mips.back();
            CpuImage next;
            next.width =
                std::max(previous.width / 2, 1u);
            next.height =
                std::max(previous.height / 2, 1u);
            next.pixels.resize(
                static_cast<std::size_t>(next.width)
                * next.height
                * 4);
            for (std::uint32_t y = 0;
                y < next.height;
                ++y)
            {
                const std::uint32_t sourceY0 =
                    std::min(
                        y * 2,
                        previous.height - 1);
                const std::uint32_t sourceY1 =
                    std::min(
                        y * 2 + 1,
                        previous.height - 1);
                for (std::uint32_t x = 0;
                    x < next.width;
                    ++x)
                {
                    const std::uint32_t sourceX0 =
                        std::min(
                            x * 2,
                            previous.width - 1);
                    const std::uint32_t sourceX1 =
                        std::min(
                            x * 2 + 1,
                            previous.width - 1);
                    for (int channel = 0;
                        channel < 4;
                        ++channel)
                    {
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
        const std::uint32_t blocksX =
            BlockCount(image.width);
        const std::uint32_t blocksY =
            BlockCount(image.height);
        std::vector<std::uint8_t> output(
            static_cast<std::size_t>(blocksX)
            * blocksY
            * 8);
        std::array<std::uint8_t, 64> block{};
        for (std::uint32_t blockY = 0;
            blockY < blocksY;
            ++blockY)
        {
            for (std::uint32_t blockX = 0;
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
        const std::uint32_t blocksX =
            BlockCount(image.width);
        const std::uint32_t blocksY =
            BlockCount(image.height);
        std::vector<std::uint8_t> output(
            static_cast<std::size_t>(blocksX)
            * blocksY
            * 16);
        std::array<std::uint8_t, 64> block{};
        for (std::uint32_t blockY = 0;
            blockY < blocksY;
            ++blockY)
        {
            for (std::uint32_t blockX = 0;
                blockX < blocksX;
                ++blockX)
            {
                ExtractBlock(
                    image,
                    blockX,
                    blockY,
                    block);
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
        const std::uint32_t blocksX =
            BlockCount(image.width);
        const std::uint32_t blocksY =
            BlockCount(image.height);
        std::vector<std::uint8_t> output(
            static_cast<std::size_t>(blocksX)
            * blocksY
            * 16);
        std::array<std::uint8_t, 64> block{};
        for (std::uint32_t blockY = 0;
            blockY < blocksY;
            ++blockY)
        {
            for (std::uint32_t blockX = 0;
                blockX < blocksX;
                ++blockX)
            {
                ExtractBlock(
                    image,
                    blockX,
                    blockY,
                    block);
                auto* destination =
                    output.data()
                    + (static_cast<std::size_t>(blockY)
                        * blocksX
                        + blockX) * 16;
                // BC5はBC4ブロック2つ。先がR、後がGです。
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
        // BCフォーマットはトップレベルの寸法が4の倍数である
        // 必要があるため、満たさない画像は非圧縮のままにします。
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
            // 粗さ、金属度、遮蔽ではアルファを使わないため、
            // 透過の有無にかかわらずBC1を使用します。
            return DXGI_FORMAT_BC1_UNORM;
        case TextureUsage::Color:
        default:
            return HasTransparentPixels(mips[0])
                ? DXGI_FORMAT_BC3_UNORM
                : DXGI_FORMAT_BC1_UNORM;
        }
    }

    // フォーマットからブロック1つ分のバイト数を返します
    // （非圧縮なら0）。
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

    // フォーマットに合わせて1ミップを圧縮します。
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

        const DXGI_FORMAT format =
            ChooseTextureFormat(mips, compress, usage);
        const std::uint32_t blockBytes =
            BlockBytesFor(format);

        // 圧縮データはCreateTexture2Dまで生存が必要です。
        std::vector<std::vector<std::uint8_t>>
            compressedMips;
        std::vector<D3D11_SUBRESOURCE_DATA> initialData(
            mips.size());
        for (std::size_t level = 0;
            level < mips.size();
            ++level)
        {
            const auto& mip = mips[level];
            if (blockBytes != 0)
            {
                compressedMips.push_back(
                    CompressForFormat(mip, format));
                const std::uint32_t blocksX =
                    BlockCount(mip.width);
                initialData[level].pSysMem =
                    compressedMips.back().data();
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

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        ThrowIfFailed(
            device->CreateTexture2D(
                &description,
                initialData.data(),
                texture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(async texture)");

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

        PreparedTextureData data;
        data.format =
            ChooseTextureFormat(mips, compress, usage);
        const std::uint32_t blockBytes =
            BlockBytesFor(data.format);
        data.levels.reserve(mips.size());
        for (auto& mip : mips)
        {
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

        std::vector<D3D11_SUBRESOURCE_DATA> initialData(
            data.levels.size());
        for (std::size_t level = 0;
            level < data.levels.size();
            ++level)
        {
            initialData[level].pSysMem =
                data.levels[level].bytes.data();
            initialData[level].SysMemPitch =
                data.levels[level].rowPitch;
        }

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

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        ThrowIfFailed(
            device->CreateTexture2D(
                &description,
                initialData.data(),
                texture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(prepared texture)");

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

        D3D11_SHADER_RESOURCE_VIEW_DESC description{};
        description.Format = format;
        description.ViewDimension =
            D3D11_SRV_DIMENSION_TEXTURE2D;
        description.Texture2D.MostDetailedMip =
            mostDetailedMip;
        description.Texture2D.MipLevels =
            mipLevels - mostDetailedMip;

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
