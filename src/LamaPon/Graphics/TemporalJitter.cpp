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

        // 逆順の桁から作る小数
        float result = 0.0f;
        // 次の桁の小数係数
        float fraction = 1.0f;
        // 未変換の整数部分
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
        // サンプル番号0の固定位置を避け、1から8を循環する。
        // ずらし列の周期フレーム数
        constexpr std::uint32_t period = 8u;
        // 1始まりのサンプル番号
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

        // ゼロを除いた画面幅
        const float safeWidth = static_cast<float>(
            std::max(width, 1u));
        // ゼロを除いた画面高さ
        const float safeHeight = static_cast<float>(
            std::max(height, 1u));
        // クリップ空間は幅2とし、画素座標とはYの向きが逆になる。
        // クリップ空間の横ずらし
        const float clipX =
            jitterPixels.x * 2.0f / safeWidth;
        // クリップ空間の縦ずらし
        const float clipY =
            -jitterPixels.y * 2.0f / safeHeight;

        // ずらしを加える射影行列
        XMFLOAT4X4 stored{};
        XMStoreFloat4x4(&stored, projection);
        stored._31 += clipX;
        stored._32 += clipY;
        return XMLoadFloat4x4(&stored);
    }
}
