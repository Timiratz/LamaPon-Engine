// ノイズ関数の共通実装（HLSL側）。
//
// 乱数との違い: 乱数は隣の座標でも値が飛びますが、ノイズは
// 「近い座標なら近い値」という連続性があります。地形・雲・炎・
// 水面・草の分布のように「自然なムラ」が欲しいときに使います。
//
// ★このファイルの実装は src/LamaPon/Core/Noise.h（C++）と
//   完全に同じ値を返します。C++で地形メッシュを作り、シェーダーで
//   同じノイズを使って色や草を乗せる、という組み合わせが成立します。
//   片方を直したら必ず両方直してください（テストが一致を検査します）。
//
// 使い方（カスタムShaderの中で）:
//   #include "LamaPonNoise.hlsli"
//   float h = LamaPonFractalNoise2D(uv * 8.0, 5, 2.0, 0.5);

#ifndef LAMAPON_NOISE_INCLUDED
#define LAMAPON_NOISE_INCLUDED

// 整数格子の点から擬似乱数（0〜1）を作ります。sin を使う定番の
// 手法は環境によって精度差が出るため、整数演算のハッシュを使います。
// 【重要】uint演算で桁あふれ（ラップ）を前提にしているので、
// C++側も uint32_t で同じ計算をしています。
uint LamaPonNoiseHash(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

float LamaPonNoiseToFloat(uint hashed)
{
    // 上位24ビットを0〜1へ（float の仮数24ビットに収める）。
    return (float)(hashed >> 8u) / 16777216.0f;
}

float LamaPonNoiseGrid1(int x)
{
    return LamaPonNoiseToFloat(
        LamaPonNoiseHash((uint)x * 0x9e3779b9u));
}

float LamaPonNoiseGrid2(int x, int y)
{
    const uint hashed = LamaPonNoiseHash(
        (uint)x * 0x9e3779b9u
        + (uint)y * 0x85ebca6bu);
    return LamaPonNoiseToFloat(hashed);
}

float LamaPonNoiseGrid3(int x, int y, int z)
{
    const uint hashed = LamaPonNoiseHash(
        (uint)x * 0x9e3779b9u
        + (uint)y * 0x85ebca6bu
        + (uint)z * 0xc2b2ae35u);
    return LamaPonNoiseToFloat(hashed);
}

// 補間の重み。5次のスムーズステップで、境目の傾きの不連続を
// 消します（3次だと格子の線がうっすら見えます）。
float LamaPonNoiseFade(float t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// Value Noise（格子点の値を補間）

float LamaPonValueNoise1D(float x)
{
    const float floorX = floor(x);
    const int ix = (int)floorX;
    const float t = LamaPonNoiseFade(x - floorX);
    return lerp(
        LamaPonNoiseGrid1(ix),
        LamaPonNoiseGrid1(ix + 1),
        t);
}

float LamaPonValueNoise2D(float2 position)
{
    const float2 floored = floor(position);
    const int ix = (int)floored.x;
    const int iy = (int)floored.y;
    const float2 f = position - floored;
    const float tx = LamaPonNoiseFade(f.x);
    const float ty = LamaPonNoiseFade(f.y);
    const float bottom = lerp(
        LamaPonNoiseGrid2(ix, iy),
        LamaPonNoiseGrid2(ix + 1, iy),
        tx);
    const float top = lerp(
        LamaPonNoiseGrid2(ix, iy + 1),
        LamaPonNoiseGrid2(ix + 1, iy + 1),
        tx);
    return lerp(bottom, top, ty);
}

float LamaPonValueNoise3D(float3 position)
{
    const float3 floored = floor(position);
    const int ix = (int)floored.x;
    const int iy = (int)floored.y;
    const int iz = (int)floored.z;
    const float3 f = position - floored;
    const float tx = LamaPonNoiseFade(f.x);
    const float ty = LamaPonNoiseFade(f.y);
    const float tz = LamaPonNoiseFade(f.z);

    const float z0Bottom = lerp(
        LamaPonNoiseGrid3(ix, iy, iz),
        LamaPonNoiseGrid3(ix + 1, iy, iz),
        tx);
    const float z0Top = lerp(
        LamaPonNoiseGrid3(ix, iy + 1, iz),
        LamaPonNoiseGrid3(ix + 1, iy + 1, iz),
        tx);
    const float z1Bottom = lerp(
        LamaPonNoiseGrid3(ix, iy, iz + 1),
        LamaPonNoiseGrid3(ix + 1, iy, iz + 1),
        tx);
    const float z1Top = lerp(
        LamaPonNoiseGrid3(ix, iy + 1, iz + 1),
        LamaPonNoiseGrid3(ix + 1, iy + 1, iz + 1),
        tx);
    return lerp(
        lerp(z0Bottom, z0Top, ty),
        lerp(z1Bottom, z1Top, ty),
        tz);
}

// Perlin Noise（格子点の傾きを補間）
// 戻り値は0〜1へ収めています（元の定義は-1〜1）。

float2 LamaPonNoiseGradient2(int x, int y)
{
    // 8方向から選びます（正規化済みの固定方向。乱数の偏りが
    // 格子の縞として出るのを避けます）。
    const uint hashed = LamaPonNoiseHash(
        (uint)x * 0x9e3779b9u
        + (uint)y * 0x85ebca6bu) & 7u;
    const float diagonal = 0.70710678f;
    if (hashed == 0u) { return float2(1.0f, 0.0f); }
    if (hashed == 1u) { return float2(-1.0f, 0.0f); }
    if (hashed == 2u) { return float2(0.0f, 1.0f); }
    if (hashed == 3u) { return float2(0.0f, -1.0f); }
    if (hashed == 4u) { return float2(diagonal, diagonal); }
    if (hashed == 5u) { return float2(-diagonal, diagonal); }
    if (hashed == 6u) { return float2(diagonal, -diagonal); }
    return float2(-diagonal, -diagonal);
}

float LamaPonPerlinNoise2D(float2 position)
{
    const float2 floored = floor(position);
    const int ix = (int)floored.x;
    const int iy = (int)floored.y;
    const float2 f = position - floored;

    const float bottomLeft = dot(
        LamaPonNoiseGradient2(ix, iy),
        f - float2(0.0f, 0.0f));
    const float bottomRight = dot(
        LamaPonNoiseGradient2(ix + 1, iy),
        f - float2(1.0f, 0.0f));
    const float topLeft = dot(
        LamaPonNoiseGradient2(ix, iy + 1),
        f - float2(0.0f, 1.0f));
    const float topRight = dot(
        LamaPonNoiseGradient2(ix + 1, iy + 1),
        f - float2(1.0f, 1.0f));

    const float tx = LamaPonNoiseFade(f.x);
    const float ty = LamaPonNoiseFade(f.y);
    const float value = lerp(
        lerp(bottomLeft, bottomRight, tx),
        lerp(topLeft, topRight, tx),
        ty);
    // -0.707〜0.707程度に収まるので、0〜1へ伸ばします。
    return saturate(value * 0.7071f + 0.5f);
}

// Fractal Noise（fBm。周波数を重ねて細部を作る）
// octaves: 重ねる回数（多いほど細かい。5前後が定番）
// lacunarity: 1回ごとに周波数を何倍にするか（2.0が定番）
// gain: 1回ごとに振幅を何倍にするか（0.5が定番）

float LamaPonFractalNoise2D(
    float2 position,
    int octaves,
    float lacunarity,
    float gain)
{
    float total = 0.0f;
    float amplitude = 1.0f;
    float normalization = 0.0f;
    float2 sample = position;
    // ループ回数を定数上限で抑えます（シェーダーで展開できるように）。
    [loop]
    for (int octave = 0; octave < 8; ++octave)
    {
        if (octave >= octaves)
        {
            break;
        }
        total += LamaPonValueNoise2D(sample) * amplitude;
        normalization += amplitude;
        sample *= lacunarity;
        amplitude *= gain;
    }
    return normalization > 0.0f
        ? total / normalization
        : 0.0f;
}

float LamaPonFractalPerlin2D(
    float2 position,
    int octaves,
    float lacunarity,
    float gain)
{
    float total = 0.0f;
    float amplitude = 1.0f;
    float normalization = 0.0f;
    float2 sample = position;
    [loop]
    for (int octave = 0; octave < 8; ++octave)
    {
        if (octave >= octaves)
        {
            break;
        }
        total += LamaPonPerlinNoise2D(sample) * amplitude;
        normalization += amplitude;
        sample *= lacunarity;
        amplitude *= gain;
    }
    return normalization > 0.0f
        ? total / normalization
        : 0.0f;
}

// Worley Noise（セル状の模様）
// 一番近い「種」までの距離を返します（0で種の位置、1で遠い）。

float LamaPonWorleyNoise2D(float2 position)
{
    const float2 floored = floor(position);
    const float2 f = position - floored;
    float nearest = 1.0e9f;
    [unroll]
    for (int offsetY = -1; offsetY <= 1; ++offsetY)
    {
        [unroll]
        for (int offsetX = -1; offsetX <= 1; ++offsetX)
        {
            const int cellX = (int)floored.x + offsetX;
            const int cellY = (int)floored.y + offsetY;
            // セルの中の種の位置（x・y別々のハッシュ）。
            const float2 seed = float2(
                LamaPonNoiseGrid2(cellX, cellY),
                LamaPonNoiseGrid2(cellY, cellX));
            const float2 delta =
                float2(offsetX, offsetY) + seed - f;
            nearest = min(nearest, dot(delta, delta));
        }
    }
    return saturate(sqrt(nearest));
}

// Curl Noise（パーティクルに使う渦状の流れ）
// 発散のない（湧き出しも吸い込みもない）流れになるので、
// 煙や水の渦に向いています。

float2 LamaPonCurlNoise2D(float2 position, float epsilon)
{
    const float e = max(epsilon, 0.0001f);
    // ポテンシャル場の勾配を90度回すと、発散が0のベクトル場になります。
    // 元にはPerlinノイズを使います。Valueノイズの微分は縦横方向へ偏り、
    // 格子に沿った縞が現れるためです。Perlinノイズは格子点の勾配を
    // 補間するので、微分が等方的になり自然な渦を作れます。
    const float right =
        LamaPonPerlinNoise2D(position + float2(e, 0.0f));
    const float left =
        LamaPonPerlinNoise2D(position - float2(e, 0.0f));
    const float up =
        LamaPonPerlinNoise2D(position + float2(0.0f, e));
    const float down =
        LamaPonPerlinNoise2D(position - float2(0.0f, e));
    const float dx = (right - left) / (2.0f * e);
    const float dy = (up - down) / (2.0f * e);
    return float2(dy, -dx);
}

#endif
