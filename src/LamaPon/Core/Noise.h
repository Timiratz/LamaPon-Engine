#pragma once

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

// ノイズ関数（C++側）。
//
// 乱数との違い: 乱数は隣の座標でも値が飛びますが、ノイズは
// 「近い座標なら近い値」という連続性があります。地形の高さ、
// オブジェクトの分布、揺らぎなど「自然なムラ」に使います。
//
// この実装は assets/shaders/LamaPonNoise.hlsli（HLSL）と
//   完全に同じ値を返します。C++で地形メッシュを作り、シェーダーで
//   同じノイズを使って色や草を乗せる、という組み合わせが成立します。
//   片方を直したら必ず両方直してください
//   （tests/NoiseTests.cpp が代表点での一致を検査します）。
//
// 一致のために守っていること:
// 1. ハッシュは uint32_t の桁あふれ（ラップ）を前提とし、HLSLのuintと揃えます。
// 2. 0〜1への変換は上位24ビット / 16777216とし、floatの仮数へ収めます。
// 3. 補間には5次のスムーズステップを使い、格子線の発生を抑えます。
// 4. 負の座標でも一致するよう、floorとintキャストの順序を揃えます。
namespace LamaPon::Noise
{
    // 格子点のハッシュ

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

    [[nodiscard]] inline float ToFloat(
        const std::uint32_t hashed) noexcept
    {
        return static_cast<float>(hashed >> 8)
            / 16777216.0f;
    }

    [[nodiscard]] inline float Grid1(
        const std::int32_t x) noexcept
    {
        return ToFloat(
            Hash(static_cast<std::uint32_t>(x)
                * 0x9e3779b9u));
    }

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

    [[nodiscard]] inline float Fade(
        const float t) noexcept
    {
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }

    [[nodiscard]] inline float Lerp(
        const float a,
        const float b,
        const float t) noexcept
    {
        return a + (b - a) * t;
    }

    // Value Noise（格子点の値を補間）

    [[nodiscard]] inline float Value1D(
        const float x) noexcept
    {
        const float floored = std::floor(x);
        const auto ix = static_cast<std::int32_t>(floored);
        const float t = Fade(x - floored);
        return Lerp(Grid1(ix), Grid1(ix + 1), t);
    }

    [[nodiscard]] inline float Value2D(
        const float x,
        const float y) noexcept
    {
        const float flooredX = std::floor(x);
        const float flooredY = std::floor(y);
        const auto ix =
            static_cast<std::int32_t>(flooredX);
        const auto iy =
            static_cast<std::int32_t>(flooredY);
        const float tx = Fade(x - flooredX);
        const float ty = Fade(y - flooredY);
        const float bottom = Lerp(
            Grid2(ix, iy),
            Grid2(ix + 1, iy),
            tx);
        const float top = Lerp(
            Grid2(ix, iy + 1),
            Grid2(ix + 1, iy + 1),
            tx);
        return Lerp(bottom, top, ty);
    }

    [[nodiscard]] inline float Value3D(
        const float x,
        const float y,
        const float z) noexcept
    {
        const float flooredX = std::floor(x);
        const float flooredY = std::floor(y);
        const float flooredZ = std::floor(z);
        const auto ix =
            static_cast<std::int32_t>(flooredX);
        const auto iy =
            static_cast<std::int32_t>(flooredY);
        const auto iz =
            static_cast<std::int32_t>(flooredZ);
        const float tx = Fade(x - flooredX);
        const float ty = Fade(y - flooredY);
        const float tz = Fade(z - flooredZ);

        const float z0Bottom = Lerp(
            Grid3(ix, iy, iz),
            Grid3(ix + 1, iy, iz),
            tx);
        const float z0Top = Lerp(
            Grid3(ix, iy + 1, iz),
            Grid3(ix + 1, iy + 1, iz),
            tx);
        const float z1Bottom = Lerp(
            Grid3(ix, iy, iz + 1),
            Grid3(ix + 1, iy, iz + 1),
            tx);
        const float z1Top = Lerp(
            Grid3(ix, iy + 1, iz + 1),
            Grid3(ix + 1, iy + 1, iz + 1),
            tx);
        return Lerp(
            Lerp(z0Bottom, z0Top, ty),
            Lerp(z1Bottom, z1Top, ty),
            tz);
    }

    // Perlin Noise（格子点の傾きを補間）

    [[nodiscard]] inline DirectX::XMFLOAT2 Gradient2(
        const std::int32_t x,
        const std::int32_t y) noexcept
    {
        const std::uint32_t hashed =
            Hash(static_cast<std::uint32_t>(x)
                    * 0x9e3779b9u
                + static_cast<std::uint32_t>(y)
                    * 0x85ebca6bu)
            & 7u;
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

    [[nodiscard]] inline float Perlin2D(
        const float x,
        const float y) noexcept
    {
        const float flooredX = std::floor(x);
        const float flooredY = std::floor(y);
        const auto ix =
            static_cast<std::int32_t>(flooredX);
        const auto iy =
            static_cast<std::int32_t>(flooredY);
        const float fx = x - flooredX;
        const float fy = y - flooredY;

        const auto dotGradient =
            [](const DirectX::XMFLOAT2& gradient,
                const float dx,
                const float dy) noexcept
            {
                return gradient.x * dx + gradient.y * dy;
            };

        const float bottomLeft = dotGradient(
            Gradient2(ix, iy), fx, fy);
        const float bottomRight = dotGradient(
            Gradient2(ix + 1, iy), fx - 1.0f, fy);
        const float topLeft = dotGradient(
            Gradient2(ix, iy + 1), fx, fy - 1.0f);
        const float topRight = dotGradient(
            Gradient2(ix + 1, iy + 1),
            fx - 1.0f,
            fy - 1.0f);

        const float tx = Fade(fx);
        const float ty = Fade(fy);
        const float value = Lerp(
            Lerp(bottomLeft, bottomRight, tx),
            Lerp(topLeft, topRight, tx),
            ty);
        return std::clamp(
            value * 0.7071f + 0.5f,
            0.0f,
            1.0f);
    }

    // Fractal Noise（fBm。周波数を重ねて細部を作る）
    // octaves: 重ねる回数（多いほど細かい。5前後が定番、上限8）
    // lacunarity: 1回ごとの周波数倍率（2.0が定番）
    // gain: 1回ごとの振幅倍率（0.5が定番）

    [[nodiscard]] inline float FractalValue2D(
        const float x,
        const float y,
        const int octaves = 5,
        const float lacunarity = 2.0f,
        const float gain = 0.5f) noexcept
    {
        float total = 0.0f;
        float amplitude = 1.0f;
        float normalization = 0.0f;
        float sampleX = x;
        float sampleY = y;
        const int limit = std::clamp(octaves, 0, 8);
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

    [[nodiscard]] inline float FractalPerlin2D(
        const float x,
        const float y,
        const int octaves = 5,
        const float lacunarity = 2.0f,
        const float gain = 0.5f) noexcept
    {
        float total = 0.0f;
        float amplitude = 1.0f;
        float normalization = 0.0f;
        float sampleX = x;
        float sampleY = y;
        const int limit = std::clamp(octaves, 0, 8);
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

    // Worley Noise（セル状の模様）
    // 一番近い「種」までの距離（0で種の位置、1で遠い）。

    [[nodiscard]] inline float Worley2D(
        const float x,
        const float y) noexcept
    {
        const float flooredX = std::floor(x);
        const float flooredY = std::floor(y);
        const float fx = x - flooredX;
        const float fy = y - flooredY;
        float nearest = 1.0e9f;
        for (int offsetY = -1; offsetY <= 1; ++offsetY)
        {
            for (int offsetX = -1; offsetX <= 1; ++offsetX)
            {
                const auto cellX =
                    static_cast<std::int32_t>(flooredX)
                    + offsetX;
                const auto cellY =
                    static_cast<std::int32_t>(flooredY)
                    + offsetY;
                const float seedX = Grid2(cellX, cellY);
                const float seedY = Grid2(cellY, cellX);
                const float deltaX =
                    static_cast<float>(offsetX)
                    + seedX - fx;
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

    // Curl Noise（発散のない渦状の流れ）

    // Valueノイズの微分は格子軸へ偏るため、等方的な微分を得られる
    // Perlinノイズを基底に使います。
    [[nodiscard]] inline DirectX::XMFLOAT2 Curl2D(
        const float x,
        const float y,
        const float epsilon = 0.01f) noexcept
    {
        const float e = std::max(epsilon, 0.0001f);
        const float right = Perlin2D(x + e, y);
        const float left = Perlin2D(x - e, y);
        const float up = Perlin2D(x, y + e);
        const float down = Perlin2D(x, y - e);
        const float dx = (right - left) / (2.0f * e);
        const float dy = (up - down) / (2.0f * e);
        return { dy, -dx };
    }
}
