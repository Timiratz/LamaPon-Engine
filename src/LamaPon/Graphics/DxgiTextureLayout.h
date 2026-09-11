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
        switch (format)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            if (width > std::numeric_limits<std::uint32_t>::max() / 4u)
            {
                throw std::invalid_argument(
                    "The texture row pitch cannot be represented.");
            }
            return { width * 4u, height };
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            if (width > std::numeric_limits<std::uint32_t>::max() / 8u)
            {
                throw std::invalid_argument(
                    "The texture row pitch cannot be represented.");
            }
            return { width * 8u, height };
        case DXGI_FORMAT_BC1_UNORM:
            return {
                std::max((width + 3u) / 4u, 1u) * 8u,
                std::max((height + 3u) / 4u, 1u)
            };
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC5_UNORM:
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
}
