#include "LamaPon/Graphics/TemporalJitter.h"

#include <algorithm>

namespace LamaPon
{
    float HaltonSequence(
        const std::uint32_t index,
        const std::uint32_t base) noexcept
    {
        if (base < 2u)
        {
            return 0.0f;
        }
        // indexをbase進数で書いて、桁を逆順に並べた小数を作ります。
        float result = 0.0f;
        float fraction = 1.0f;
        std::uint32_t remaining = index;
        while (remaining > 0u)
        {
            fraction /= static_cast<float>(base);
            result += fraction
                * static_cast<float>(remaining % base);
            remaining /= base;
        }
        return result;
    }

    DirectX::XMFLOAT2 TemporalJitterOffset(
        const std::uint32_t index) noexcept
    {
        // 1周を8枚にします。Haltonは1から数えるのが慣習で、0を
        // 渡すと必ず(0,0)＝ずらさない回になってしまいます。
        constexpr std::uint32_t period = 8u;
        const std::uint32_t sample = index % period + 1u;
        return {
            HaltonSequence(sample, 2u) - 0.5f,
            HaltonSequence(sample, 3u) - 0.5f
        };
    }

    DirectX::XMMATRIX ApplyTemporalJitter(
        DirectX::FXMMATRIX projection,
        const DirectX::XMFLOAT2& jitterPixels,
        const std::uint32_t width,
        const std::uint32_t height) noexcept
    {
        using namespace DirectX;

        const float safeWidth = static_cast<float>(
            std::max(width, 1u));
        const float safeHeight = static_cast<float>(
            std::max(height, 1u));
        // ピクセル→クリップ空間。クリップ空間の幅は2なので、
        // 1ピクセルは2/幅にあたります。yは上下が逆です。
        const float clipX =
            jitterPixels.x * 2.0f / safeWidth;
        const float clipY =
            -jitterPixels.y * 2.0f / safeHeight;

        XMFLOAT4X4 stored{};
        XMStoreFloat4x4(&stored, projection);
        stored._31 += clipX;
        stored._32 += clipY;
        return XMLoadFloat4x4(&stored);
    }
}
