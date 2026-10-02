#pragma once

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

// HLSLと同じ値を返す、CPU側のノイズ関数です。
// 実装変更時はassets/shaders/LamaPonNoise.hlsliも更新し、CPU/GPUの一致を維持します。
// uint32_tの桁あふれ、上位24ビットの変換、5次補間、floor後のint変換をHLSLと揃えます。
namespace LamaPon::Noise
{
    // 整数格子のハッシュを求めます(value: 格子点から作った整数値)。
    [[nodiscard]] inline std::uint32_t Hash(
        std::uint32_t value) noexcept
    {
        value ^= value >> 16;
        value *= 0x7feb352du;
        value ^= value >> 15;
        value *= 0x846ca68bu;
        value ^= value >> 16;
        return value;
    }

    // ハッシュの上位24ビットを0以上1未満へ変換します(hashed: ハッシュ値)。
    [[nodiscard]] inline float ToFloat(
        const std::uint32_t hashed) noexcept
    {
        return static_cast<float>(hashed >> 8)
            / 16777216.0f;
    }

    // 1次元格子点のノイズ値を返します(x: 整数格子のX座標)。
    [[nodiscard]] inline float Grid1(
        const std::int32_t x) noexcept
    {
        return ToFloat(
            Hash(static_cast<std::uint32_t>(x)
                * 0x9e3779b9u));
    }

    // 2次元格子点のノイズ値を返します(x: 整数格子のX座標, y: 整数格子のY座標)。
    [[nodiscard]] inline float Grid2(
        const std::int32_t x,
        const std::int32_t y) noexcept
    {
        return ToFloat(
            Hash(static_cast<std::uint32_t>(x)
                    * 0x9e3779b9u
                + static_cast<std::uint32_t>(y)
                    * 0x85ebca6bu));
    }

    // 3次元格子点のノイズ値を返します(x: 整数格子のX座標, y: 整数格子のY座標, z: 整数格子のZ座標)。
    [[nodiscard]] inline float Grid3(
        const std::int32_t x,
        const std::int32_t y,
        const std::int32_t z) noexcept
    {
        return ToFloat(
            Hash(static_cast<std::uint32_t>(x)
                    * 0x9e3779b9u
                + static_cast<std::uint32_t>(y)
                    * 0x85ebca6bu
                + static_cast<std::uint32_t>(z)
                    * 0xc2b2ae35u));
    }

    // 5次補間の重みを返します(t: 格子内の位置、0～1)。
    [[nodiscard]] inline float Fade(
        const float t) noexcept
    {
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }

    // 2値を線形補間します(a: 始点の値, b: 終点の値, t: 補間率)。
    [[nodiscard]] inline float Lerp(
        const float a,
        const float b,
        const float t) noexcept
    {
        return a + (b - a) * t;
    }

    // 1次元Valueノイズを返します(x: サンプル座標)。
    [[nodiscard]] inline float Value1D(
        const float x) noexcept
    {
        // サンプル座標の格子原点
        const float floored = std::floor(x);
        // 格子点のX座標
        const auto ix = static_cast<std::int32_t>(floored);
        // X方向の補間重み
        const float t = Fade(x - floored);
        return Lerp(Grid1(ix), Grid1(ix + 1), t);
    }

    // 2次元Valueノイズを返します(x: X座標, y: Y座標)。
    [[nodiscard]] inline float Value2D(
        const float x,
        const float y) noexcept
    {
        // X方向の格子原点
        const float flooredX = std::floor(x);
        // Y方向の格子原点
        const float flooredY = std::floor(y);
        // 格子点のX座標
        const auto ix =
            static_cast<std::int32_t>(flooredX);
        // 格子点のY座標
        const auto iy =
            static_cast<std::int32_t>(flooredY);
        // X方向の補間重み
        const float tx = Fade(x - flooredX);
        // Y方向の補間重み
        const float ty = Fade(y - flooredY);
        // Y方向の下側での補間値
        const float bottom = Lerp(
            Grid2(ix, iy),
            Grid2(ix + 1, iy),
            tx);
        // Y方向の上側での補間値
        const float top = Lerp(
            Grid2(ix, iy + 1),
            Grid2(ix + 1, iy + 1),
            tx);
        return Lerp(bottom, top, ty);
    }

    // 3次元Valueノイズを返します(x: X座標, y: Y座標, z: Z座標)。
    [[nodiscard]] inline float Value3D(
        const float x,
        const float y,
        const float z) noexcept
    {
        // X方向の格子原点
        const float flooredX = std::floor(x);
        // Y方向の格子原点
        const float flooredY = std::floor(y);
        // Z方向の格子原点
        const float flooredZ = std::floor(z);
        // 格子点のX座標
        const auto ix =
            static_cast<std::int32_t>(flooredX);
        // 格子点のY座標
        const auto iy =
            static_cast<std::int32_t>(flooredY);
        // 格子点のZ座標
        const auto iz =
            static_cast<std::int32_t>(flooredZ);
        // X方向の補間重み
        const float tx = Fade(x - flooredX);
        // Y方向の補間重み
        const float ty = Fade(y - flooredY);
        // Z方向の補間重み
        const float tz = Fade(z - flooredZ);

        // 手前かつ下側の補間値
        const float z0Bottom = Lerp(
            Grid3(ix, iy, iz),
            Grid3(ix + 1, iy, iz),
            tx);
        // 手前かつ上側の補間値
        const float z0Top = Lerp(
            Grid3(ix, iy + 1, iz),
            Grid3(ix + 1, iy + 1, iz),
            tx);
        // 奥かつ下側の補間値
        const float z1Bottom = Lerp(
            Grid3(ix, iy, iz + 1),
            Grid3(ix + 1, iy, iz + 1),
            tx);
        // 奥かつ上側の補間値
        const float z1Top = Lerp(
            Grid3(ix, iy + 1, iz + 1),
            Grid3(ix + 1, iy + 1, iz + 1),
            tx);
        return Lerp(
            Lerp(z0Bottom, z0Top, ty),
            Lerp(z1Bottom, z1Top, ty),
            tz);
    }

    // 格子点に割り当てた8方向の単位勾配を返します(x: 整数格子のX座標, y: 整数格子のY座標)。
    [[nodiscard]] inline DirectX::XMFLOAT2 Gradient2(
        const std::int32_t x,
        const std::int32_t y) noexcept
    {
        // 格子点に割り当てる方向番号
        const std::uint32_t hashed =
            Hash(static_cast<std::uint32_t>(x)
                    * 0x9e3779b9u
                + static_cast<std::uint32_t>(y)
                    * 0x85ebca6bu)
            & 7u;
        // 対角勾配の各軸成分
        constexpr float diagonal = 0.70710678f;
        switch (hashed)
        {
        case 0u: return { 1.0f, 0.0f };
        case 1u: return { -1.0f, 0.0f };
        case 2u: return { 0.0f, 1.0f };
        case 3u: return { 0.0f, -1.0f };
        case 4u: return { diagonal, diagonal };
        case 5u: return { -diagonal, diagonal };
        case 6u: return { diagonal, -diagonal };
        default: return { -diagonal, -diagonal };
        }
    }

    // 0～1の2次元Perlinノイズを返します(x: X座標, y: Y座標)。
    [[nodiscard]] inline float Perlin2D(
        const float x,
        const float y) noexcept
    {
        // X方向の格子原点
        const float flooredX = std::floor(x);
        // Y方向の格子原点
        const float flooredY = std::floor(y);
        // 格子点のX座標
        const auto ix =
            static_cast<std::int32_t>(flooredX);
        // 格子点のY座標
        const auto iy =
            static_cast<std::int32_t>(flooredY);
        // 格子原点からのX方向の距離
        const float fx = x - flooredX;
        // 格子原点からのY方向の距離
        const float fy = y - flooredY;

        // 勾配と変位の内積を返します(gradient: 格子点の勾配, dx: X方向の変位, dy: Y方向の変位)。
        // 勾配と変位の内積を返す関数
        const auto dotGradient =
            [](const DirectX::XMFLOAT2& gradient,
                const float dx,
                const float dy) noexcept
            {
                return gradient.x * dx + gradient.y * dy;
            };

        // 左下の勾配と変位の内積
        const float bottomLeft = dotGradient(
            Gradient2(ix, iy), fx, fy);
        // 右下の勾配と変位の内積
        const float bottomRight = dotGradient(
            Gradient2(ix + 1, iy), fx - 1.0f, fy);
        // 左上の勾配と変位の内積
        const float topLeft = dotGradient(
            Gradient2(ix, iy + 1), fx, fy - 1.0f);
        // 右上の勾配と変位の内積
        const float topRight = dotGradient(
            Gradient2(ix + 1, iy + 1),
            fx - 1.0f,
            fy - 1.0f);

        // X方向の補間重み
        const float tx = Fade(fx);
        // Y方向の補間重み
        const float ty = Fade(fy);
        // 勾配の内積を補間した値
        const float value = Lerp(
            Lerp(bottomLeft, bottomRight, tx),
            Lerp(topLeft, topRight, tx),
            ty);
        return std::clamp(
            value * 0.7071f + 0.5f,
            0.0f,
            1.0f);
    }

    // Valueノイズを重ねて振幅和で正規化します(x: X座標, y: Y座標, octaves: 合成回数、0～8, lacunarity: 周波数倍率, gain: 振幅倍率)。
    [[nodiscard]] inline float FractalValue2D(
        const float x,
        const float y,
        const int octaves = 5,
        const float lacunarity = 2.0f,
        const float gain = 0.5f) noexcept
    {
        // 振幅を掛けたノイズ値の和
        float total = 0.0f;
        // 現在の合成層の振幅
        float amplitude = 1.0f;
        // 合成した振幅の和
        float normalization = 0.0f;
        // 合成層のXサンプル座標
        float sampleX = x;
        // 合成層のYサンプル座標
        float sampleY = y;
        // 0～8に制限した合成回数
        const int limit = std::clamp(octaves, 0, 8);
        // 現在の合成層の番号
        for (int octave = 0; octave < limit; ++octave)
        {
            total += Value2D(sampleX, sampleY) * amplitude;
            normalization += amplitude;
            sampleX *= lacunarity;
            sampleY *= lacunarity;
            amplitude *= gain;
        }
        return normalization > 0.0f
            ? total / normalization
            : 0.0f;
    }

    // Perlinノイズを重ねて振幅和で正規化します(x: X座標, y: Y座標, octaves: 合成回数、0～8, lacunarity: 周波数倍率, gain: 振幅倍率)。
    [[nodiscard]] inline float FractalPerlin2D(
        const float x,
        const float y,
        const int octaves = 5,
        const float lacunarity = 2.0f,
        const float gain = 0.5f) noexcept
    {
        // 振幅を掛けたノイズ値の和
        float total = 0.0f;
        // 現在の合成層の振幅
        float amplitude = 1.0f;
        // 合成した振幅の和
        float normalization = 0.0f;
        // 合成層のXサンプル座標
        float sampleX = x;
        // 合成層のYサンプル座標
        float sampleY = y;
        // 0～8に制限した合成回数
        const int limit = std::clamp(octaves, 0, 8);
        // 現在の合成層の番号
        for (int octave = 0; octave < limit; ++octave)
        {
            total += Perlin2D(sampleX, sampleY) * amplitude;
            normalization += amplitude;
            sampleX *= lacunarity;
            sampleY *= lacunarity;
            amplitude *= gain;
        }
        return normalization > 0.0f
            ? total / normalization
            : 0.0f;
    }

    // 最近傍の種までの距離を0～1に収めて返します(x: X座標, y: Y座標)。
    [[nodiscard]] inline float Worley2D(
        const float x,
        const float y) noexcept
    {
        // X方向の格子原点
        const float flooredX = std::floor(x);
        // Y方向の格子原点
        const float flooredY = std::floor(y);
        // 格子原点からのX方向の距離
        const float fx = x - flooredX;
        // 格子原点からのY方向の距離
        const float fy = y - flooredY;
        // 種までの最小二乗距離
        float nearest = 1.0e9f;
        // 隣接セルのY方向の差
        for (int offsetY = -1; offsetY <= 1; ++offsetY)
        {
            // 隣接セルのX方向の差
            for (int offsetX = -1; offsetX <= 1; ++offsetX)
            {
                // 隣接セルのX座標
                const auto cellX =
                    static_cast<std::int32_t>(flooredX)
                    + offsetX;
                // 隣接セルのY座標
                const auto cellY =
                    static_cast<std::int32_t>(flooredY)
                    + offsetY;
                // セル内の種のX座標
                const float seedX = Grid2(cellX, cellY);
                // セル内の種のY座標
                const float seedY = Grid2(cellY, cellX);
                // 種までのX方向の距離
                const float deltaX =
                    static_cast<float>(offsetX)
                    + seedX - fx;
                // 種までのY方向の距離
                const float deltaY =
                    static_cast<float>(offsetY)
                    + seedY - fy;
                nearest = std::min(
                    nearest,
                    deltaX * deltaX + deltaY * deltaY);
            }
        }
        return std::clamp(
            std::sqrt(nearest),
            0.0f,
            1.0f);
    }

    // Perlinノイズの勾配を回転した渦の流れを返します(x: X座標, y: Y座標, epsilon: 中心差分の座標間隔、最小0.0001)。
    // 格子軸への偏りを避けるため、差分の基底にはPerlinノイズを使います。
    [[nodiscard]] inline DirectX::XMFLOAT2 Curl2D(
        const float x,
        const float y,
        const float epsilon = 0.01f) noexcept
    {
        // 最小値を保証した差分間隔
        const float e = std::max(epsilon, 0.0001f);
        // 右側のPerlinノイズ値
        const float right = Perlin2D(x + e, y);
        // 左側のPerlinノイズ値
        const float left = Perlin2D(x - e, y);
        // 上側のPerlinノイズ値
        const float up = Perlin2D(x, y + e);
        // 下側のPerlinノイズ値
        const float down = Perlin2D(x, y - e);
        // X方向の中心差分微分
        const float dx = (right - left) / (2.0f * e);
        // Y方向の中心差分微分
        const float dy = (up - down) / (2.0f * e);
        return { dy, -dx };
    }
}
