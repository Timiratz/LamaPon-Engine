// C++と同じ値を返す、シェーダー用ノイズ関数です。
// 実装変更時はsrc/LamaPon/Core/Noise.hも更新し、CPU/GPUの一致を維持します。

#ifndef LAMAPON_NOISE_INCLUDED
#define LAMAPON_NOISE_INCLUDED
// 整数格子のハッシュを求めます(value: 格子点から作った整数値)。
uint LamaPonNoiseHash(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

// ハッシュの上位24ビットを0以上1未満へ変換します(hashed: ハッシュ値)。
float LamaPonNoiseToFloat(uint hashed)
{
    return (float)(hashed >> 8u) / 16777216.0f;
}

// 1次元格子点のノイズ値を返します(x: 整数格子のX座標)。
float LamaPonNoiseGrid1(int x)
{
    return LamaPonNoiseToFloat(
        LamaPonNoiseHash((uint)x * 0x9e3779b9u));
}

// 2次元格子点のノイズ値を返します(x: 整数格子のX座標, y: 整数格子のY座標)。
float LamaPonNoiseGrid2(int x, int y)
{
    // 格子点の整数ハッシュ値
    const uint hashed = LamaPonNoiseHash(
        (uint)x * 0x9e3779b9u
        + (uint)y * 0x85ebca6bu);
    return LamaPonNoiseToFloat(hashed);
}

// 3次元格子点のノイズ値を返します(x: 整数格子のX座標, y: 整数格子のY座標, z: 整数格子のZ座標)。
float LamaPonNoiseGrid3(int x, int y, int z)
{
    // 格子点の整数ハッシュ値
    const uint hashed = LamaPonNoiseHash(
        (uint)x * 0x9e3779b9u
        + (uint)y * 0x85ebca6bu
        + (uint)z * 0xc2b2ae35u);
    return LamaPonNoiseToFloat(hashed);
}

// 5次補間の重みを返します(t: 格子内の位置、0～1)。
float LamaPonNoiseFade(float t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// 1次元Valueノイズを返します(x: サンプル座標)。
float LamaPonValueNoise1D(float x)
{
    // X方向の格子原点
    const float floorX = floor(x);
    // 格子点のX座標
    const int ix = (int)floorX;
    // X方向の補間重み
    const float t = LamaPonNoiseFade(x - floorX);
    return lerp(
        LamaPonNoiseGrid1(ix),
        LamaPonNoiseGrid1(ix + 1),
        t);
}

// 2次元Valueノイズを返します(position: サンプル座標)。
float LamaPonValueNoise2D(float2 position)
{
    // サンプル座標の格子原点
    const float2 floored = floor(position);
    // 格子点のX座標
    const int ix = (int)floored.x;
    // 格子点のY座標
    const int iy = (int)floored.y;
    // 格子原点からの変位
    const float2 f = position - floored;
    // X方向の補間重み
    const float tx = LamaPonNoiseFade(f.x);
    // Y方向の補間重み
    const float ty = LamaPonNoiseFade(f.y);
    // Y方向の下側での補間値
    const float bottom = lerp(
        LamaPonNoiseGrid2(ix, iy),
        LamaPonNoiseGrid2(ix + 1, iy),
        tx);
    // Y方向の上側での補間値
    const float top = lerp(
        LamaPonNoiseGrid2(ix, iy + 1),
        LamaPonNoiseGrid2(ix + 1, iy + 1),
        tx);
    return lerp(bottom, top, ty);
}

// 3次元Valueノイズを返します(position: サンプル座標)。
float LamaPonValueNoise3D(float3 position)
{
    // サンプル座標の格子原点
    const float3 floored = floor(position);
    // 格子点のX座標
    const int ix = (int)floored.x;
    // 格子点のY座標
    const int iy = (int)floored.y;
    // 格子点のZ座標
    const int iz = (int)floored.z;
    // 格子原点からの変位
    const float3 f = position - floored;
    // X方向の補間重み
    const float tx = LamaPonNoiseFade(f.x);
    // Y方向の補間重み
    const float ty = LamaPonNoiseFade(f.y);
    // Z方向の補間重み
    const float tz = LamaPonNoiseFade(f.z);

    // 手前かつ下側の補間値
    const float z0Bottom = lerp(
        LamaPonNoiseGrid3(ix, iy, iz),
        LamaPonNoiseGrid3(ix + 1, iy, iz),
        tx);
    // 手前かつ上側の補間値
    const float z0Top = lerp(
        LamaPonNoiseGrid3(ix, iy + 1, iz),
        LamaPonNoiseGrid3(ix + 1, iy + 1, iz),
        tx);
    // 奥かつ下側の補間値
    const float z1Bottom = lerp(
        LamaPonNoiseGrid3(ix, iy, iz + 1),
        LamaPonNoiseGrid3(ix + 1, iy, iz + 1),
        tx);
    // 奥かつ上側の補間値
    const float z1Top = lerp(
        LamaPonNoiseGrid3(ix, iy + 1, iz + 1),
        LamaPonNoiseGrid3(ix + 1, iy + 1, iz + 1),
        tx);
    return lerp(
        lerp(z0Bottom, z0Top, ty),
        lerp(z1Bottom, z1Top, ty),
        tz);
}

// LamaPonNoiseGradient2(x: 整数格子X, y: 整数格子Y): 格子点用の8方向単位勾配を返します。
float2 LamaPonNoiseGradient2(int x, int y)
{
    // hashed: 格子座標から選んだ勾配index。
    const uint hashed = LamaPonNoiseHash(
        (uint)x * 0x9e3779b9u
        + (uint)y * 0x85ebca6bu) & 7u;
    // diagonal: 正規化した対角勾配の軸成分。
    const float diagonal = 0.70710678f;
    // index 0は右向きです。
    if (hashed == 0u) { return float2(1.0f, 0.0f); }
    // index 1は左向きです。
    if (hashed == 1u) { return float2(-1.0f, 0.0f); }
    // index 2は上向きです。
    if (hashed == 2u) { return float2(0.0f, 1.0f); }
    // index 3は下向きです。
    if (hashed == 3u) { return float2(0.0f, -1.0f); }
    // index 4は右上向きです。
    if (hashed == 4u) { return float2(diagonal, diagonal); }
    // index 5は左上向きです。
    if (hashed == 5u) { return float2(-diagonal, diagonal); }
    // index 6は右下向きです。
    if (hashed == 6u) { return float2(diagonal, -diagonal); }
    // 残るindex 7は左下向きです。
    return float2(-diagonal, -diagonal);
}

// 0～1の2次元Perlinノイズを返します(position: サンプル座標)。
float LamaPonPerlinNoise2D(float2 position)
{
    // サンプル座標の格子原点
    const float2 floored = floor(position);
    // 格子点のX座標
    const int ix = (int)floored.x;
    // 格子点のY座標
    const int iy = (int)floored.y;
    // 格子原点からの変位
    const float2 f = position - floored;

    // 左下の勾配と変位の内積
    const float bottomLeft = dot(
        LamaPonNoiseGradient2(ix, iy),
        f - float2(0.0f, 0.0f));
    // 右下の勾配と変位の内積
    const float bottomRight = dot(
        LamaPonNoiseGradient2(ix + 1, iy),
        f - float2(1.0f, 0.0f));
    // 左上の勾配と変位の内積
    const float topLeft = dot(
        LamaPonNoiseGradient2(ix, iy + 1),
        f - float2(0.0f, 1.0f));
    // 右上の勾配と変位の内積
    const float topRight = dot(
        LamaPonNoiseGradient2(ix + 1, iy + 1),
        f - float2(1.0f, 1.0f));

    // X方向の補間重み
    const float tx = LamaPonNoiseFade(f.x);
    // Y方向の補間重み
    const float ty = LamaPonNoiseFade(f.y);
    // 勾配の内積を補間した値
    const float value = lerp(
        lerp(bottomLeft, bottomRight, tx),
        lerp(topLeft, topRight, tx),
        ty);
    // -0.707〜0.707程度に収まるので、0〜1へ伸ばします。
    return saturate(value * 0.7071f + 0.5f);
}

// Valueノイズを重ねて振幅和で正規化します(position: サンプル座標, octaves: 合成回数、最大8, lacunarity: 周波数倍率, gain: 振幅倍率)。
float LamaPonFractalNoise2D(
    float2 position,
    int octaves,
    float lacunarity,
    float gain)
{
    // 振幅を掛けたノイズ値の和
    float total = 0.0f;
    // 現在の合成層の振幅
    float amplitude = 1.0f;
    // 合成した振幅の和
    float normalization = 0.0f;
    // 合成層のサンプル座標
    float2 sample = position;
    // 現在の合成層の番号
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

// Perlinノイズを重ねて振幅和で正規化します(position: サンプル座標, octaves: 合成回数、最大8, lacunarity: 周波数倍率, gain: 振幅倍率)。
float LamaPonFractalPerlin2D(
    float2 position,
    int octaves,
    float lacunarity,
    float gain)
{
    // 振幅を掛けたノイズ値の和
    float total = 0.0f;
    // 現在の合成層の振幅
    float amplitude = 1.0f;
    // 合成した振幅の和
    float normalization = 0.0f;
    // 合成層のサンプル座標
    float2 sample = position;
    // 現在の合成層の番号
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

// 最近傍の種までの距離を0～1に収めて返します(position: サンプル座標)。
float LamaPonWorleyNoise2D(float2 position)
{
    // サンプル座標の格子原点
    const float2 floored = floor(position);
    // 格子原点からの変位
    const float2 f = position - floored;
    // 種までの最小二乗距離
    float nearest = 1.0e9f;
    // 隣接セルのY方向の差
    [unroll]
    for (int offsetY = -1; offsetY <= 1; ++offsetY)
    {
        // 隣接セルのX方向の差
        [unroll]
        for (int offsetX = -1; offsetX <= 1; ++offsetX)
        {
            // 隣接セルのX座標
            const int cellX = (int)floored.x + offsetX;
            // 隣接セルのY座標
            const int cellY = (int)floored.y + offsetY;
            // セル内の種の座標
            const float2 seed = float2(
                LamaPonNoiseGrid2(cellX, cellY),
                LamaPonNoiseGrid2(cellY, cellX));
            // 種までの変位
            const float2 delta =
                float2(offsetX, offsetY) + seed - f;
            nearest = min(nearest, dot(delta, delta));
        }
    }
    return saturate(sqrt(nearest));
}

// Perlinノイズの勾配を回転した渦の流れを返します(position: サンプル座標, epsilon: 中心差分の座標間隔、最小0.0001)。
float2 LamaPonCurlNoise2D(float2 position, float epsilon)
{
    // 最小値を保証した差分間隔
    const float e = max(epsilon, 0.0001f);
    // 格子軸への偏りを避けるため、差分の基底にはPerlinノイズを使います。
    // 右側のPerlinノイズ値
    const float right =
        LamaPonPerlinNoise2D(position + float2(e, 0.0f));
    // 左側のPerlinノイズ値
    const float left =
        LamaPonPerlinNoise2D(position - float2(e, 0.0f));
    // 上側のPerlinノイズ値
    const float up =
        LamaPonPerlinNoise2D(position + float2(0.0f, e));
    // 下側のPerlinノイズ値
    const float down =
        LamaPonPerlinNoise2D(position - float2(0.0f, e));
    // X方向の中心差分微分
    const float dx = (right - left) / (2.0f * e);
    // Y方向の中心差分微分
    const float dy = (up - down) / (2.0f * e);
    return float2(dy, -dx);
}

#endif
