#include "LamaPonScreenDepth.hlsli"

// 天空色と太陽円盤の定数
cbuffer SkyBuffer : register(b0)
{
    // 天空復元の逆ビュー透視行列
    row_major float4x4 InverseViewProjection;
    // 天空視点のWorld位置
    float4 CameraPosition;
    // 天頂RGB・W天空全体倍率
    float4 TopColor;
    // 地平のRGB
    float4 HorizonColor;
    // 地面側のRGB
    float4 GroundColor;
    // X天空画像有効・YZW予約
    float4 SkyOptions;
    // 太陽への単位方向XYZ・W角半径
    float4 SunDirection;
    // 強度込み太陽RGB・W有効
    float4 SunDiskColor;
};

// BloomとFXAAの画面定数
cbuffer BloomBuffer : register(b1)
{
    // 元画像の逆幅と逆高さ
    float2 TexelSize;
    // 抽出する高輝度の閾値
    float BloomThreshold;
    // Bloomの加算強度
    float BloomIntensity;
    // Bloomの採取間隔倍率
    float BloomRadius;
    // 定数配置用の予約3値
    float3 BloomPadding;
};

// フレアと多段の筋の定数
cbuffer LensFlareBuffer : register(b7)
{
    // 逆画面幅高さ・閾値・全体強度
    float4 LensFlarePrimary;
    // ゴースト間隔・環位置・分散・筋
    float4 LensFlareSecondary;
    // X筋の長さ・YZW予約
    float4 LensFlareTertiary;
    // UV間隔・方向数・初角・初回
    float4 LensFlareStreakPass;
};

// 入口ごとに用途が変わる採取定数
cbuffer PrefilterBuffer : register(b3)
{
    // Cube面・粗さ・解像度など
    float4 PrefilterParameters;
};

// 色調整と露出の定数
cbuffer ColorGradingBuffer : register(b2)
{
    // 露出・対比・彩度・色温度
    float4 ColorGradePrimary;
    // 色合い・減光・有効・自動露出
    float4 ColorGradeSecondary;
};

// 深度復元とAO採取の定数
cbuffer AmbientOcclusionBuffer : register(b4)
{
    // 逆画面幅高さ・半径・AO強度
    float4 AmbientOcclusionParameters;
    // 投影33・43と11・22の逆数
    float4 AmbientOcclusionProjection;
    // X採取数・YZW予約
    float4 AmbientOcclusionQuality;
};

// 平行光の散乱と影の定数
cbuffer VolumetricBuffer : register(b5)
{
    // 散乱光復元の逆ビュー透視
    row_major float4x4 VolumetricInverseViewProjection;
    // World視点XYZ・W最大距離
    float4 VolumetricCameraPosition;
    // 光進行方向XYZ・W採取件数
    float4 VolumetricLightDirection;
    // 強度込み光RGB・W前方散乱
    float4 VolumetricLightColor;
    // 平行光の4Cascade影変換
    row_major float4x4 VolumetricCascades[4];
    // Cascade数・深度補正・画素幅
    float4 VolumetricShadowParameters;
};

// TAA再投影と履歴制限の定数
cbuffer TemporalBuffer : register(b6)
{
    // 現フレームの逆ビュー透視
    row_major float4x4 TemporalInverseViewProjection;
    // 前フレームのビュー透視
    row_major float4x4 TemporalPreviousViewProjection;
    // 履歴比・色範囲倍率・逆幅高さ
    float4 TemporalParameters;
};

// 焦点とぼけ範囲の定数
cbuffer DepthOfFieldBuffer : register(b8)
{
    // 焦点距離・帯幅・強度・最大半径
    float4 DepthOfFieldParameters;
    // 投影33・43・ZW予約
    float4 DepthOfFieldProjection;
    // 逆幅高さ・採取数・W予約
    float4 DepthOfFieldTexel;
};

// カメラ速度のぼかし定数
cbuffer MotionBlurBuffer : register(b9)
{
    // 現在の揺らしなし逆ビュー透視
    row_major float4x4 MotionBlurInverseViewProjection;
    // 以前の揺らしなしビュー透視
    row_major float4x4 MotionBlurPreviousViewProjection;
    // 強度・最大画素長・採取数
    float4 MotionBlurParameters;
    // 逆画面幅高さ・ZW予約
    float4 MotionBlurTexel;
};

// 自動露出の測定画像定数
cbuffer LuminanceBuffer : register(b10)
{
    // 測定画像の逆幅高さ・ZW予約
    float4 LuminanceTexel;
};

// 深度と法線の輪郭判定定数
cbuffer ScreenOutlineBuffer : register(b11)
{
    // 輪郭RGB・W強度
    float4 ScreenOutlineColor;
    // 画素太さ・深度法線閾値
    float4 ScreenOutlineParameters;
    // 投影33・43と11・22の逆数
    float4 ScreenOutlineProjection;
    // 逆画面幅高さ・画面幅高さ
    float4 ScreenOutlineTexel;
};

// 処理対象の色または深度画像
Texture2D SourceTexture : register(t0);
// 天空または畳み込み元のCube
TextureCube SkyCubemap : register(t1);
// 現フレームのデバイス深度
Texture2D DepthTexture : register(t2);
// 平行光のCascade影画像
Texture2DArray VolumetricShadowTexture : register(t3);
// 前フレームのTAA解決済み色
Texture2D TemporalHistoryTexture : register(t4);
// 多段で準備した筋の画像
Texture2D LensFlareStreakTexture : register(t5);
// DoFの色RGB・A符号CoC
Texture2D DepthOfFieldTexture : register(t6);
// 線形補間する画像採取設定
SamplerState LinearSampler : register(s0);
// 影の深度比較採取設定
SamplerComparisonState VolumetricShadowSampler
    : register(s1);

struct ScreenVertex
{
    // 画面三角形の透視位置
    float4 position : SV_Position;
    // 画像採取UV
    float2 uv : TEXCOORD0;
};

// 頂点IDだけで画面を覆う三角形とUVを作る(vertexId: 0～2の頂点番号)。
ScreenVertex VSMain(uint vertexId : SV_VertexID)
{
    // 画面全体の三角形頂点
    ScreenVertex output;
    // 画面全体を覆うNDC位置
    const float2 position = vertexId == 0u
        ? float2(-1.0f, -1.0f)
        : (vertexId == 1u
            ? float2(-1.0f, 3.0f)
            : float2(3.0f, -1.0f));
    output.position = float4(position, 0.0f, 1.0f);
    output.uv = float2(
        position.x * 0.5f + 0.5f,
        0.5f - position.y * 0.5f);
    return output;
}

// 中心を囲む8方向の画素差分
static const int2 ScreenOutlineDirections[8] = {
    int2(-1, -1),
    int2( 0, -1),
    int2( 1, -1),
    int2(-1,  0),
    int2( 1,  0),
    int2(-1,  1),
    int2( 0,  1),
    int2( 1,  1)
};

// 輪郭の採取画素を画面内へ制限する(pixel: 採取画素XY)。
int2 ScreenOutlineClampPixel(int2 pixel)
{
    // 輪郭判定画像の幅と高さ
    const int2 size = max(
        int2(ScreenOutlineTexel.zw),
        int2(1, 1));
    return clamp(pixel, int2(0, 0), size - 1);
}

// 指定画素の深度を正の視点距離へ戻す(pixel: 採取画素XY)。
float ScreenOutlineSceneDistance(int2 pixel)
{
    // 0～1のデバイス深度
    const float deviceDepth = DepthTexture.Load(int3(
        ScreenOutlineClampPixel(pixel),
        0)).r;
    return LamaPonSceneDistance(
        deviceDepth,
        ScreenOutlineProjection);
}

// 端を1画素内へ寄せて4近傍から法線を復元する(pixel: 対象画素XY)。
float3 ScreenOutlineNormal(int2 pixel)
{
    // 輪郭判定画像の幅と高さ
    const int2 size = max(
        int2(ScreenOutlineTexel.zw),
        int2(3, 3));
    // 4近傍を読む内側の最終画素
    const int2 interiorMaximum = max(
        size - 2,
        int2(1, 1));
    // 画面端を避けた採取画素XY
    // 法線復元は4近傍を読むため、中心を画面端から1画素内側へ制限する。
    const int2 safePixel = clamp(
        pixel,
        int2(1, 1),
        interiorMaximum);
    return LamaPonReconstructViewNormal(
        DepthTexture,
        safePixel,
        ScreenOutlineTexel.xy,
        ScreenOutlineProjection,
        float4(
            ScreenOutlineProjection.z,
            ScreenOutlineProjection.w,
            0.0f,
            0.0f));
}

// 8近傍の深度・法線差で輪郭色を元の画像へ混ぜる(input: 画面位置とUV)。
float4 PSScreenOutline(ScreenVertex input) : SV_Target
{
    // 処理前の採取RGBA
    const float4 source = SourceTexture.Sample(
        LinearSampler,
        input.uv);
    // 輪郭判定画面の幅と高さ
    const int2 screenSize = int2(ScreenOutlineTexel.zw);
    if (screenSize.x < 3 || screenSize.y < 3)
    {
        return source;
    }

    // 対象の画素XY
    const int2 pixel = int2(input.position.xy);
    // 中心画素の正の視点距離
    const float centerDistance =
        ScreenOutlineSceneDistance(pixel);
    // 中心の深度復元法線
    const float3 centerNormal =
        ScreenOutlineNormal(pixel);
    // 1～4の輪郭採取画素半径
    const int radius = clamp(
        (int)ScreenOutlineParameters.x,
        1,
        4);
    // 相対深度差の輪郭閾値
    const float depthThreshold = max(
        ScreenOutlineParameters.y,
        0.0001f);
    // 法線差の輪郭閾値
    const float normalThreshold = max(
        ScreenOutlineParameters.z,
        0.0001f);
    // 深度差による輪郭の強度
    float depthEdge = 0.0f;
    // 法線差による輪郭の強度
    float normalEdge = 0.0f;

    // 採取方向または採取点の番号
    [unroll]
    for (int index = 0; index < 8; ++index)
    {
        // 輪郭を調べる近傍画素XY
        const int2 samplePixel = pixel
            + ScreenOutlineDirections[index] * radius;
        // 近傍の正の視点距離
        const float sampleDistance =
            ScreenOutlineSceneDistance(samplePixel);
        // 中心が未描画深度か
        const bool centerIsSky = centerDistance >= 999999.0f;
        // 採取点が未描画深度か
        const bool sampleIsSky = sampleDistance >= 999999.0f;
        if (centerIsSky != sampleIsSky)
        {
            depthEdge = 1.0f;
        }
        else if (!centerIsSky)
        {
            // 中心距離に対する深度差
            const float relativeDifference = abs(
                sampleDistance - centerDistance)
                / max(centerDistance, 0.001f);
            depthEdge = max(
                depthEdge,
                smoothstep(
                    0.35f,
                    1.0f,
                    relativeDifference / depthThreshold));

            // 中心と近傍の法線内積差
            const float normalDifference = 1.0f - saturate(dot(
                centerNormal,
                ScreenOutlineNormal(samplePixel)));
            normalEdge = max(
                normalEdge,
                smoothstep(
                    0.35f,
                    1.0f,
                    normalDifference / normalThreshold));
        }
    }

    // 色混合する輪郭の強度
    const float edge = saturate(
        max(depthEdge, normalEdge)
        * saturate(ScreenOutlineColor.a));
    return float4(
        lerp(source.rgb, ScreenOutlineColor.rgb, edge),
        source.a);
}

// 視線方向から天空画像または勾配色と太陽円盤を描く(input: 画面位置とUV)。
float4 PSSky(ScreenVertex input) : SV_Target
{
    // 画面UVから作るNDC位置
    const float2 clip = float2(
        input.uv.x * 2.0f - 1.0f,
        1.0f - input.uv.y * 2.0f);
    // 遠平面のWorld同次位置
    const float4 farPosition = mul(
        float4(clip, 1.0f, 1.0f),
        InverseViewProjection);
    // 遠平面のWorld位置
    const float3 worldPosition =
        farPosition.xyz / max(abs(farPosition.w), 0.00001f);
    // 視点から天空への単位方向
    const float3 direction = normalize(
        worldPosition - CameraPosition.xyz);
    if (SkyOptions.x > 0.5f)
    {
        // 天空Cubeの採取RGB
        const float3 cubeColor =
            SkyCubemap.SampleLevel(
                LinearSampler,
                direction,
                0.0f).rgb;
        return float4(
            cubeColor * max(TopColor.a, 0.0f),
            1.0f);
    }
    // 地平から天頂への混合比
    const float above = smoothstep(
        -0.03f, 0.85f, direction.y);
    // 地平から地面への混合比
    const float below = smoothstep(
        0.0f, 0.65f, -direction.y);
    // 処理前後のRGB
    float3 color = lerp(
        HorizonColor.rgb,
        TopColor.rgb,
        above);
    color = lerp(color, GroundColor.rgb, below);

    if (SunDiskColor.a > 0.0f)
    {
        // 視線と太陽方向の内積
        const float cosine = dot(direction, SunDirection.xyz);
        // 太陽の角半径rad
        const float radius = max(SunDirection.w, 0.0001f);
        // 太陽円盤の輪郭混合比
        const float disk = smoothstep(
            cos(radius * 1.05f),
            cos(radius * 0.95f),
            cosine);
        // 太陽周辺の光の強度
        const float glow = pow(
            saturate(
                (cosine - cos(radius * 30.0f))
                / max(1.0f - cos(radius * 30.0f), 0.0001f)),
            4.0f);
        // 地平付近の太陽の減衰比
        const float horizonFade = smoothstep(
            -0.12f, 0.02f, SunDirection.y);
        color += SunDiskColor.rgb * glow * 0.35f * horizonFade;
        color = lerp(
            color,
            SunDiskColor.rgb,
            disk * horizonFade);
    }
    return float4(color * max(TopColor.a, 0.0f), 1.0f);
}

// 最大RGBがBloom閾値を超えた分を抽出する(uv: 画像の採取UV)。
float3 BrightColor(float2 uv)
{
    // 処理前後のRGB
    const float3 color =
        SourceTexture.Sample(LinearSampler, uv).rgb;
    // 最大RGB成分の輝度
    const float brightness = max(
        color.r,
        max(color.g, color.b));
    return color * saturate(
        (brightness - BloomThreshold)
        / max(brightness, 0.0001f));
}

// 周囲9点の高輝度を元の画像へ加える(input: 画面位置とUV)。
float4 PSBloom(ScreenVertex input) : SV_Target
{
    // 処理前の採取RGBA
    const float4 source =
        SourceTexture.Sample(LinearSampler, input.uv);
    // BloomのUV採取間隔
    const float2 step = TexelSize * BloomRadius;
    // 9点の高輝度加算RGB
    float3 bloom = BrightColor(input.uv) * 0.2f;
    bloom += BrightColor(input.uv + float2(step.x, 0.0f)) * 0.12f;
    bloom += BrightColor(input.uv - float2(step.x, 0.0f)) * 0.12f;
    bloom += BrightColor(input.uv + float2(0.0f, step.y)) * 0.12f;
    bloom += BrightColor(input.uv - float2(0.0f, step.y)) * 0.12f;
    bloom += BrightColor(input.uv + step) * 0.08f;
    bloom += BrightColor(input.uv - step) * 0.08f;
    bloom += BrightColor(input.uv + float2(step.x, -step.y)) * 0.08f;
    bloom += BrightColor(input.uv + float2(-step.x, step.y)) * 0.08f;
    return float4(
        source.rgb + bloom * BloomIntensity,
        source.a);
}

// 画面内で高輝度を滑らかに抽出する(uv: 画像の採取UV)。
float3 LensFlareBright(float2 uv)
{
    if (any(uv < 0.0f) || any(uv > 1.0f))
    {
        return 0.0f;
    }
    // 処理前後のRGB
    const float3 color =
        SourceTexture.Sample(LinearSampler, uv).rgb;
    // 最大RGB成分の輝度
    const float brightness = max(
        color.r,
        max(color.g, color.b));
    // フレア高輝度抽出の閾値
    const float threshold = max(LensFlarePrimary.z, 0.0f);
    // 閾値付近の高輝度混合比
    const float gate = smoothstep(
        threshold,
        threshold + max(threshold * 0.35f, 0.25f),
        brightness);
    return color * gate;
}

// 方向に沿ってRGBの採取位置をずらす(uv: 採取中心UV, direction: 分散する単位方向)。
float3 LensFlareChromaticSample(float2 uv, float2 direction)
{
    // RGB採取位置のUV分散幅
    const float chromatic =
        saturate(LensFlareSecondary.z) * 0.015f;
    // RGB分散方向のUV差分
    const float2 offset = direction * chromatic;
    // 赤側ずらしの採取RGB
    const float3 red = LensFlareBright(uv + offset);
    // 中心ずらしなしの採取RGB
    const float3 green = LensFlareBright(uv);
    // 青側ずらしの採取RGB
    const float3 blue = LensFlareBright(uv - offset);
    return float3(red.r, green.g, blue.b);
}


// 1～4方向の5点採取で筋を広げて重み合計で正規化する(input: 画面位置とUV)。
float4 PSLensFlareStreak(ScreenVertex input) : SV_Target
{
    // 筋の採取間隔UV
    const float stride = LensFlareStreakPass.x;
    // 1～4の筋方向件数
    const int directionCount = clamp(
        (int)LensFlareStreakPass.y,
        1,
        4);
    // 最初の筋方向の角度rad
    const float baseAngle = LensFlareStreakPass.z;
    // 高輝度を抽出する初回か
    const bool firstPass = LensFlareStreakPass.w > 0.5f;

    // 採取値の重み付き合計
    float3 total = 0.0f;
    // 筋の採取重みの合計
    float weightTotal = 0.0f;
    // 採取方向または採取点の番号
    [loop]
    for (int index = 0; index < directionCount; ++index)
    {
        // 採取方向の角度rad
        const float angle = baseAngle
            + 3.14159265f * (float)index
                / (float)directionCount;
        // 筋を延ばす2次元単位方向
        const float2 axis = float2(cos(angle), sin(angle));
        // 筋方向の-2～2の採取番号
        [unroll]
        for (int tap = -2; tap <= 2; ++tap)
        {
            // 採取位置のずらし量
            const float2 offset =
                axis * ((float)tap * stride);
            // 処理画像の採取UV
            // 画面端でも採取数と重みを保つため、採取UVを端へ制限する。
    const float2 uv = clamp(
                input.uv + offset,
                0.0f,
                1.0f);
            // 採取または履歴の混合重み
            const float weight =
                1.0f - abs((float)tap) * 0.22f;
            // 筋の色分散の強度
            const float dispersion =
                saturate(LensFlareSecondary.z);
            // 採取方向による色分散比
            const float shift =
                (float)tap / 2.0f * dispersion;
            // 重みを掛ける筋の採取RGB
            float3 sample = firstPass
                ? LensFlareBright(uv)
                : LensFlareStreakTexture.SampleLevel(
                    LinearSampler,
                    uv,
                    0.0f).rgb;
            if (dispersion > 0.0f)
            {
                sample *= float3(
                    1.0f + shift,
                    1.0f,
                    1.0f - shift);
            }
            total += sample * weight;
            weightTotal += weight;
        }
    }
    // 多段パスで輝度が減らないよう、採取数でなく実際の重み合計で割る。
    total /= max(weightTotal, 0.0001f);
    return float4(total, 1.0f);
}

// 高輝度のゴースト・ハロー・準備済みの筋を合成する(input: 画面位置とUV)。
float4 PSScreenSpaceLensFlare(ScreenVertex input) : SV_Target
{
    // 処理前の採取RGBA
    const float4 source =
        SourceTexture.Sample(LinearSampler, input.uv);
    // 画面中心のUV
    const float2 center = float2(0.5f, 0.5f);
    // 画面中心からのUV差分
    const float2 fromCenter = input.uv - center;
    // 画面中心からのUV距離
    const float radius = length(fromCenter);
    // 画面中心からの単位方向
    const float2 direction = radius > 0.0001f
        ? fromCenter / radius
        : float2(1.0f, 0.0f);

    // ゴースト・ハロー・筋のRGB
    float3 flare = LensFlareBright(input.uv) * 0.22f;

    // ゴーストの間隔倍率
    const float dispersal = max(
        LensFlareSecondary.x,
        0.01f);
    // 採取方向または採取点の番号
    [unroll]
    for (int index = 1; index <= 4; ++index)
    {
        // ゴースト番号込みの間隔倍率
        const float scale = dispersal * (float)index;
        // 画面中心の反対側の採取UV
        const float2 ghostUv = center - fromCenter * scale;
        // 現在ゴーストの加算倍率
        const float ghostWeight = 0.23f - (float)index * 0.025f;
        flare += LensFlareChromaticSample(
            ghostUv,
            direction) * max(ghostWeight, 0.05f);
    }

    // ハローのUV半径
    const float haloRadius = clamp(
        LensFlareSecondary.y,
        0.05f,
        1.5f);
    // ハロー中心円からの距離
    const float haloDistance = abs(radius - haloRadius);
    // ハローの輪郭混合比
    const float halo = 1.0f - smoothstep(
        0.015f,
        0.10f + haloRadius * 0.18f,
        haloDistance);
    // ハロー光源の反対側UV
    const float2 haloUv = center - direction * haloRadius;
    flare += LensFlareChromaticSample(
        haloUv,
        direction) * halo * 0.32f;

    // 多段で準備済みの筋RGB
    const float3 streak =
        LensFlareStreakTexture.SampleLevel(
            LinearSampler,
            input.uv,
            0.0f).rgb;
    flare += streak * LensFlareSecondary.w;

    return float4(
        source.rgb
            + flare * max(LensFlarePrimary.w, 0.0f),
        source.a);
}

// RGBから固定係数の輝度を求める(color: 評価するRGB)。
float Luminance(float3 color)
{
    return dot(color, float3(0.299f, 0.587f, 0.114f));
}

// 近傍の輝度差からエッジ方向の色を補間する(input: 画面位置とUV)。
float4 PSFXAA(ScreenVertex input) : SV_Target
{
    // 処理画像の逆幅と逆高さ
    const float2 texel = TexelSize;
    // 中心画素のRGB
    const float3 center =
        SourceTexture.Sample(LinearSampler, input.uv).rgb;
    // 中心画素の輝度
    const float lumaCenter = Luminance(center);
    // 上の画素の輝度
    const float lumaNorth = Luminance(
        SourceTexture.Sample(
            LinearSampler,
            input.uv + float2(0.0f, -texel.y)).rgb);
    // 下の画素の輝度
    const float lumaSouth = Luminance(
        SourceTexture.Sample(
            LinearSampler,
            input.uv + float2(0.0f, texel.y)).rgb);
    // 左の画素の輝度
    const float lumaWest = Luminance(
        SourceTexture.Sample(
            LinearSampler,
            input.uv + float2(-texel.x, 0.0f)).rgb);
    // 右の画素の輝度
    const float lumaEast = Luminance(
        SourceTexture.Sample(
            LinearSampler,
            input.uv + float2(texel.x, 0.0f)).rgb);
    // 中心と4近傍の最低輝度
    const float lumaMinimum = min(
        lumaCenter,
        min(min(lumaNorth, lumaSouth), min(lumaWest, lumaEast)));
    // 中心と4近傍の最高輝度
    const float lumaMaximum = max(
        lumaCenter,
        max(max(lumaNorth, lumaSouth), max(lumaWest, lumaEast)));
    if (lumaMaximum - lumaMinimum < 0.0312f)
    {
        return float4(center, 1.0f);
    }

    // 近傍輝度から作るUV採取方向
    float2 direction = float2(
        -(lumaNorth - lumaSouth),
        lumaWest - lumaEast);
    // エッジ方向の正規化補正
    const float reduction = max(
        (lumaNorth + lumaSouth + lumaWest + lumaEast)
            * 0.03125f,
        0.0078125f);
    // 補正後の最小成分の逆数
    const float inverseMinimum =
        1.0f / (min(abs(direction.x), abs(direction.y)) + reduction);
    direction = clamp(
        direction * inverseMinimum,
        -8.0f,
        8.0f) * texel;

    // 内側2点の平均RGB
    const float3 first =
        0.5f * (
            SourceTexture.Sample(
                LinearSampler,
                input.uv + direction * (1.0f / 3.0f - 0.5f)).rgb
            + SourceTexture.Sample(
                LinearSampler,
                input.uv + direction * (2.0f / 3.0f - 0.5f)).rgb);
    // 内外4点を混ぜたRGB
    const float3 second =
        first * 0.5f
        + 0.25f * (
            SourceTexture.Sample(
                LinearSampler,
                input.uv + direction * -0.5f).rgb
            + SourceTexture.Sample(
                LinearSampler,
                input.uv + direction * 0.5f).rgb);
    // 内外4点を混ぜた輝度
    const float secondLuma = Luminance(second);
    return float4(
        secondLuma < lumaMinimum || secondLuma > lumaMaximum
            ? first
            : second,
        1.0f);
}

// HDRのRGBをACES近似で0～1へ圧縮する(color: HDRのRGB)。
float3 ACESFilm(float3 color)
{
    // ACES分子の二次係数
    const float a = 2.51f;
    // ACES分子の一次係数
    const float b = 0.03f;
    // ACES分母の二次係数
    const float c = 2.43f;
    // ACES分母の一次係数
    const float d = 0.59f;
    // ACES分母の定数項
    const float e = 0.14f;
    return saturate(
        (color * (a * color + b))
        / (color * (c * color + d) + e));
}

// 露出と白色補正・ACES・色調整・周辺減光を適用する(input: 画面位置とUV)。
float4 PSToneMap(ScreenVertex input) : SV_Target
{
    // 処理前後のRGB
    float3 color = max(
        SourceTexture.Sample(LinearSampler, input.uv).rgb,
        0.0f);
    // 色調整の適用比
    const float gradingEnabled = saturate(ColorGradeSecondary.z);
    // 色調整による露出段数
    const float exposure = lerp(
        0.0f,
        ColorGradePrimary.x,
        gradingEnabled);
    // 白色補正の色温度
    const float temperature = lerp(
        0.0f,
        ColorGradePrimary.w,
        gradingEnabled);
    // 白色補正の緑紫方向
    const float tint = lerp(
        0.0f,
        ColorGradeSecondary.x,
        gradingEnabled);
    // 正の下限付きの白色補正RGB
    const float3 whiteBalance = max(float3(
        1.0f + temperature * 0.16f - tint * 0.05f,
        1.0f + tint * 0.10f,
        1.0f - temperature * 0.16f - tint * 0.05f),
        0.05f);
    // CPU測定の自動露出段数
    // 自動露出は色調整の有効状態と独立に適用する。
    const float autoExposure = ColorGradeSecondary.w;
    color *= exp2(exposure + autoExposure) * whiteBalance;
    color = ACESFilm(color);

    // ACES後のRGB輝度
    const float luminance = Luminance(color);
    // 色調整による彩度倍率
    const float saturation = lerp(
        1.0f,
        max(ColorGradePrimary.z, 0.0f),
        gradingEnabled);
    color = lerp(luminance.xxx, color, saturation);
    // 色調整による対比倍率
    const float contrast = lerp(
        1.0f,
        max(ColorGradePrimary.y, 0.0f),
        gradingEnabled);
    color = (color - 0.5f) * contrast + 0.5f;

    // 画面中心を0とする座標
    const float2 centered = input.uv * 2.0f - 1.0f;
    // 中心距離による明るさ倍率
    const float vignetteShape = saturate(
        1.0f - dot(centered, centered) * 0.42f);
    // 周辺減光の混合比
    const float vignette = lerp(
        0.0f,
        saturate(ColorGradeSecondary.y),
        gradingEnabled);
    color *= lerp(1.0f, vignetteShape, vignette);
    return float4(saturate(color), 1.0f);
}

// 元の画像を同じUVで採取する(input: 画面位置とUV)。
float4 PSCopy(ScreenVertex input) : SV_Target
{
    return SourceTexture.Sample(
        LinearSampler,
        input.uv);
}


// RH深度をHi-Z用の正の視点距離へ変換する(input: 画面位置とUV)。
float4 PSReflectionDepthLinearize(
    ScreenVertex input) : SV_Target
{
    // 対象の画素XY
    const int2 pixel = int2(input.position.xy);
    // 0～1のデバイス深度
    // この入口のPrefilterParameters.xyはRH投影33・43で、未描画深度は距離1e6へ戻す。
    const float deviceDepth =
        SourceTexture.Load(int3(pixel, 0)).r;
    // 距離復元または分布の分母
    const float denominator =
        deviceDepth + PrefilterParameters.x;
    if (denominator > -1e-6f)
    {
        return 1e6f;
    }
    return PrefilterParameters.y / denominator;
}

// 親Mipの最短距離を集約し奇数辺の余りも取り込む(input: 子Mipの画面位置とUV)。
float4 PSReflectionDepthDownsample(
    ScreenVertex input) : SV_Target
{
    // 深度親Mipの幅と高さ
    // この入口のPrefilterParameters.xyは親Mipの幅高さで、奇数辺は追加採取して拾い漏れを防ぐ。
    const int2 parentSize = int2(PrefilterParameters.xy);
    // 親Mipの2×2基点XY
    const int2 parent = int2(input.position.xy) * 2;
    // 親Mipの最終画素XY
    const int2 last = parentSize - 1;
    // 親の左上の視点距離
    const float a = SourceTexture.Load(
        int3(min(parent, last), 0)).r;
    // 親の右上の視点距離
    const float b = SourceTexture.Load(
        int3(min(parent + int2(1, 0), last), 0)).r;
    // 親の左下の視点距離
    const float c = SourceTexture.Load(
        int3(min(parent + int2(0, 1), last), 0)).r;
    // 親の右下の視点距離
    const float d = SourceTexture.Load(
        int3(min(parent + int2(1, 1), last), 0)).r;
    // 集約した最短視点距離
    float nearest = min(min(a, b), min(c, d));
    // 親Mipの幅が奇数か
    const bool oddWidth = (parentSize.x & 1) != 0;
    // 親Mipの高さが奇数か
    const bool oddHeight = (parentSize.y & 1) != 0;
    if (oddWidth)
    {
        nearest = min(nearest, SourceTexture.Load(
            int3(min(parent + int2(2, 0), last), 0)).r);
        nearest = min(nearest, SourceTexture.Load(
            int3(min(parent + int2(2, 1), last), 0)).r);
    }
    if (oddHeight)
    {
        nearest = min(nearest, SourceTexture.Load(
            int3(min(parent + int2(0, 2), last), 0)).r);
        nearest = min(nearest, SourceTexture.Load(
            int3(min(parent + int2(1, 2), last), 0)).r);
    }
    if (oddWidth && oddHeight)
    {
        nearest = min(nearest, SourceTexture.Load(
            int3(min(parent + int2(2, 2), last), 0)).r);
    }
    return nearest;
}


// AO用のRH深度を正の視点距離へ戻す(depth: 0～1のデバイス深度)。
float AmbientOcclusionViewDepth(float depth)
{
    return LamaPonSceneDistance(
        depth,
        AmbientOcclusionProjection);
}

// AO用の右X・上Y・奥Zの位置を復元する(uv: 画面UV, depth: 0～1のデバイス深度)。
float3 AmbientOcclusionViewPosition(float2 uv, float depth)
{
    return LamaPonViewPositionFromDepth(
        uv,
        depth,
        AmbientOcclusionProjection,
        float4(
            AmbientOcclusionProjection.z,
            AmbientOcclusionProjection.w,
            0.0f,
            0.0f));
}

// 指定UVの深度からAO用の位置を復元する(uv: 深度採取UV)。
float3 AmbientOcclusionViewPositionAt(float2 uv)
{
    // 採取したデバイス深度
    const float depth = DepthTexture.Sample(
        LinearSampler,
        uv).r;
    return AmbientOcclusionViewPosition(uv, depth);
}

// 各軸で段差の小さい近傍を選びAO用法線を作る(uv: 中心画面UV, origin: 中心のAO用位置, texelSize: 元深度画像の逆幅高さ)。
float3 AmbientOcclusionReconstructNormal(
    float2 uv,
    float3 origin,
    float2 texelSize)
{
    // 画面横方向の1画素UV
    // UVのYは下向きのため、上近傍はUVから縦幅を引いて復元する。
    const float2 offsetX = float2(texelSize.x, 0.0f);
    // 画面縦方向の1画素UV
    const float2 offsetY = float2(0.0f, texelSize.y);

    // 左近傍のAO用位置
    const float3 left = AmbientOcclusionViewPositionAt(uv - offsetX);
    // 右近傍のAO用位置
    const float3 right = AmbientOcclusionViewPositionAt(uv + offsetX);
    // 上近傍のAO用位置
    const float3 up = AmbientOcclusionViewPositionAt(uv - offsetY);
    // 下近傍のAO用位置
    const float3 down = AmbientOcclusionViewPositionAt(uv + offsetY);

    return LamaPonNormalFromNeighbours(
        origin, left, right, up, down);
}

// 画素位置から採取方向を回す擬似乱数を作る(pixel: 画素XY)。
float AmbientOcclusionNoise(float2 pixel)
{
    return frac(
        52.9829189f
        * frac(dot(pixel, float2(0.06711056f, 0.00583715f))));
}

// 深度の近傍から残る間接光の倍率1～0を求める(input: 画面位置とUV)。
float4 PSAmbientOcclusion(ScreenVertex input) : SV_Target
{
    // 採取したデバイス深度
    const float depth = DepthTexture.Sample(
        LinearSampler,
        input.uv).r;
    if (depth >= 0.999999f)
    {
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    // 中心画素のAO用位置
    const float3 origin = AmbientOcclusionViewPosition(
        input.uv,
        depth);

    // 深度採取の逆幅と逆高さ
    const float2 texelSize = AmbientOcclusionParameters.xy;
    // 遮蔽を調べるWorld半径
    const float radius = AmbientOcclusionParameters.z;
    // AO暗さの強度倍率
    const float strength = AmbientOcclusionParameters.w;

    // 近傍深度から復元した法線
    const float3 normal = AmbientOcclusionReconstructNormal(
        input.uv,
        origin,
        texelSize);

    // 距離補正した画面採取半径
    const float projectedRadius =
        radius / max(origin.z, 0.001f);
    // 画素ごとの採取回転角rad
    const float rotation = AmbientOcclusionNoise(
        input.uv / max(texelSize.x, 1e-6f)
            * float2(1.0f, texelSize.x / max(texelSize.y, 1e-6f)))
        * 6.2831853f;

    // 4～32のAO採取件数
    const int SampleCount = clamp(
        int(AmbientOcclusionQuality.x),
        4,
        32);
    // 法線側の遮蔽の累積比
    float occlusion = 0.0f;
    // 採取方向または採取点の番号
    for (int index = 0; index < SampleCount; ++index)
    {
        // 採取件数に対する番号比
        const float fraction =
            (float(index) + 0.5f) / float(SampleCount);
        // 採取方向の角度rad
        const float angle = rotation + fraction * 18.849556f;
        // 採取円内の正規化半径
        const float distance = sqrt(fraction);
        // 採取位置のずらし量
        const float2 offset = float2(
            cos(angle),
            sin(angle)) * distance * projectedRadius * 0.5f;
        // 近傍の深度採取UV
        const float2 sampleUv = input.uv + offset;
        if (any(sampleUv < 0.0f) || any(sampleUv > 1.0f))
        {
            continue;
        }

        // 採取深度または視点距離
        const float sampleDepth = DepthTexture.Sample(
            LinearSampler,
            sampleUv).r;
        if (sampleDepth >= 0.999999f)
        {
            continue;
        }
        // 近傍深度のAO用位置
        const float3 samplePosition =
            AmbientOcclusionViewPosition(
                sampleUv,
                sampleDepth);
        // 中心から近傍への単位方向
        float3 difference = samplePosition - origin;
        // 中心から近傍への二乗距離
        const float length2 = dot(difference, difference);
        if (length2 < 1e-8f)
        {
            continue;
        }
        difference *= rsqrt(length2);
        // 中心と近傍のWorld距離
        const float sampleDistance = sqrt(length2);
        // 自己遮蔽補正後の法線内積
        const float facing = saturate(
            dot(normal, difference) - 0.06f);
        // World半径による遮蔽減衰
        const float falloff = saturate(
            1.0f - sampleDistance / max(radius, 0.001f));
        occlusion += facing * falloff;
    }

    occlusion = saturate(
        occlusion / float(SampleCount) * 2.4f * strength);
    // 遮蔽後に残る光の倍率
    const float visibility = 1.0f - occlusion;
    return float4(visibility, visibility, visibility, 1.0f);
}

// 4×4の深度差重みでAO倍率を平滑化する(input: 画面位置とUV)。
float4 PSAmbientOcclusionBlur(ScreenVertex input) : SV_Target
{
    // 深度採取の逆幅と逆高さ
    const float2 texelSize = AmbientOcclusionParameters.xy;
    // 中心画素の正の視点距離
    const float centerDepth = AmbientOcclusionViewDepth(
        DepthTexture.Sample(LinearSampler, input.uv).r);

    // 中心距離に比例する許容差
    const float depthScale =
        max(centerDepth * 0.08f, 0.05f);

    // 採取値の重み付き合計
    float total = 0.0f;
    // AO平滑化の重み合計
    float weightSum = 0.0f;
    // 採取する縦画素オフセット
    [unroll]
    for (int y = -2; y <= 1; ++y)
    {
        // 採取する横画素オフセット
        [unroll]
        for (int x = -2; x <= 1; ++x)
        {
            // 採取位置のずらし量
            const float2 offset = float2(
                (float(x) + 0.5f) * texelSize.x,
                (float(y) + 0.5f) * texelSize.y);
            // 近傍の深度採取UV
            const float2 sampleUv = input.uv + offset;
            // 採取画素の正の視点距離
            const float sampleDepth = AmbientOcclusionViewDepth(
                DepthTexture.Sample(
                    LinearSampler,
                    sampleUv).r);
            // 許容差に対する深度差
            const float depthRatio =
                (sampleDepth - centerDepth) / depthScale;
            // 採取または履歴の混合重み
            const float weight =
                1.0f / (1.0f + depthRatio * depthRatio);
            total += SourceTexture.Sample(
                LinearSampler,
                sampleUv).r * weight;
            weightSum += weight;
        }
    }

    // 遮蔽後に残る光の倍率
    const float visibility = weightSum > 0.0f
        ? total / weightSum
        : SourceTexture.Sample(LinearSampler, input.uv).r;
    return float4(visibility, visibility, visibility, 1.0f);
}


// DoF専用のRH投影係数で正の視点距離を戻す(depth: 0～1のデバイス深度)。
float DepthOfFieldViewDepth(float depth)
{
    // RH透視のprojection33
    const float projectionA = DepthOfFieldProjection.x;
    // RH透視のprojection43
    const float projectionB = DepthOfFieldProjection.y;
    // 距離復元または分布の分母
    const float denominator = depth + projectionA;
    if (denominator > -1e-6f)
    {
        return 1e6f;
    }
    return projectionB / denominator;
}

// 焦点帯からの逆距離差で負が手前・正が奥のCoCを返す(viewDepth: 正の視点距離)。
float DepthOfFieldSignedCircleOfConfusion(float viewDepth)
{
    // 正の下限付きの焦点距離
    const float focus = max(DepthOfFieldParameters.x, 0.01f);
    // 焦点の合う帯の距離半幅
    const float halfRange =
        max(DepthOfFieldParameters.y, 0.0f) * 0.5f;
    // 焦点帯の手前距離
    const float nearEdge = max(focus - halfRange, 0.01f);
    // 焦点帯の奥の距離
    const float farEdge = focus + halfRange;

    // 焦点帯の近い境界距離
    float reference;
    // 手前-1・奥+1のCoC符号
    float direction;
    if (viewDepth < nearEdge)
    {
        reference = nearEdge;
        direction = -1.0f;
    }
    else if (viewDepth > farEdge)
    {
        reference = farEdge;
        direction = 1.0f;
    }
    else
    {
        return 0.0f;
    }

    // 焦点境界との逆距離差
    const float relative = abs(
        1.0f - reference / max(viewDepth, 0.001f));
    return direction
        * saturate(relative * max(DepthOfFieldParameters.z, 0.0f));
}

// 半解像度の色と2×2内で絶対値最大の符号付きCoCを書く(input: 半解像度の位置とUV)。
float4 PSDepthOfFieldPrepare(ScreenVertex input) : SV_Target
{
    // 処理前後のRGB
    const float3 color = SourceTexture.SampleLevel(
        LinearSampler,
        input.uv,
        0.0f).rgb;

    // 半解像度画素の元2×2基点
    // 輪郭を跨ぐ平均深度を避け、元2×2で絶対値最大の符号CoCを保つ。
    const int2 basePixel = int2(input.position.xy) * 2;
    // 絶対値最大の符号付きCoC
    float signedCoc = 0.0f;
    // 採取する縦画素オフセット
    [unroll]
    for (int y = 0; y < 2; ++y)
    {
        // 採取する横画素オフセット
        [unroll]
        for (int x = 0; x < 2; ++x)
        {
            // 採取したデバイス深度
            const float depth = DepthTexture.Load(
                int3(basePixel + int2(x, y), 0)).r;
            // 元画素の符号付きCoC
            const float candidate =
                DepthOfFieldSignedCircleOfConfusion(
                    DepthOfFieldViewDepth(depth));
            if (abs(candidate) > abs(signedCoc))
            {
                signedCoc = candidate;
            }
        }
    }
    return float4(color, signedCoc);
}

// 黄金角の円内採取で前後のCoCに応じてぼかす(input: 半解像度の位置とUV)。
float4 PSDepthOfFieldBlur(ScreenVertex input) : SV_Target
{
    // 処理画像の逆幅と逆高さ
    const float2 texel = DepthOfFieldTexel.xy;
    // 処理ごとの採取件数
    const int sampleCount = clamp(
        (int)DepthOfFieldTexel.z,
        4,
        64);
    // 半解像度の最大ぼけ半径
    const float maximumRadius =
        max(DepthOfFieldParameters.w, 0.0f) * 0.5f;

    // 中心のRGBと符号付きCoC
    const float4 center = DepthOfFieldTexture.SampleLevel(
        LinearSampler,
        input.uv,
        0.0f);
    // 中心の絶対値CoC
    const float centerCoc = abs(center.a);

    // 採取値の重み付き合計
    float3 total = center.rgb;
    // 採取値の重み合計
    float totalWeight = 1.0f;

    // 画素ごとの採取回転角rad
    const float rotation =
        AmbientOcclusionNoise(input.position.xy) * 6.28318531f;

    // 採取方向または採取点の番号
    [loop]
    for (int index = 0; index < sampleCount; ++index)
    {
        // 黄金角と画素回転の角度rad
        const float angle =
            (float)index * 2.39996323f + rotation;
        // 半解像度の円内採取半径
        const float radius = sqrt(
            ((float)index + 0.5f) / (float)sampleCount)
            * maximumRadius;
        // 処理画像の採取UV
        // 画面端でも採取数と重みを保つため、採取UVを端へ制限する。
    const float2 uv = clamp(
            input.uv
                + float2(cos(angle), sin(angle))
                    * radius * texel,
            0.0f,
            1.0f);
        // 採取するRGBと符号付きCoC
        const float4 tap = DepthOfFieldTexture.SampleLevel(
            LinearSampler,
            uv,
            0.0f);

        // 採取点の絶対値CoC
        const float tapCoc = abs(tap.a);
        // 前後関係で制限したぼけ量
        // 負CoCの前ぼけは奥へ広げ、後ぼけは中心CoCを上限として手前へ漏らさない。
        const float spread = tap.a < 0.0f
            ? tapCoc
            : min(tapCoc, centerCoc);
        // 採取または履歴の混合重み
        const float weight = saturate(
            spread * maximumRadius - radius + 1.0f);
        total += tap.rgb * weight;
        totalWeight += weight;
    }
    return float4(total / totalWeight, center.a);
}

// フル解像度のCoCで鮮明色と半解像度のぼけ色を混ぜる(input: フル解像度の位置とUV)。
float4 PSDepthOfFieldComposite(ScreenVertex input) : SV_Target
{
    // 元のフル解像度RGBA
    const float4 sharp = SourceTexture.Sample(
        LinearSampler,
        input.uv);
    // 採取したデバイス深度
    const float depth = DepthTexture.Sample(
        LinearSampler,
        input.uv).r;
    // フル解像度の絶対値CoC
    const float coc = abs(
        DepthOfFieldSignedCircleOfConfusion(
            DepthOfFieldViewDepth(depth)));
    // 半解像度のぼけRGB
    const float3 blurred = DepthOfFieldTexture.SampleLevel(
        LinearSampler,
        input.uv,
        0.0f).rgb;

    // 最大半径込みのぼけ混合比
    // フル解像度のCoCを読み直し、焦点内の鮮明色へ半解像度のぼけを混ぜない。
    const float mixAmount = saturate(
        coc * max(DepthOfFieldParameters.w, 0.0f));
    return float4(
        lerp(sharp.rgb, blurred, mixAmount),
        sharp.a);
}

// カメラの再投影速度に沿って前後対称に色を平均する(input: 画面位置とUV)。
float4 PSMotionBlur(ScreenVertex input) : SV_Target
{
    // 処理前の採取RGBA
    const float4 source =
        SourceTexture.Sample(LinearSampler, input.uv);
    // 採取したデバイス深度
    const float depth = DepthTexture.Sample(
        LinearSampler,
        input.uv).r;

    // 画面UVから作るNDC位置
    const float2 clip = float2(
        input.uv.x * 2.0f - 1.0f,
        1.0f - input.uv.y * 2.0f);
    // 深度復元のWorld同次位置
    const float4 worldHomogeneous = mul(
        float4(clip, depth, 1.0f),
        MotionBlurInverseViewProjection);
    if (worldHomogeneous.w <= 0.0001f)
    {
        return source;
    }
    // 深度復元したWorld位置
    const float3 worldPosition =
        worldHomogeneous.xyz / worldHomogeneous.w;

    // 前フレームの透視同次位置
    const float4 previousClip = mul(
        float4(worldPosition, 1.0f),
        MotionBlurPreviousViewProjection);
    if (previousClip.w <= 0.0001f)
    {
        return source;
    }
    // 再投影した前フレームUV
    const float2 previousUv = float2(
        previousClip.x / previousClip.w * 0.5f + 0.5f,
        0.5f - previousClip.y / previousClip.w * 0.5f);

    // カメラ再投影によるUV速度
    // 物体速度は扱わず、揺らしなしの現・前行列でカメラ移動分だけを求める。
    float2 velocity = (input.uv - previousUv)
        * max(MotionBlurParameters.x, 0.0f);
    // 画素単位のカメラ速度
    const float2 velocityPixels =
        velocity / max(MotionBlurTexel.xy, 1e-6f);
    // 速度の画素長
    const float lengthPixels = length(velocityPixels);
    // ブレの最大画素長
    const float limitPixels = max(MotionBlurParameters.y, 0.0f);
    if (lengthPixels < 0.5f || limitPixels <= 0.0f)
    {
        return source;
    }
    if (lengthPixels > limitPixels)
    {
        velocity *= limitPixels / lengthPixels;
    }

    // 処理ごとの採取件数
    const int sampleCount = clamp(
        (int)MotionBlurParameters.z,
        2,
        32);
    // 採取値の重み付き合計
    float3 total = source.rgb;
    // 採取値の重み合計
    float totalWeight = 1.0f;
    // 採取方向または採取点の番号
    [loop]
    for (int index = 0; index < sampleCount; ++index)
    {
        // 現在位置を中心とする採取比
        const float offset =
            ((float)index + 0.5f) / (float)sampleCount - 0.5f;
        // 処理画像の採取UV
        // 画面端でも採取数と重みを保つため、採取UVを端へ制限する。
    const float2 uv = clamp(
            input.uv + velocity * offset,
            0.0f,
            1.0f);
        total += SourceTexture.SampleLevel(
            LinearSampler,
            uv,
            0.0f).rgb;
        totalWeight += 1.0f;
    }
    return float4(total / totalWeight, source.a);
}

// HDRの4採取平均から自動露出用の自然対数輝度を書く(input: 四分解像度の位置とUV)。
float4 PSLuminance(ScreenVertex input) : SV_Target
{
    // 元画素1個分のUV採取幅
    const float2 offset = LuminanceTexel.xy * 0.25f;
    // 採取値の重み付き合計
    float3 total = 0.0f;
    total += SourceTexture.SampleLevel(
        LinearSampler,
        input.uv + float2(-offset.x, -offset.y),
        0.0f).rgb;
    total += SourceTexture.SampleLevel(
        LinearSampler,
        input.uv + float2(offset.x, -offset.y),
        0.0f).rgb;
    total += SourceTexture.SampleLevel(
        LinearSampler,
        input.uv + float2(-offset.x, offset.y),
        0.0f).rgb;
    total += SourceTexture.SampleLevel(
        LinearSampler,
        input.uv + float2(offset.x, offset.y),
        0.0f).rgb;
    // 4点の平均RGBの輝度
    const float average = Luminance(max(total * 0.25f, 0.0f));
    // CPUが最小Mipを読んで露出を決めるため、平均後の輝度を自然対数で出力する。
    return log(max(average, 1e-4f)).xxxx;
}


// 散乱位相に使う円周率
static const float LamaPonVolumetricPi = 3.14159265f;

// Henyey-Greensteinの前方散乱係数を求める(cosineAngle: 視線と光源方向の内積, scattering: 前方散乱の強さ)。
float VolumetricPhase(float cosineAngle, float scattering)
{
    // 0～0.95の前方散乱係数
    const float g = clamp(scattering, 0.0f, 0.95f);
    // 前方散乱係数の二乗
    const float gSquared = g * g;
    // 距離復元または分布の分母
    const float denominator =
        1.0f + gSquared - 2.0f * g * cosineAngle;
    return (1.0f - gSquared)
        / (4.0f * LamaPonVolumetricPi
            * pow(max(denominator, 0.0001f), 1.5f));
}

// 範囲内の先頭Cascadeで可視率を読み範囲外は1を返す(worldPosition: 採取するWorld位置)。
float VolumetricShadowAt(float3 worldPosition)
{
    // 有効な平行光Cascade件数
    const int cascadeCount =
        (int)VolumetricShadowParameters.x;
    // 採取するCascade番号
    [loop]
    for (int cascade = 0; cascade < 4; ++cascade)
    {
        if (cascade >= cascadeCount)
        {
            break;
        }
        // Cascade影透視の同次位置
        const float4 lightPosition = mul(
            float4(worldPosition, 1.0f),
            VolumetricCascades[cascade]);
        if (lightPosition.w <= 0.0001f)
        {
            continue;
        }
        // 影透視除算後のXYZ
        const float3 projected =
            lightPosition.xyz / lightPosition.w;
        // Cascade影画像の採取UV
        const float2 shadowUv =
            projected.xy * float2(0.5f, -0.5f) + 0.5f;
        if (shadowUv.x < 0.0f || shadowUv.x > 1.0f
            || shadowUv.y < 0.0f || shadowUv.y > 1.0f
            || projected.z <= 0.0f
            || projected.z >= 1.0f)
        {
            continue;
        }
        return VolumetricShadowTexture.SampleCmpLevelZero(
            VolumetricShadowSampler,
            float3(shadowUv, cascade),
            projected.z
                - VolumetricShadowParameters.y);
    }
    return 1.0f;
}

// 深度までのレイで平行光の影を積算して散乱光を加える(input: 画面位置とUV)。
float4 PSVolumetricLight(ScreenVertex input) : SV_Target
{
    // 散乱光を加える元RGBA
    const float4 sceneColor =
        SourceTexture.Sample(LinearSampler, input.uv);

    // 採取したデバイス深度
    const float depth =
        DepthTexture.Sample(LinearSampler, input.uv).r;
    // 画面UVから作るNDC位置
    const float2 clip = float2(
        input.uv.x * 2.0f - 1.0f,
        1.0f - input.uv.y * 2.0f);
    // 深度復元のWorld同次位置
    const float4 worldHomogeneous = mul(
        float4(clip, depth, 1.0f),
        VolumetricInverseViewProjection);
    if (worldHomogeneous.w <= 0.0001f)
    {
        return sceneColor;
    }
    // 深度復元したWorld位置
    const float3 worldPosition =
        worldHomogeneous.xyz / worldHomogeneous.w;

    // 視点のWorld位置
    const float3 cameraPosition =
        VolumetricCameraPosition.xyz;
    // 視点から表面への差分
    const float3 toPixel = worldPosition - cameraPosition;
    // 視点から表面のWorld距離
    const float pixelDistance = length(toPixel);
    if (pixelDistance <= 0.0001f)
    {
        return sceneColor;
    }
    // 視点から表面への単位方向
    const float3 rayDirection = toPixel / pixelDistance;
    // 上限内の散乱光探索距離
    const float marchDistance = min(
        pixelDistance,
        VolumetricCameraPosition.w);

    // 処理ごとの採取件数
    const int sampleCount =
        (int)max(VolumetricLightDirection.w, 1.0f);
    // 散乱光採取のWorld間隔
    const float stepLength =
        marchDistance / (float)sampleCount;

    // 画素ごとの探索ずらし乱数
    const float dither = frac(
        52.9829189f
        * frac(dot(
            input.position.xy,
            float2(0.06711056f, 0.00583715f))));
    // 現在採取点までのWorld距離
    float travelled =
        stepLength * (0.5f + dither * 0.5f);

    // 視線と逆光進行方向の内積
    // 光方向は光源からの進行方向なので、散乱角は逆方向との内積で測る。
    const float cosineAngle = dot(
        rayDirection,
        -normalize(VolumetricLightDirection.xyz));
    // Henyey-Greenstein位相係数
    const float phase = VolumetricPhase(
        cosineAngle,
        VolumetricLightColor.w);

    // 各採取点の光可視率の合計
    float accumulated = 0.0f;
    // 散乱光の採取番号
    [loop]
    for (int step = 0; step < sampleCount; ++step)
    {
        // レイ上のWorld採取位置
        const float3 samplePosition =
            cameraPosition + rayDirection * travelled;
        accumulated +=
            VolumetricShadowAt(samplePosition);
        travelled += stepLength;
    }
    // 遮蔽後に残る光の倍率
    const float visibility =
        accumulated / (float)sampleCount;

    // 距離補正した散乱光RGB
    const float3 scatter =
        VolumetricLightColor.rgb
        * visibility
        * phase
        * (marchDistance
            / max(VolumetricCameraPosition.w, 0.0001f));
    return float4(
        sceneColor.rgb + max(scatter, 0.0f),
        sceneColor.a);
}

// 深度で履歴を再投影し現画像の3×3色範囲へ制限して混ぜる(input: 画面位置とUV)。
float4 PSTemporalAntiAliasing(ScreenVertex input)
    : SV_Target
{
    // 現フレームの中心RGB
    // 物体速度は持たず、3×3の現画像色範囲で再投影した履歴の残像を抑える。
    const float3 current =
        SourceTexture.Sample(LinearSampler, input.uv).rgb;

    // 採取したデバイス深度
    const float depth =
        DepthTexture.Sample(LinearSampler, input.uv).r;

    // 画面UVから作るNDC位置
    const float2 clip = float2(
        input.uv.x * 2.0f - 1.0f,
        1.0f - input.uv.y * 2.0f);
    // 深度復元のWorld同次位置
    const float4 worldHomogeneous = mul(
        float4(clip, depth, 1.0f),
        TemporalInverseViewProjection);
    if (worldHomogeneous.w <= 0.0001f)
    {
        return float4(current, 1.0f);
    }
    // 深度復元したWorld位置
    const float3 worldPosition =
        worldHomogeneous.xyz / worldHomogeneous.w;

    // 前フレームの透視同次位置
    const float4 previousClip = mul(
        float4(worldPosition, 1.0f),
        TemporalPreviousViewProjection);
    if (previousClip.w <= 0.0001f)
    {
        return float4(current, 1.0f);
    }
    // 以前の透視除算後のXYZ
    const float3 previousProjected =
        previousClip.xyz / previousClip.w;
    // 再投影した前フレームUV
    const float2 previousUv = float2(
        previousProjected.x * 0.5f + 0.5f,
        0.5f - previousProjected.y * 0.5f);
    if (previousUv.x < 0.0f || previousUv.x > 1.0f
        || previousUv.y < 0.0f || previousUv.y > 1.0f)
    {
        return float4(current, 1.0f);
    }

    // 処理画像の逆幅と逆高さ
    const float2 texel = TemporalParameters.zw;
    // 近傍範囲と補正後の最小RGB
    float3 minimumColor = current;
    // 近傍範囲と補正後の最大RGB
    float3 maximumColor = current;
    // 画面縦方向の1画素UV
    [unroll]
    for (int offsetY = -1; offsetY <= 1; ++offsetY)
    {
        // 画面横方向の1画素UV
        [unroll]
        for (int offsetX = -1; offsetX <= 1; ++offsetX)
        {
            if (offsetX == 0 && offsetY == 0)
            {
                continue;
            }
            // 3×3内の近傍RGB
            const float3 neighbour =
                SourceTexture.Sample(
                    LinearSampler,
                    input.uv
                        + float2(
                            (float)offsetX * texel.x,
                            (float)offsetY * texel.y)).rgb;
            minimumColor = min(minimumColor, neighbour);
            maximumColor = max(maximumColor, neighbour);
        }
    }
    // 履歴制限範囲の拡張倍率
    const float tolerance = max(
        TemporalParameters.y,
        0.0f);
    // 近傍色範囲の中央RGB
    const float3 middle =
        (minimumColor + maximumColor) * 0.5f;
    // 許容倍率込みの色範囲半幅
    const float3 extent =
        (maximumColor - minimumColor) * 0.5f * tolerance;
    minimumColor = middle - extent;
    maximumColor = middle + extent;

    // 再投影UVの履歴RGB
    // 未描画の空も無限遠として再投影し、画面外の履歴だけを除外する。
    const float3 history =
        TemporalHistoryTexture.Sample(
            LinearSampler,
            previousUv).rgb;
    // 近傍色範囲に制限した履歴RGB
    const float3 clampedHistory = clamp(
        history,
        minimumColor,
        maximumColor);

    // 制限後の履歴を混ぜる比率
    const float weight = saturate(TemporalParameters.x);
    return float4(
        lerp(current, clampedHistory, weight),
        1.0f);
}

// RHで描いたCube面をD3D配置に合わせて左右反転する(input: 画面位置とUV)。
float4 PSCopyMirrorX(ScreenVertex input) : SV_Target
{
    return SourceTexture.Sample(
        LinearSampler,
        float2(1.0f - input.uv.x, input.uv.y));
}


// D3DのCube面UVから単位方向を作る(face: D3Dの0～5の面番号, uv: Cube面内のUV)。
float3 CubeDirection(uint face, float2 uv)
{
    // Cube面内の-1～1座標
    // Cubeの面番号はD3D順の+X・-X・+Y・-Y・+Z・-Zに保つ。
    const float2 st = float2(
        uv.x * 2.0f - 1.0f,
        1.0f - uv.y * 2.0f);
    if (face == 0u)
    {
        return normalize(float3(1.0f, st.y, -st.x));
    }
    if (face == 1u)
    {
        return normalize(float3(-1.0f, st.y, st.x));
    }
    if (face == 2u)
    {
        return normalize(float3(st.x, 1.0f, -st.y));
    }
    if (face == 3u)
    {
        return normalize(float3(st.x, -1.0f, st.y));
    }
    if (face == 4u)
    {
        return normalize(float3(st.x, st.y, 1.0f));
    }
    return normalize(float3(-st.x, st.y, -1.0f));
}

// 32bitを反転して0～1のVan der Corput値を作る(bits: 反転する整数)。
float RadicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u)
        | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u)
        | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u)
        | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u)
        | ((bits & 0xFF00FF00u) >> 8u);
    return (float)bits * 2.3283064365386963e-10f;
}

// 等間隔とbit反転から2次元の採取列を作る(index: 採取番号, count: 正の採取件数)。
float2 Hammersley(uint index, uint count)
{
    return float2(
        (float)index / (float)count,
        RadicalInverseVdC(index));
}

// GGX分布の中間方向を指定法線の接空間へ変換する(xi: 0～1の2次元採取値, roughness: 表面の粗さ, normal: Cube面の単位法線)。
float3 ImportanceSampleGGX(
    float2 xi,
    float roughness,
    float3 normal)
{
    // GGXの二乗粗さ幅
    const float alpha = roughness * roughness;
    // 半球の方位角rad
    const float phi = 6.2831853f * xi.x;
    // GGX天頂角の余弦
    const float cosTheta = sqrt(
        (1.0f - xi.y)
        / (1.0f + (alpha * alpha - 1.0f) * xi.y));
    // GGX天頂角の正弦
    const float sinTheta =
        sqrt(1.0f - cosTheta * cosTheta);
    // 接空間のGGX中間方向
    const float3 halfVector = float3(
        sinTheta * cos(phi),
        sinTheta * sin(phi),
        cosTheta);
    // 平行を避ける接空間補助軸
    const float3 up =
        abs(normal.z) < 0.999f
            ? float3(0.0f, 0.0f, 1.0f)
            : float3(1.0f, 0.0f, 0.0f);
    // 半球内の接単位方向
    const float3 tangent =
        normalize(cross(up, normal));
    // 半球内の従接単位方向
    const float3 bitangent = cross(normal, tangent);
    return normalize(
        tangent * halfVector.x
        + bitangent * halfVector.y
        + normal * halfVector.z);
}

// Cubeの鏡面反射を64点のGGX分布で事前畳み込みする(input: Cube面の位置とUV)。
float4 PSPrefilterEnvironment(
    ScreenVertex input) : SV_Target
{
    // D3Dの0～5のCube面番号
    const uint face = (uint)PrefilterParameters.x;
    // 事前畳み込み用の粗さ
    const float roughness = PrefilterParameters.y;
    // 畳み込み元Cubeの面解像度
    const float sourceResolution =
        max(PrefilterParameters.z, 1.0f);
    // Cube面UVの単位方向
    const float3 normal =
        CubeDirection(face, input.uv);

    if (roughness <= 0.001f)
    {
        return float4(
            SkyCubemap.SampleLevel(
                LinearSampler,
                normal,
                0.0f).rgb,
            1.0f);
    }

    // 64点のGGX採取件数
    const uint SampleCount = 64u;
    // Cubeの1画素の立体角
    // 採取確率と1画素の立体角から元Mipを選び、鏡面畳み込みのちらつきを抑える。
    const float saTexel =
        4.0f * 3.14159265f
        / (6.0f * sourceResolution
            * sourceResolution);
    // 処理前後のRGB
    float3 color = 0.0f.xxx;
    // 採取方向の内積の重み合計
    float weight = 0.0f;
    // 採取方向または採取点の番号
    [loop]
    for (uint index = 0u; index < SampleCount; ++index)
    {
        // GGX分布の中間単位方向
        const float3 halfVector = ImportanceSampleGGX(
            Hammersley(index, SampleCount),
            roughness,
            normal);
        // GGX採取の光源単位方向
        const float3 lightDirection = normalize(
            2.0f * dot(normal, halfVector) * halfVector
            - normal);
        // 面法線と採取方向の内積
        const float normalDotLight =
            saturate(dot(normal, lightDirection));
        if (normalDotLight <= 0.0f)
        {
            continue;
        }
        // 面法線と中間方向の内積
        const float normalDotHalf =
            saturate(dot(normal, halfVector));
        // GGXの二乗粗さ幅
        const float alpha = roughness * roughness;
        // GGX分布分母の中間項
        const float denominator =
            normalDotHalf * normalDotHalf
                * (alpha * alpha - 1.0f)
            + 1.0f;
        // GGX法線分布密度
        const float distribution =
            alpha * alpha
            / (3.14159265f
                * denominator * denominator);
        // GGX採取の確率密度
        const float pdf =
            distribution * normalDotHalf
                / (4.0f * max(normalDotHalf, 0.0001f))
            + 0.0001f;
        // 採取1点の立体角
        const float saSample =
            1.0f / ((float)SampleCount * pdf);
        // 立体角比で選ぶ採取Mip
        const float mip =
            0.5f * log2(saSample / saTexel);
        color +=
            SkyCubemap.SampleLevel(
                LinearSampler,
                lightDirection,
                max(mip, 0.0f)).rgb
            * normalDotLight;
        weight += normalDotLight;
    }
    return float4(color / max(weight, 0.0001f), 1.0f);
}

// Cubeの拡散光を32×8点の半球コサイン重みで畳み込む(input: Cube面の位置とUV)。
float4 PSIrradiance(ScreenVertex input) : SV_Target
{
    // D3Dの0～5のCube面番号
    const uint face = (uint)PrefilterParameters.x;
    // Cube面UVの単位方向
    const float3 normal =
        CubeDirection(face, input.uv);
    // 平行を避ける接空間補助軸
    const float3 up =
        abs(normal.z) < 0.999f
            ? float3(0.0f, 0.0f, 1.0f)
            : float3(1.0f, 0.0f, 0.0f);
    // 半球内の接単位方向
    const float3 tangent =
        normalize(cross(up, normal));
    // 半球内の従接単位方向
    const float3 bitangent = cross(normal, tangent);

    // 半球コサイン重みの累積RGB
    float3 irradiance = 0.0f.xxx;
    // 半球採取のcos・sin重み合計
    float weight = 0.0f;
    // 32方向の方位角間隔rad
    // 方位角32×天頂角8の半段ずらしで、cosθ・sinθを重みにする。
    const float PhiStep = 6.2831853f / 32.0f;
    // 8段階の天頂角間隔rad
    const float ThetaStep = 1.5707963f / 8.0f;
    // 半球の方位角番号
    [loop]
    for (uint phiIndex = 0u; phiIndex < 32u; ++phiIndex)
    {
        // 半球の方位角rad
        const float phi = (float)phiIndex * PhiStep;
        // 半球の天頂角番号
        [loop]
        for (uint thetaIndex = 0u;
            thetaIndex < 8u;
            ++thetaIndex)
        {
            // 半段ずらした天頂角rad
            const float theta =
                ((float)thetaIndex + 0.5f) * ThetaStep;
            // 半球のWorld採取単位方向
            const float3 direction =
                tangent * (sin(theta) * cos(phi))
                + bitangent * (sin(theta) * sin(phi))
                + normal * cos(theta);
            // cosとsinの半球重み
            const float sampleWeight =
                cos(theta) * sin(theta);
            irradiance +=
                SkyCubemap.SampleLevel(
                    LinearSampler,
                    direction,
                    2.0f).rgb
                * sampleWeight;
            weight += sampleWeight;
        }
    }
    return float4(
        irradiance / max(weight, 0.0001f),
        1.0f);
}
