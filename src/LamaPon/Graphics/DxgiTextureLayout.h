#pragma once

// D3D11 / D3D12 Backendが共有する、DXGI texture formatへの変換と
// CPU側subresource範囲の検証です。Runtime内部headerで、SDKにはinstallしません。
#include "LamaPon/Graphics/GraphicsResource.h"

#include <dxgiformat.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace LamaPon::Detail
{
    [[nodiscard]] inline DXGI_FORMAT ToDxgiTextureFormat(
        const GraphicsTextureFormat format)
    {
        switch (format)
        {
        case GraphicsTextureFormat::Rgba8Unorm:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case GraphicsTextureFormat::Bgra8Unorm:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case GraphicsTextureFormat::Bc1Unorm:
            return DXGI_FORMAT_BC1_UNORM;
        case GraphicsTextureFormat::Bc3Unorm:
            return DXGI_FORMAT_BC3_UNORM;
        case GraphicsTextureFormat::Bc5Unorm:
            return DXGI_FORMAT_BC5_UNORM;
        case GraphicsTextureFormat::Rgba16Float:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case GraphicsTextureFormat::R8Unorm:
            return DXGI_FORMAT_R8_UNORM;
        case GraphicsTextureFormat::Rg8Unorm:
            return DXGI_FORMAT_R8G8_UNORM;
        case GraphicsTextureFormat::Rgba8UnormSrgb:
            return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case GraphicsTextureFormat::Bgra8UnormSrgb:
            return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case GraphicsTextureFormat::Bgrx8Unorm:
            return DXGI_FORMAT_B8G8R8X8_UNORM;
        case GraphicsTextureFormat::Bgrx8UnormSrgb:
            return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
        case GraphicsTextureFormat::Bc1UnormSrgb:
            return DXGI_FORMAT_BC1_UNORM_SRGB;
        case GraphicsTextureFormat::Bc2Unorm:
            return DXGI_FORMAT_BC2_UNORM;
        case GraphicsTextureFormat::Bc2UnormSrgb:
            return DXGI_FORMAT_BC2_UNORM_SRGB;
        case GraphicsTextureFormat::Bc3UnormSrgb:
            return DXGI_FORMAT_BC3_UNORM_SRGB;
        case GraphicsTextureFormat::Bc4Unorm:
            return DXGI_FORMAT_BC4_UNORM;
        case GraphicsTextureFormat::Bc4Snorm:
            return DXGI_FORMAT_BC4_SNORM;
        case GraphicsTextureFormat::Bc5Snorm:
            return DXGI_FORMAT_BC5_SNORM;
        case GraphicsTextureFormat::Bc6hUf16:
            return DXGI_FORMAT_BC6H_UF16;
        case GraphicsTextureFormat::Bc6hSf16:
            return DXGI_FORMAT_BC6H_SF16;
        case GraphicsTextureFormat::Bc7Unorm:
            return DXGI_FORMAT_BC7_UNORM;
        case GraphicsTextureFormat::Bc7UnormSrgb:
            return DXGI_FORMAT_BC7_UNORM_SRGB;
        case GraphicsTextureFormat::R16Float:
            return DXGI_FORMAT_R16_FLOAT;
        case GraphicsTextureFormat::Rg16Float:
            return DXGI_FORMAT_R16G16_FLOAT;
        case GraphicsTextureFormat::R32Float:
            return DXGI_FORMAT_R32_FLOAT;
        case GraphicsTextureFormat::Rg32Float:
            return DXGI_FORMAT_R32G32_FLOAT;
        case GraphicsTextureFormat::Rgba32Float:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case GraphicsTextureFormat::R10g10b10a2Unorm:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case GraphicsTextureFormat::Rg16Unorm:
            return DXGI_FORMAT_R16G16_UNORM;
        case GraphicsTextureFormat::B5g5r5a1Unorm:
            return DXGI_FORMAT_B5G5R5A1_UNORM;
        case GraphicsTextureFormat::B5g6r5Unorm:
            return DXGI_FORMAT_B5G6R5_UNORM;
        case GraphicsTextureFormat::B4g4r4a4Unorm:
            return DXGI_FORMAT_B4G4R4A4_UNORM;
        case GraphicsTextureFormat::R16Unorm:
            return DXGI_FORMAT_R16_UNORM;
        case GraphicsTextureFormat::A8Unorm:
            return DXGI_FORMAT_A8_UNORM;
        case GraphicsTextureFormat::Rg8Snorm:
            return DXGI_FORMAT_R8G8_SNORM;
        case GraphicsTextureFormat::Rgba8Snorm:
            return DXGI_FORMAT_R8G8B8A8_SNORM;
        case GraphicsTextureFormat::Rg16Snorm:
            return DXGI_FORMAT_R16G16_SNORM;
        case GraphicsTextureFormat::Rgba16Unorm:
            return DXGI_FORMAT_R16G16B16A16_UNORM;
        case GraphicsTextureFormat::Rgba16Snorm:
            return DXGI_FORMAT_R16G16B16A16_SNORM;
        case GraphicsTextureFormat::R8g8B8g8Unorm:
            return DXGI_FORMAT_R8G8_B8G8_UNORM;
        case GraphicsTextureFormat::G8r8G8b8Unorm:
            return DXGI_FORMAT_G8R8_G8B8_UNORM;
        case GraphicsTextureFormat::R8Snorm:
            return DXGI_FORMAT_R8_SNORM;
        case GraphicsTextureFormat::R16Snorm:
            return DXGI_FORMAT_R16_SNORM;
        case GraphicsTextureFormat::R11g11b10Float:
            return DXGI_FORMAT_R11G11B10_FLOAT;
        case GraphicsTextureFormat::R9g9b9e5SharedExp:
            return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
        default:
            throw std::invalid_argument(
                "Unsupported graphics texture format.");
        }
    }

    [[nodiscard]] inline std::uint32_t MaximumTextureMipLevels(
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t depth = 1) noexcept
    {
        std::uint32_t levels = 1;
        while (width > 1 || height > 1 || depth > 1)
        {
            width = std::max(width / 2, 1u);
            height = std::max(height / 2, 1u);
            depth = std::max(depth / 2, 1u);
            ++levels;
        }
        return levels;
    }

    struct TextureSubresourceLayout final
    {
        std::uint32_t minimumRowBytes{};
        std::uint32_t rowCount{};
    };

    [[nodiscard]] inline TextureSubresourceLayout RequiredTextureLayout(
        const DXGI_FORMAT format,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        const auto uncompressed = [width, height](
            const std::uint32_t bytesPerPixel)
        {
            if (width
                > std::numeric_limits<std::uint32_t>::max()
                    / bytesPerPixel)
            {
                throw std::invalid_argument(
                    "The texture row pitch cannot be represented.");
            }
            return TextureSubresourceLayout{
                width * bytesPerPixel,
                height
            };
        };
        switch (format)
        {
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_A8_UNORM:
        case DXGI_FORMAT_R8_SNORM:
            return uncompressed(1u);
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R8G8_SNORM:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_R16_SNORM:
        case DXGI_FORMAT_B5G5R5A1_UNORM:
        case DXGI_FORMAT_B5G6R5_UNORM:
        case DXGI_FORMAT_B4G4R4A4_UNORM:
            return uncompressed(2u);
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
        case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R16G16_SNORM:
        case DXGI_FORMAT_R32_FLOAT:
            return uncompressed(4u);
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R32G32_FLOAT:
            return uncompressed(8u);
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            return uncompressed(16u);
        case DXGI_FORMAT_R8G8_B8G8_UNORM:
        case DXGI_FORMAT_G8R8_G8B8_UNORM:
        case DXGI_FORMAT_YUY2:
            if (width > std::numeric_limits<std::uint32_t>::max() - 1u
                || (width + 1u) / 2u
                    > std::numeric_limits<std::uint32_t>::max() / 4u)
            {
                throw std::invalid_argument(
                    "The packed texture row pitch cannot be represented.");
            }
            return {
                ((width + 1u) / 2u) * 4u,
                height
            };
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            return {
                std::max((width + 3u) / 4u, 1u) * 8u,
                std::max((height + 3u) / 4u, 1u)
            };
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return {
                std::max((width + 3u) / 4u, 1u) * 16u,
                std::max((height + 3u) / 4u, 1u)
            };
        default:
            throw std::invalid_argument(
                "The texture format has no upload layout.");
        }
    }

    // 2D textureの1 mip分として渡されたCPU dataが、row pitchとbyte範囲の
    // 両方で必要量を満たすか検証します。slicePitchが0ならbytes全体を使います。
    inline void ValidateTexture2DSubresourceData(
        const DXGI_FORMAT format,
        const std::uint32_t width,
        const std::uint32_t height,
        const std::uint32_t mipLevels,
        const std::uint32_t mipLevel,
        const GraphicsTextureSubresourceData& data)
    {
        if (mipLevel >= mipLevels
            || data.bytes.empty()
            || data.rowPitch == 0
            || (data.slicePitch == 0
                && data.bytes.size()
                    > std::numeric_limits<std::uint32_t>::max()))
        {
            throw std::invalid_argument(
                "The texture subresource data is incomplete.");
        }

        const auto mipWidth = std::max(width >> mipLevel, 1u);
        const auto mipHeight = std::max(height >> mipLevel, 1u);
        const auto layout = RequiredTextureLayout(
            format,
            mipWidth,
            mipHeight);
        if (data.rowPitch < layout.minimumRowBytes)
        {
            throw std::invalid_argument(
                "The texture subresource row pitch is too small.");
        }
        const auto requiredBytes =
            static_cast<std::uint64_t>(data.rowPitch)
                * (layout.rowCount - 1u)
            + layout.minimumRowBytes;
        const auto slicePitch = data.slicePitch != 0
            ? static_cast<std::uint64_t>(data.slicePitch)
            : static_cast<std::uint64_t>(data.bytes.size());
        if (requiredBytes > data.bytes.size()
            || requiredBytes > slicePitch
            || slicePitch > data.bytes.size())
        {
            throw std::invalid_argument(
                "The texture subresource byte range is too small.");
        }
    }

    // Texture3Dの1 mip分として渡されたCPU dataが、row pitch、slice pitch、
    // byte範囲のすべてで必要量を満たすか検証します。slicePitchは省略できません。
    inline void ValidateTexture3DSubresourceData(
        const DXGI_FORMAT format,
        const std::uint32_t width,
        const std::uint32_t height,
        const std::uint32_t depth,
        const std::uint32_t mipLevels,
        const std::uint32_t mipLevel,
        const GraphicsTextureSubresourceData& data)
    {
        if (mipLevel >= mipLevels
            || data.bytes.empty()
            || data.rowPitch == 0
            || data.slicePitch == 0)
        {
            throw std::invalid_argument(
                "The Texture3D subresource data is incomplete.");
        }

        const auto mipWidth = std::max(width >> mipLevel, 1u);
        const auto mipHeight = std::max(height >> mipLevel, 1u);
        const auto mipDepth = std::max(depth >> mipLevel, 1u);
        const auto layout = RequiredTextureLayout(
            format,
            mipWidth,
            mipHeight);
        if (data.rowPitch < layout.minimumRowBytes)
        {
            throw std::invalid_argument(
                "The Texture3D row pitch is too small.");
        }
        const auto requiredSliceBytes =
            static_cast<std::uint64_t>(data.rowPitch)
                * (layout.rowCount - 1u)
            + layout.minimumRowBytes;
        const auto requiredBytes =
            static_cast<std::uint64_t>(data.slicePitch)
                * (mipDepth - 1u)
            + requiredSliceBytes;
        if (requiredSliceBytes > data.slicePitch
            || data.slicePitch > data.bytes.size()
            || requiredBytes > data.bytes.size())
        {
            throw std::invalid_argument(
                "The Texture3D subresource byte range is too small.");
        }
    }
}
