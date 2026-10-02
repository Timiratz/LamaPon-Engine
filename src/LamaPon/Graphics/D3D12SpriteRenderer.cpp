#include "LamaPon/Graphics/D3D12SpriteRenderer.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/D3D12Backend.h"
#include "LamaPon/Graphics/EnvironmentSettings.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/RenderTargetBackendState.h"
#include "LamaPon/Graphics/ShaderCompiler.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace
{
    // 1回で送信する最大矩形数
    constexpr std::size_t MaximumSpritesPerDraw = 2048u;
    // 矩形1枚に必要な頂点数
    constexpr std::size_t VerticesPerSprite = 4u;
    // 矩形1枚に必要な三角形索引数
    constexpr std::size_t IndicesPerSprite = 6u;

    // 標準スプライトと全画面効果のHLSL
    constexpr char SpriteShaderSource[] = R"(
// VSのb0へ渡す画素変換倍率
cbuffer SpriteViewport : register(b0)
{
    // 画素をNDCへ移すXY倍率
    float2 ViewportScale;
};

// PSのb1へ渡す効果別の16定数
cbuffer FullscreenPass : register(b1)
{
    // 効果別の第1定数ベクトル
    float4 PassPrimary;
    // 効果別の第2定数ベクトル
    float4 PassSecondary;
    // 効果別の第3定数ベクトル
    float4 PassTertiary;
    // 効果別の第4定数ベクトル
    float4 PassQuaternary;
};

// TAAと動きぼかしの32行列定数
cbuffer TemporalPass : register(b2)
{
    // 現在のワールド復元行列
    row_major float4x4 TemporalInverseViewProjection;
    // 履歴を描いた合成射影行列
    row_major float4x4 TemporalPreviousViewProjection;
};

// C++の384バイトと対応するb3
cbuffer VolumetricPass : register(b3)
{
    // 光の筋用のワールド復元行列
    row_major float4x4 VolumetricInverseViewProjection;
    // 視点XYZと光の筋の最大距離
    float4 VolumetricCameraPosition;
    // 光源方向XYZと採取点数
    float4 VolumetricLightDirection;
    // 光源RGBと散乱係数
    float4 VolumetricLightColor;
    // 影カスケード別の描画行列
    row_major float4x4 VolumetricCascades[4];
    // 影の段数・バイアス・画素幅
    float4 VolumetricShadowParameters;
};

// t0の主入力画像
Texture2D SpriteTexture : register(t0);
// t1の履歴または光条画像
Texture2D TemporalHistoryTexture : register(t1);
// t2のデバイス深度画像
Texture2D DepthTexture : register(t2);
// t3の光の筋用の影配列
Texture2DArray<float> VolumetricShadowTexture : register(t3);
// s0の線形端固定サンプラー
SamplerState SpriteSampler : register(s0);
// s1の白境界の深度比較
SamplerComparisonState VolumetricShadowSampler : register(s1);

struct VertexInput
{
    // 画面内の画素座標と層深度
    float3 position : POSITION;
    // 頂点のRGBA色
    float4 color : COLOR;
    // 入力画像のUV座標
    float2 textureCoordinate : TEXCOORD;
};

// 外部PSMainとの互換のためCOLOR0・TEXCOORD0・SV_Positionの順を維持します。
struct PixelInput
{
    // 頂点のRGBA色
    float4 color : COLOR;
    // 入力画像のUV座標
    float2 textureCoordinate : TEXCOORD;
    // 頂点のクリップ座標
    float4 position : SV_Position;
};

// 画素位置をクリップ座標へ変換して色とUVを渡します(input: 画素位置・RGBA色・UV)。
PixelInput SpriteVertexShader(VertexInput input)
{
    // 画素段へ渡す頂点出力
    PixelInput output;
    output.position = float4(
        input.position.x * ViewportScale.x - 1.0f,
        1.0f - input.position.y * ViewportScale.y,
        input.position.z,
        1.0f);
    output.color = input.color;
    output.textureCoordinate = input.textureCoordinate;
    return output;
}

// 主画像に頂点色を掛けたRGBAを返します(input: 色・UV・射影位置)。
float4 SpritePixelShader(PixelInput input) : SV_Target
{
    return SpriteTexture.Sample(SpriteSampler, input.textureCoordinate)
        * input.color;
}

// Rec.601の係数でRGBを輝度へ変換します(color: 入力RGB)。
float Luminance(float3 color)
{
    return dot(color, float3(0.299f, 0.587f, 0.114f));
}

// ACES近似曲線でHDRを表示範囲へ圧縮します(color: 露出補正済みのHDR値)。
float3 ACESFilm(float3 color)
{
    return saturate(
        color * (2.51f * color + 0.03f)
        / (color * (2.43f * color + 0.59f) + 0.14f));
}

// 露出・色調・周辺減光を適用し、gamma変換せずUNORMへ出力します(input: 色・画面UV・射影位置)。
float4 ToneMappedPixelShader(PixelInput input) : SV_Target
{
    // x露出・y対比・z彩度・w色温度
    const float4 ColorGradePrimary = PassPrimary;
    // x色合い・y減光・z有無・w露出補正
    const float4 ColorGradeSecondary = PassSecondary;
    // 色調整するHDRからLDRのRGB
    float3 color = max(
        SpriteTexture.Sample(
            SpriteSampler,
            input.textureCoordinate).rgb * input.color.rgb,
        0.0f);
    // 色調整を有効にする重み
    const float gradingEnabled = saturate(ColorGradeSecondary.z);
    // 有効時の手動露出段数
    const float exposure = lerp(
        0.0f,
        ColorGradePrimary.x,
        gradingEnabled);
    // 有効時の色温度補正
    const float temperature = lerp(
        0.0f,
        ColorGradePrimary.w,
        gradingEnabled);
    // 有効時の緑と紫の色合い補正
    const float tint = lerp(
        0.0f,
        ColorGradeSecondary.x,
        gradingEnabled);
    // 色温度と色合いのRGB倍率
    const float3 whiteBalance = max(float3(
        1.0f + temperature * 0.16f - tint * 0.05f,
        1.0f + tint * 0.10f,
        1.0f - temperature * 0.16f - tint * 0.05f),
        0.05f);
    color *= exp2(exposure + ColorGradeSecondary.w) * whiteBalance;
    color = ACESFilm(color);

    // トーン変換後の輝度
    const float luminance = Luminance(color);
    color = lerp(
        luminance.xxx,
        color,
        lerp(1.0f, max(ColorGradePrimary.z, 0.0f), gradingEnabled));
    color = (color - 0.5f)
        * lerp(1.0f, max(ColorGradePrimary.y, 0.0f), gradingEnabled)
        + 0.5f;

    // 画面中心基準の正規化UV
    const float2 centered = input.textureCoordinate * 2.0f - 1.0f;
    // 周辺を暗くする形状の倍率
    const float vignetteShape = saturate(
        1.0f - dot(centered, centered) * 0.42f);
    color *= lerp(
        1.0f,
        vignetteShape,
        lerp(
            0.0f,
            saturate(ColorGradeSecondary.y),
            gradingEnabled));
    // UNORMへgamma変換せず書き、トーン変換なしの表示と明るさの基準を合わせます。
    return float4(saturate(color), 1.0f);
}

// しきい値を超える主画像のRGBだけを抽出します(uv: 主画像を読むUV)。
float3 BrightColor(float2 uv)
{
    // 抽出対象の主画像RGB
    const float3 color = SpriteTexture.Sample(SpriteSampler, uv).rgb;
    // RGBの最大成分で測る明るさ
    const float brightness = max(
        color.r,
        max(color.g, color.b));
    return color * saturate(
        (brightness - PassPrimary.z)
        / max(brightness, 0.0001f));
}

// 高輝度RGBを9点でぼかして主画像へ加算します(input: 色・画面UV・射影位置)。
float4 BloomPixelShader(PixelInput input) : SV_Target
{
    // 主画像を読むUV
    const float2 uv = input.textureCoordinate;
    // ブルームを加える元のRGBA
    const float4 source = SpriteTexture.Sample(SpriteSampler, uv);
    // ブルームのXY参照間隔
    const float2 offset = PassPrimary.xy * PassSecondary.x;
    // 9点で蓄積する高輝度RGB
    float3 bloom = BrightColor(uv) * 0.2f;
    bloom += BrightColor(uv + float2(offset.x, 0.0f)) * 0.12f;
    bloom += BrightColor(uv - float2(offset.x, 0.0f)) * 0.12f;
    bloom += BrightColor(uv + float2(0.0f, offset.y)) * 0.12f;
    bloom += BrightColor(uv - float2(0.0f, offset.y)) * 0.12f;
    bloom += BrightColor(uv + offset) * 0.08f;
    bloom += BrightColor(uv - offset) * 0.08f;
    bloom += BrightColor(uv + float2(offset.x, -offset.y)) * 0.08f;
    bloom += BrightColor(uv + float2(-offset.x, offset.y)) * 0.08f;
    return float4(source.rgb + bloom * PassPrimary.w, source.a);
}

// 上下左右の輝度差に沿って色を平滑化します(input: 色・画面UV・射影位置)。
float4 FxaaPixelShader(PixelInput input) : SV_Target
{
    // 主画像を読むUV
    const float2 uv = input.textureCoordinate;
    // 出力1画素のUV幅
    const float2 texel = PassPrimary.xy;
    // 中心の入力RGB
    const float3 center = SpriteTexture.Sample(SpriteSampler, uv).rgb;
    // 中心の輝度
    const float lumaCenter = Luminance(center);
    // 上隣の輝度
    const float lumaNorth = Luminance(
        SpriteTexture.Sample(
            SpriteSampler,
            uv + float2(0.0f, -texel.y)).rgb);
    // 下隣の輝度
    const float lumaSouth = Luminance(
        SpriteTexture.Sample(
            SpriteSampler,
            uv + float2(0.0f, texel.y)).rgb);
    // 左隣の輝度
    const float lumaWest = Luminance(
        SpriteTexture.Sample(
            SpriteSampler,
            uv + float2(-texel.x, 0.0f)).rgb);
    // 右隣の輝度
    const float lumaEast = Luminance(
        SpriteTexture.Sample(
            SpriteSampler,
            uv + float2(texel.x, 0.0f)).rgb);
    // 中心と4近傍の最小輝度
    const float lumaMinimum = min(
        lumaCenter,
        min(min(lumaNorth, lumaSouth), min(lumaWest, lumaEast)));
    // 中心と4近傍の最大輝度
    const float lumaMaximum = max(
        lumaCenter,
        max(max(lumaNorth, lumaSouth), max(lumaWest, lumaEast)));
    if (lumaMaximum - lumaMinimum < 0.0312f)
    {
        return float4(center, 1.0f);
    }

    // 輝度境界に沿うUV方向
    float2 direction = float2(
        -(lumaNorth - lumaSouth),
        lumaWest - lumaEast);
    // 境界方向の計算を安定させる値
    const float reduction = max(
        (lumaNorth + lumaSouth + lumaWest + lumaEast)
            * 0.03125f,
        0.0078125f);
    // 方向の短い軸と補正値の逆数
    const float inverseMinimum =
        1.0f / (min(abs(direction.x), abs(direction.y)) + reduction);
    direction = clamp(
        direction * inverseMinimum,
        -8.0f,
        8.0f) * texel;

    // 方向内側の2点平均RGB
    const float3 first =
        0.5f * (
            SpriteTexture.Sample(
                SpriteSampler,
                uv + direction * (1.0f / 3.0f - 0.5f)).rgb
            + SpriteTexture.Sample(
                SpriteSampler,
                uv + direction * (2.0f / 3.0f - 0.5f)).rgb);
    // 方向の両端も加えた平均RGB
    const float3 second =
        first * 0.5f
        + 0.25f * (
            SpriteTexture.Sample(
                SpriteSampler,
                uv + direction * -0.5f).rgb
            + SpriteTexture.Sample(
                SpriteSampler,
                uv + direction * 0.5f).rgb);
    // 4点平均候補の輝度
    const float secondLuma = Luminance(second);
    return float4(
        secondLuma < lumaMinimum || secondLuma > lumaMaximum
            ? first
            : second,
        1.0f);
}

// 4回の線形読出しで覆う領域を平均して対数輝度を返します(input: 色・縮小先UV・射影位置)。
float4 LuminancePixelShader(PixelInput input) : SV_Target
{
    // 縮小先画素のUV
    const float2 uv = input.textureCoordinate;
    // フル解像度1画素分のUV幅
    const float2 offset = PassPrimary.xy * 0.25f;
    // 4回の読出しのRGB合計
    float3 total = float3(0.0f, 0.0f, 0.0f);
    total += SpriteTexture.SampleLevel(
        SpriteSampler,
        uv + float2(-offset.x, -offset.y),
        0.0f).rgb;
    total += SpriteTexture.SampleLevel(
        SpriteSampler,
        uv + float2(offset.x, -offset.y),
        0.0f).rgb;
    total += SpriteTexture.SampleLevel(
        SpriteSampler,
        uv + float2(-offset.x, offset.y),
        0.0f).rgb;
    total += SpriteTexture.SampleLevel(
        SpriteSampler,
        uv + float2(offset.x, offset.y),
        0.0f).rgb;
    // RGB平均から求めた輝度
    const float average = Luminance(max(total * 0.25f, 0.0f));
    return log(max(average, 1e-4f)).xxxx;
}

// 深度で履歴UVを再投影し、近傍色域に制限した履歴を混ぜます(input: 色・画面UV・射影位置)。
float4 TemporalPixelShader(PixelInput input) : SV_Target
{
    // 現在フレームのRGB
    const float3 current = SpriteTexture.Sample(
        SpriteSampler,
        input.textureCoordinate).rgb;
    // 現在画素のデバイス深度
    const float depth = DepthTexture.Sample(
        SpriteSampler,
        input.textureCoordinate).r;
    // 画面UVから復元したNDCのXY
    const float2 clip = float2(
        input.textureCoordinate.x * 2.0f - 1.0f,
        1.0f - input.textureCoordinate.y * 2.0f);
    // 逆射影後の同次ワールド座標
    const float4 worldHomogeneous = mul(
        float4(clip, depth, 1.0f),
        TemporalInverseViewProjection);
    if (worldHomogeneous.w <= 0.0001f)
    {
        return float4(current, 1.0f);
    }
    // 復元したワールド位置
    const float3 worldPosition =
        worldHomogeneous.xyz / worldHomogeneous.w;
    // 履歴フレームのクリップ座標
    const float4 previousClip = mul(
        float4(worldPosition, 1.0f),
        TemporalPreviousViewProjection);
    if (previousClip.w <= 0.0001f)
    {
        return float4(current, 1.0f);
    }
    // 履歴フレームのNDC座標
    const float3 previousProjected = previousClip.xyz / previousClip.w;
    // 履歴画像を読むUV
    const float2 previousUv = float2(
        previousProjected.x * 0.5f + 0.5f,
        0.5f - previousProjected.y * 0.5f);
    if (previousUv.x < 0.0f || previousUv.x > 1.0f
        || previousUv.y < 0.0f || previousUv.y > 1.0f)
    {
        return float4(current, 1.0f);
    }

    // 現在画像1画素のUV幅
    const float2 texel = PassPrimary.zw;
    // 3×3近傍の最小RGB
    float3 minimumColor = current;
    // 3×3近傍の最大RGB
    float3 maximumColor = current;
    // 近傍画素の縦オフセット
    [unroll]
    for (int offsetY = -1; offsetY <= 1; ++offsetY)
    {
        // 近傍画素の横オフセット
        [unroll]
        for (int offsetX = -1; offsetX <= 1; ++offsetX)
        {
            if (offsetX == 0 && offsetY == 0)
            {
                continue;
            }
            // 近傍画素のRGB
            const float3 neighbour = SpriteTexture.Sample(
                SpriteSampler,
                input.textureCoordinate
                    + float2(offsetX, offsetY) * texel).rgb;
            minimumColor = min(minimumColor, neighbour);
            maximumColor = max(maximumColor, neighbour);
        }
    }
    // 近傍色域の中央RGB
    const float3 middle = (minimumColor + maximumColor) * 0.5f;
    // 許容値を掛けた色域の半幅
    const float3 extent = (maximumColor - minimumColor)
        * 0.5f * max(PassPrimary.y, 0.0f);
    // 再投影した履歴画像のRGB
    const float3 history = TemporalHistoryTexture.Sample(
        SpriteSampler,
        previousUv).rgb;
    return float4(
        lerp(
            current,
            clamp(history, middle - extent, middle + extent),
            saturate(PassPrimary.x)),
        1.0f);
}

// 再投影で得た速度方向のRGBを平均し、透過度を保持します(input: 色・画面UV・射影位置)。
float4 MotionBlurPixelShader(PixelInput input) : SV_Target
{
    // ぼかす前の入力RGBA
    const float4 source = SpriteTexture.Sample(
        SpriteSampler,
        input.textureCoordinate);
    // 現在画素のデバイス深度
    const float depth = DepthTexture.Sample(
        SpriteSampler,
        input.textureCoordinate).r;
    // 画面UVから復元したNDCのXY
    const float2 clip = float2(
        input.textureCoordinate.x * 2.0f - 1.0f,
        1.0f - input.textureCoordinate.y * 2.0f);
    // 逆射影後の同次ワールド座標
    const float4 worldHomogeneous = mul(
        float4(clip, depth, 1.0f),
        TemporalInverseViewProjection);
    if (worldHomogeneous.w <= 0.0001f)
    {
        return source;
    }
    // 復元したワールド位置
    const float3 worldPosition =
        worldHomogeneous.xyz / worldHomogeneous.w;
    // 前フレームのクリップ座標
    const float4 previousClip = mul(
        float4(worldPosition, 1.0f),
        TemporalPreviousViewProjection);
    if (previousClip.w <= 0.0001f)
    {
        return source;
    }
    // 前フレームへ再投影したUV
    const float2 previousUv = float2(
        previousClip.x / previousClip.w * 0.5f + 0.5f,
        0.5f - previousClip.y / previousClip.w * 0.5f);

    // 入力画像1画素のUV幅
    const float2 texel = float2(PassPrimary.w, PassSecondary.x);
    // 強度を掛けた画面UV速度
    float2 velocity = (input.textureCoordinate - previousUv)
        * max(PassPrimary.x, 0.0f);
    // 画素単位の画面速度
    const float2 velocityPixels = velocity / max(texel, 1e-6f);
    // 画面速度の画素単位の長さ
    const float lengthPixels = length(velocityPixels);
    // ぼかし半径の画素単位の上限
    const float limitPixels = max(PassPrimary.y, 0.0f);
    if (lengthPixels < 0.5f || limitPixels <= 0.0f)
    {
        return source;
    }
    if (lengthPixels > limitPixels)
    {
        velocity *= limitPixels / lengthPixels;
    }

    // 速度方向の採取点数
    const int sampleCount = clamp((int)PassPrimary.z, 2, 32);
    // 採取したRGBの合計
    float3 total = source.rgb;
    // 中心を含む採取数
    float totalWeight = 1.0f;
    // 速度方向の採取番号
    [loop]
    for (int index = 0; index < sampleCount; ++index)
    {
        // 中心前後の採取比率
        const float offset =
            ((float)index + 0.5f) / (float)sampleCount - 0.5f;
        // 画面内に制限した採取UV
        const float2 uv = clamp(
            input.textureCoordinate + velocity * offset,
            0.0f,
            1.0f);
        total += SpriteTexture.SampleLevel(
            SpriteSampler,
            uv,
            0.0f).rgb;
        totalWeight += 1.0f;
    }
    return float4(total / totalWeight, source.a);
}

// 射影係数からビュー距離を復元し、遠平面は十分遠い値にします(deviceDepth: デバイス深度)。
float DepthOfFieldSceneDistance(float deviceDepth)
{
    // 深度を距離へ戻す分母
    const float denominator = deviceDepth + PassTertiary.x;
    return denominator > -1e-6f
        ? 1e6f
        : PassTertiary.y / denominator;
}

// 焦点帯の前後に応じた符号付きぼけ量を返します(viewDepth: 正のビュー距離)。
float DepthOfFieldSignedCircleOfConfusion(float viewDepth)
{
    // 焦点帯の中心距離
    const float focus = max(PassPrimary.x, 0.01f);
    // 焦点帯の距離の半幅
    const float halfRange = max(PassPrimary.y, 0.0f) * 0.5f;
    // 焦点帯の近い側の境界
    const float nearEdge = max(focus - halfRange, 0.01f);
    // 焦点帯の遠い側の境界
    const float farEdge = focus + halfRange;
    // 前景または背景の焦点境界
    float reference;
    // 前景なら負・背景なら正の符号
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
    // 焦点境界に対する距離のずれ
    const float relative = abs(
        1.0f - reference / max(viewDepth, 0.001f));
    return direction * saturate(relative * max(PassPrimary.z, 0.0f));
}

// 画素位置から採取角度用の擬似乱数を返します(pixel: 画素座標)。
float DepthOfFieldNoise(float2 pixel)
{
    return frac(
        52.9829189f
        * frac(dot(pixel, float2(0.06711056f, 0.00583715f))));
}

// 深度に応じた採取重みで焦点帯の外をぼかします(input: 色・画面UV・射影位置)。
float4 DepthOfFieldPixelShader(PixelInput input) : SV_Target
{
    // ぼかす前の入力RGBA
    const float4 sharp = SpriteTexture.Sample(
        SpriteSampler,
        input.textureCoordinate);
    // 中心画素のデバイス深度
    const float centerDepth = DepthTexture.Sample(
        SpriteSampler,
        input.textureCoordinate).r;
    // 中心画素の符号付きぼけ量
    const float centerSignedCoc = DepthOfFieldSignedCircleOfConfusion(
        DepthOfFieldSceneDistance(centerDepth));
    // 中心画素のぼけ量の絶対値
    const float centerCoc = abs(centerSignedCoc);
    // 採取半径の画素単位の上限
    const float maximumRadius = max(PassPrimary.w, 0.0f);
    // ぼかし色を混ぜる比率
    const float mixAmount = saturate(centerCoc * maximumRadius);
    if (mixAmount <= 0.0f || maximumRadius <= 0.0f)
    {
        return sharp;
    }

    // 円盤内の採取点数
    const int sampleCount = clamp((int)PassSecondary.x, 4, 64);
    // 入力画像1画素のUV幅
    const float2 texel = PassSecondary.yz;
    // 画素ごとに変える採取角度
    const float rotation = DepthOfFieldNoise(input.position.xy)
        * 6.28318531f;
    // 採取重みを掛けたRGB合計
    float3 total = sharp.rgb;
    // 中心を含む採取重みの合計
    float totalWeight = 1.0f;
    // 円盤内の採取番号
    [loop]
    for (int index = 0; index < sampleCount; ++index)
    {
        // 黄金角と画素回転を足した角度
        const float angle = (float)index * 2.39996323f + rotation;
        // 採取点の画素単位の中心距離
        const float radius = sqrt(
            ((float)index + 0.5f) / (float)sampleCount)
            * maximumRadius;
        // 画面内に制限した採取UV
        const float2 uv = clamp(
            input.textureCoordinate
                + float2(cos(angle), sin(angle)) * radius * texel,
            0.0f,
            1.0f);
        // 採取画素のデバイス深度
        const float tapDepth = DepthTexture.SampleLevel(
            SpriteSampler,
            uv,
            0.0f).r;
        // 採取画素の符号付きぼけ量
        const float tapSignedCoc = DepthOfFieldSignedCircleOfConfusion(
            DepthOfFieldSceneDistance(tapDepth));
        // 採取画素のぼけ量の絶対値
        const float tapCoc = abs(tapSignedCoc);
        // 前後関係を考慮した広がり量
        const float spread = tapSignedCoc < 0.0f
            ? tapCoc
            : min(tapCoc, centerCoc);
        // 採取半径に応じた混合重み
        const float weight = saturate(
            spread * maximumRadius - radius + 1.0f);
        total += SpriteTexture.SampleLevel(
            SpriteSampler,
            uv,
            0.0f).rgb * weight;
        totalWeight += weight;
    }
    return float4(
        lerp(sharp.rgb, total / totalWeight, mixAmount),
        sharp.a);
}

// 射影係数からビュー距離を復元し、遠平面を十分遠く扱います(deviceDepth: デバイス深度)。
float OutlineSceneDistance(float deviceDepth)
{
    // 深度を距離へ戻す分母
    const float denominator = deviceDepth + PassTertiary.x;
    return denominator > -1e-6f
        ? 1e6f
        : PassTertiary.y / denominator;
}

// 輪郭用の画素座標を入力画像内に制限します(pixel: 参照する画素座標)。
int2 OutlineClampPixel(int2 pixel)
{
    // 参照範囲を制限する画像寸法
    const int2 size = max(int2(PassQuaternary.zw), int2(1, 1));
    return clamp(pixel, int2(0, 0), size - 1);
}

// 深度と射影係数からビュー位置を復元します(pixel: 深度画像の画素座標)。
float3 OutlineViewPosition(int2 pixel)
{
    // 画面内に制限した画素座標
    const int2 safePixel = OutlineClampPixel(pixel);
    // 画素のデバイス深度
    const float depth = DepthTexture.Load(int3(safePixel, 0)).r;
    // 復元した正のビュー距離
    const float distance = OutlineSceneDistance(depth);
    // 画素中心の画面UV
    const float2 uv = (float2(safePixel) + 0.5f) * PassQuaternary.xy;
    // 画素中心のNDC座標
    const float2 ndc = float2(
        uv.x * 2.0f - 1.0f,
        1.0f - uv.y * 2.0f);
    return float3(
        ndc.x * PassTertiary.z * distance,
        ndc.y * PassTertiary.w * distance,
        distance);
}

// 中心との深度差が小さい側を選んで輪郭用の法線を復元します(pixel: 法線を求める画素座標)。
float3 OutlineNormal(int2 pixel)
{
    // 参照範囲を制限する画像寸法
    const int2 size = max(int2(PassQuaternary.zw), int2(3, 3));
    // 画面内に制限した画素座標
    const int2 safePixel = clamp(
        pixel,
        int2(1, 1),
        max(size - 2, int2(1, 1)));
    // 中心画素のビュー位置
    const float3 origin = OutlineViewPosition(safePixel);
    // 左隣画素のビュー位置
    const float3 left = OutlineViewPosition(safePixel + int2(-1, 0));
    // 右隣画素のビュー位置
    const float3 right = OutlineViewPosition(safePixel + int2(1, 0));
    // 上隣画素のビュー位置
    const float3 up = OutlineViewPosition(safePixel + int2(0, -1));
    // 下隣画素のビュー位置
    const float3 down = OutlineViewPosition(safePixel + int2(0, 1));
    // 段差が小さい側の横方向差
    const float3 horizontal = abs(left.z - origin.z)
            < abs(right.z - origin.z)
        ? origin - left
        : right - origin;
    // 段差が小さい側の縦方向差
    const float3 vertical = abs(up.z - origin.z)
            < abs(down.z - origin.z)
        ? up - origin
        : origin - down;
    // 近傍位置差から求めた法線
    const float3 normal = cross(vertical, horizontal);
    // 復元法線の長さの二乗
    const float lengthSquared = dot(normal, normal);
    return lengthSquared < 1e-12f
        ? float3(0.0f, 0.0f, -1.0f)
        : normal * rsqrt(lengthSquared);
}

// 輪郭を調べる8方向の画素差
static const int2 OutlineDirections[8] = {
    int2(-1, -1), int2(0, -1), int2(1, -1), int2(-1, 0),
    int2(1, 0), int2(-1, 1), int2(0, 1), int2(1, 1)
};

// 8方向の深度差と法線差で輪郭色を合成します(input: 色・画面UV・射影位置)。
float4 ScreenOutlinePixelShader(PixelInput input) : SV_Target
{
    // 輪郭を加える元のRGBA
    const float4 source = SpriteTexture.Sample(
        SpriteSampler,
        input.textureCoordinate);
    // 入力深度画像の幅と高さ
    const int2 screenSize = int2(PassQuaternary.zw);
    if (screenSize.x < 3 || screenSize.y < 3)
    {
        return source;
    }
    // 中心画素の整数座標
    const int2 pixel = int2(input.position.xy);
    // 中心画素のビュー距離
    const float centerDistance = OutlineSceneDistance(
        DepthTexture.Load(int3(OutlineClampPixel(pixel), 0)).r);
    // 中心画素の単位法線
    const float3 centerNormal = OutlineNormal(pixel);
    // 輪郭参照の画素単位の半径
    const int radius = clamp((int)PassSecondary.x, 1, 4);
    // 深度の相対差のしきい値
    const float depthThreshold = max(PassSecondary.y, 0.0001f);
    // 法線方向差のしきい値
    const float normalThreshold = max(PassSecondary.z, 0.0001f);
    // 深度差による輪郭の強さ
    float depthEdge = 0.0f;
    // 法線差による輪郭の強さ
    float normalEdge = 0.0f;
    // 8方向の参照番号
    [unroll]
    for (int index = 0; index < 8; ++index)
    {
        // 比較先の整数画素座標
        const int2 samplePixel = pixel + OutlineDirections[index] * radius;
        // 比較先画素のビュー距離
        const float sampleDistance = OutlineSceneDistance(
            DepthTexture.Load(
                int3(OutlineClampPixel(samplePixel), 0)).r);
        // 中心画素が遠平面にある状態
        const bool centerIsSky = centerDistance >= 999999.0f;
        // 比較先画素が遠平面にある状態
        const bool sampleIsSky = sampleDistance >= 999999.0f;
        if (centerIsSky != sampleIsSky)
        {
            depthEdge = 1.0f;
        }
        else if (!centerIsSky)
        {
            // 中心距離に対する深度差の比率
            const float relativeDifference = abs(
                sampleDistance - centerDistance)
                / max(centerDistance, 0.001f);
            depthEdge = max(
                depthEdge,
                smoothstep(
                    0.35f,
                    1.0f,
                    relativeDifference / depthThreshold));
            // 中心と比較先の法線の方向差
            const float normalDifference = 1.0f - saturate(dot(
                centerNormal,
                OutlineNormal(samplePixel)));
            normalEdge = max(
                normalEdge,
                smoothstep(
                    0.35f,
                    1.0f,
                    normalDifference / normalThreshold));
        }
    }
    // 強度を適用した輪郭の混合率
    const float edge = saturate(
        max(depthEdge, normalEdge) * saturate(PassPrimary.w));
    return float4(
        lerp(source.rgb, PassPrimary.rgb, edge),
        source.a);
}

// 射影係数でビュー距離を復元し、遠平面を十分遠く扱います(deviceDepth: デバイス深度)。
float AmbientOcclusionSceneDistance(float deviceDepth)
{
    // 深度を距離へ戻す分母
    const float denominator = deviceDepth + PassTertiary.x;
    if (denominator > -1e-6f)
    {
        return 1e6f;
    }
    return PassTertiary.y / denominator;
}

// UVと深度から正のZを持つビュー位置を復元します(uv: 深度画像のUV, deviceDepth: デバイス深度)。
float3 AmbientOcclusionViewPosition(float2 uv, float deviceDepth)
{
    // 復元した正のビュー距離
    const float viewZ = AmbientOcclusionSceneDistance(deviceDepth);
    // 画面UVから得るNDCのXY
    const float2 ndc = float2(
        uv.x * 2.0f - 1.0f,
        1.0f - uv.y * 2.0f);
    return float3(
        ndc.x * PassTertiary.z * viewZ,
        ndc.y * PassTertiary.w * viewZ,
        viewZ);
}

// 指定UVの深度を読んでビュー位置を復元します(uv: 深度画像のUV)。
float3 AmbientOcclusionViewPositionAt(float2 uv)
{
    return AmbientOcclusionViewPosition(
        uv,
        DepthTexture.SampleLevel(SpriteSampler, uv, 0.0f).r);
}

// 上下左右の段差が小さい側を選んで法線を復元します(uv: 中心の画面UV, origin: 中心のビュー位置, texelSize: 遮蔽画像1画素のUV幅)。
float3 AmbientOcclusionReconstructNormal(
    float2 uv,
    float3 origin,
    float2 texelSize)
{
    // 遮蔽画像1画素の横UV差
    const float2 offsetX = float2(texelSize.x, 0.0f);
    // 遮蔽画像1画素の縦UV差
    const float2 offsetY = float2(0.0f, texelSize.y);
    // 左隣のビュー位置
    const float3 left = AmbientOcclusionViewPositionAt(uv - offsetX);
    // 右隣のビュー位置
    const float3 right = AmbientOcclusionViewPositionAt(uv + offsetX);
    // 上隣のビュー位置
    const float3 up = AmbientOcclusionViewPositionAt(uv - offsetY);
    // 下隣のビュー位置
    const float3 down = AmbientOcclusionViewPositionAt(uv + offsetY);
    // 段差が小さい側の横方向差
    const float3 horizontal = abs(left.z - origin.z)
            < abs(right.z - origin.z)
        ? origin - left
        : right - origin;
    // 段差が小さい側の縦方向差
    const float3 vertical = abs(up.z - origin.z)
            < abs(down.z - origin.z)
        ? up - origin
        : origin - down;
    // 深度から復元するビュー法線
    const float3 normal = cross(vertical, horizontal);
    // 復元法線の長さの二乗
    const float lengthSquared = dot(normal, normal);
    if (lengthSquared < 1e-12f)
    {
        return float3(0.0f, 0.0f, -1.0f);
    }
    return normal * rsqrt(lengthSquared);
}

// 画素位置から採取角度用の擬似乱数を返します(pixel: 画素座標)。
float AmbientOcclusionNoise(float2 pixel)
{
    return frac(
        52.9829189f
        * frac(dot(pixel, float2(0.06711056f, 0.00583715f))));
}

// 近傍の深度を探索し、残る明るさをRへ出力します(input: 色・半解像度UV・射影位置)。
float4 AmbientOcclusionPixelShader(PixelInput input) : SV_Target
{
    // 遮蔽画像の画面UV
    const float2 uv = input.textureCoordinate;
    // 中心のデバイス深度
    const float depth = DepthTexture.SampleLevel(
        SpriteSampler,
        uv,
        0.0f).r;
    if (depth >= 0.999999f)
    {
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    // 中心画素のビュー位置
    const float3 origin = AmbientOcclusionViewPosition(uv, depth);
    // 遮蔽画像1画素のUV幅
    const float2 texelSize = PassPrimary.xy;
    // ビュー空間の遮蔽探索半径
    const float radius = PassPrimary.z;
    // 遮蔽の強度倍率
    const float strength = PassPrimary.w;
    // 深度から復元するビュー法線
    const float3 normal = AmbientOcclusionReconstructNormal(
        uv,
        origin,
        texelSize);
    // ビュー距離で割った探索半径
    const float projectedRadius = radius / max(origin.z, 0.001f);
    // 画素ごとに変える採取角度
    const float rotation = AmbientOcclusionNoise(
        uv / max(texelSize.x, 1e-6f)
            * float2(1.0f, texelSize.x / max(texelSize.y, 1e-6f)))
        * 6.2831853f;
    // 遮蔽を探索する採取点数
    const int sampleCount = clamp(int(PassSecondary.x), 4, 32);
    // 正規化前後の遮蔽量
    float occlusion = 0.0f;
    // 探索点の番号
    [loop]
    for (int index = 0; index < sampleCount; ++index)
    {
        // 採取点の正規化進行率
        const float fraction =
            (float(index) + 0.5f) / float(sampleCount);
        // 画素回転とらせんの採取角度
        const float angle = rotation + fraction * 18.849556f;
        // 円盤内の正規化採取半径
        const float spiralDistance = sqrt(fraction);
        // 近傍深度を読む画面UV
        const float2 sampleUv = uv
            + float2(cos(angle), sin(angle))
                * spiralDistance * projectedRadius * 0.5f;
        if (any(sampleUv < 0.0f) || any(sampleUv > 1.0f))
        {
            continue;
        }
        // 採取先の深度またはビュー距離
        const float sampleDepth = DepthTexture.SampleLevel(
            SpriteSampler,
            sampleUv,
            0.0f).r;
        if (sampleDepth >= 0.999999f)
        {
            continue;
        }
        // 中心から採取位置へのビュー差
        float3 difference = AmbientOcclusionViewPosition(
            sampleUv,
            sampleDepth) - origin;
        // 採取位置差の長さの二乗
        const float length2 = dot(difference, difference);
        if (length2 < 1e-8f)
        {
            continue;
        }
        difference *= rsqrt(length2);
        // 中心から採取位置への距離
        const float sampleDistance = sqrt(length2);
        // 法線より手前側の遮蔽度合い
        const float facing = saturate(dot(normal, difference) - 0.06f);
        // 探索半径の末端での減衰率
        const float falloff = saturate(
            1.0f - sampleDistance / max(radius, 0.001f));
        occlusion += facing * falloff;
    }
    occlusion = saturate(
        occlusion / float(sampleCount) * 2.4f * strength);
    // 遮蔽後に残る明るさ
    const float visibility = 1.0f - occlusion;
    return float4(visibility, visibility, visibility, 1.0f);
}

// 深度が近い画素だけを混ぜて遮蔽率を平滑化します(input: 色・半解像度UV・射影位置)。
float4 AmbientOcclusionBlurPixelShader(PixelInput input) : SV_Target
{
    // 遮蔽画像の画面UV
    const float2 uv = input.textureCoordinate;
    // 遮蔽画像1画素のUV幅
    const float2 texelSize = PassPrimary.xy;
    // 中心画素のビュー距離
    const float centerDepth = AmbientOcclusionSceneDistance(
        DepthTexture.SampleLevel(SpriteSampler, uv, 0.0f).r);
    // 深度差の重みを決める距離幅
    const float depthScale = max(centerDepth * 0.08f, 0.05f);
    // 混合重みを掛けた遮蔽率合計
    float total = 0.0f;
    // 採取先の混合重みの合計
    float weightSum = 0.0f;
    // ブラー採取の縦方向オフセット
    [unroll]
    for (int y = -2; y <= 1; ++y)
    {
        // ブラー採取の横方向オフセット
        [unroll]
        for (int x = -2; x <= 1; ++x)
        {
            // 近傍深度を読む画面UV
            const float2 sampleUv = uv + float2(
                (float(x) + 0.5f) * texelSize.x,
                (float(y) + 0.5f) * texelSize.y);
            // 採取先の深度またはビュー距離
            const float sampleDepth = AmbientOcclusionSceneDistance(
                DepthTexture.SampleLevel(
                    SpriteSampler,
                    sampleUv,
                    0.0f).r);
            // 中心からの距離差の正規化値
            const float depthRatio =
                (sampleDepth - centerDepth) / depthScale;
            // 深度差によるブラー混合重み
            const float weight =
                1.0f / (1.0f + depthRatio * depthRatio);
            total += SpriteTexture.SampleLevel(
                SpriteSampler,
                sampleUv,
                0.0f).r * weight;
            weightSum += weight;
        }
    }
    // 遮蔽後に残る明るさ
    const float visibility = weightSum > 0.0f
        ? total / weightSum
        : SpriteTexture.SampleLevel(SpriteSampler, uv, 0.0f).r;
    return float4(visibility, visibility, visibility, 1.0f);
}

// 散乱計算に使う円周率
static const float VolumetricPi = 3.14159265f;

// Henyey–Greenstein位相関数で散乱方向の重みを返します(cosineAngle: 視線と光方向のcos, scattering: 前方散乱の係数)。
float VolumetricPhase(float cosineAngle, float scattering)
{
    // 上限0.95に制限した散乱係数
    const float g = clamp(scattering, 0.0f, 0.95f);
    // 散乱係数の二乗
    const float gSquared = g * g;
    // 位相関数の角度依存の分母
    const float denominator =
        1.0f + gSquared - 2.0f * g * cosineAngle;
    return (1.0f - gSquared)
        / (4.0f * VolumetricPi
            * pow(max(denominator, 0.0001f), 1.5f));
}

// 投影内に入る最初のカスケードで光の可視率を読みます(worldPosition: 光の筋を採取する位置)。
float VolumetricShadowAt(float3 worldPosition)
{
    // 影の有効カスケード数
    const int cascadeCount = (int)VolumetricShadowParameters.x;
    // 影を調べるカスケード番号
    [loop]
    for (int cascade = 0; cascade < 4; ++cascade)
    {
        if (cascade >= cascadeCount)
        {
            break;
        }
        // 採取位置のライト射影座標
        const float4 lightPosition = mul(
            float4(worldPosition, 1.0f),
            VolumetricCascades[cascade]);
        if (lightPosition.w <= 0.0001f)
        {
            continue;
        }
        // ライトのNDC座標
        const float3 projected = lightPosition.xyz / lightPosition.w;
        // 影配列を読むUV
        const float2 shadowUv =
            projected.xy * float2(0.5f, -0.5f) + 0.5f;
        if (shadowUv.x < 0.0f || shadowUv.x > 1.0f
            || shadowUv.y < 0.0f || shadowUv.y > 1.0f
            || projected.z <= 0.0f || projected.z >= 1.0f)
        {
            continue;
        }
        return VolumetricShadowTexture.SampleCmpLevelZero(
            VolumetricShadowSampler,
            float3(shadowUv, cascade),
            projected.z - VolumetricShadowParameters.y);
    }
    return 1.0f;
}

// 視点レイ上の影を平均して散乱光を加算します(input: 色・画面UV・射影位置)。
float4 VolumetricLightPixelShader(PixelInput input) : SV_Target
{
    // 光の筋を加える元のRGBA
    const float4 sceneColor = SpriteTexture.Sample(
        SpriteSampler,
        input.textureCoordinate);
    // 画素のデバイス深度
    const float depth = DepthTexture.Sample(
        SpriteSampler,
        input.textureCoordinate).r;
    // 画面UVから得るNDCのXY
    const float2 clip = float2(
        input.textureCoordinate.x * 2.0f - 1.0f,
        1.0f - input.textureCoordinate.y * 2.0f);
    // 逆射影後の同次ワールド座標
    const float4 worldHomogeneous = mul(
        float4(clip, depth, 1.0f),
        VolumetricInverseViewProjection);
    if (worldHomogeneous.w <= 0.0001f)
    {
        return sceneColor;
    }
    // 復元した画素のワールド位置
    const float3 worldPosition =
        worldHomogeneous.xyz / worldHomogeneous.w;
    // 視点のワールド位置
    const float3 cameraPosition = VolumetricCameraPosition.xyz;
    // 視点から画素への位置差
    const float3 toPixel = worldPosition - cameraPosition;
    // 視点から画素までの距離
    const float pixelDistance = length(toPixel);
    if (pixelDistance <= 0.0001f)
    {
        return sceneColor;
    }
    // 視点から画素への単位方向
    const float3 rayDirection = toPixel / pixelDistance;
    // 最大距離で制限した探索長
    const float marchDistance = min(
        pixelDistance,
        VolumetricCameraPosition.w);
    // レイ上の光の採取点数
    const int sampleCount = (int)max(VolumetricLightDirection.w, 1.0f);
    // レイ上の採取点の間隔
    const float stepLength = marchDistance / (float)sampleCount;
    // 画素ごとの開始位置の乱数
    const float dither = frac(
        52.9829189f
        * frac(dot(
            input.position.xy,
            float2(0.06711056f, 0.00583715f))));
    // 視点から採取点までの距離
    float travelled = stepLength * (0.5f + dither * 0.5f);
    // 視線と入射光方向のcos
    const float cosineAngle = dot(
        rayDirection,
        -normalize(VolumetricLightDirection.xyz));
    // 視線方向の散乱重み
    const float phase = VolumetricPhase(
        cosineAngle,
        VolumetricLightColor.w);
    // 採取点の光の可視率合計
    float accumulated = 0.0f;
    // レイ上の採取番号
    [loop]
    for (int step = 0; step < sampleCount; ++step)
    {
        // 光を調べるワールド位置
        const float3 samplePosition =
            cameraPosition + rayDirection * travelled;
        accumulated += VolumetricShadowAt(samplePosition);
        travelled += stepLength;
    }
    // レイ上で平均した光の可視率
    const float visibility = accumulated / (float)sampleCount;
    // 距離・影・位相を適用したRGB
    const float3 scatter = VolumetricLightColor.rgb
        * visibility
        * phase
        * (marchDistance / max(VolumetricCameraPosition.w, 0.0001f));
    return float4(sceneColor.rgb + max(scatter, 0.0f), sceneColor.a);
}

// 画面内のしきい値以上のRGBを滑らかに抽出します(uv: 主画像を読むUV)。
float3 LensFlareBright(float2 uv)
{
    if (any(uv < 0.0f) || any(uv > 1.0f))
    {
        return 0.0f;
    }
    // 高輝度抽出する元のRGB
    const float3 color = SpriteTexture.Sample(SpriteSampler, uv).rgb;
    // 元RGBの最大成分
    const float brightness = max(color.r, max(color.g, color.b));
    // 高輝度抽出のしきい値
    const float threshold = max(PassPrimary.z, 0.0f);
    // しきい値付近の抽出混合率
    const float gate = smoothstep(
        threshold,
        threshold + max(threshold * 0.35f, 0.25f),
        brightness);
    return color * gate;
}

// 方向に沿うRGB別のUV差で色収差を付けます(uv: 採取中心のUV, direction: 色をずらす方向)。
float3 LensFlareChromaticSample(float2 uv, float2 direction)
{
    // 色収差でずらすUVの幅
    const float chromatic = saturate(PassSecondary.z) * 0.015f;
    // 色収差を付けるUV差
    const float2 offset = direction * chromatic;
    // 正方向へずらした採取RGB
    const float3 red = LensFlareBright(uv + offset);
    // 中心で採取したRGB
    const float3 green = LensFlareBright(uv);
    // 負方向へずらした採取RGB
    const float3 blue = LensFlareBright(uv - offset);
    return float3(red.r, green.g, blue.b);
}

// 指定方向の5点フィルターで光条を伸ばします(input: 色・画面UV・射影位置)。
float4 LensFlareStreakPixelShader(PixelInput input) : SV_Target
{
    // 光条の採取間隔のUV幅
    const float stride = PassQuaternary.x;
    // 光条を伸ばす方向の数
    const int directionCount = clamp((int)PassQuaternary.y, 1, 4);
    // 光条の基準角度
    const float baseAngle = PassQuaternary.z;
    // 元画像から高輝度を抽出する段
    const bool firstPass = PassQuaternary.w > 0.5f;
    // 方向別の重み付きRGB合計
    float3 total = 0.0f;
    // 光条採取重みの合計
    float weightTotal = 0.0f;
    // 光条方向またはゴーストの番号
    [loop]
    for (int index = 0; index < directionCount; ++index)
    {
        // 光条を伸ばす採取角度
        const float angle = baseAngle
            + 3.14159265f * (float)index / (float)directionCount;
        // 光条の単位UV方向
        const float2 axis = float2(cos(angle), sin(angle));
        // 光条の中心前後の採取番号
        [unroll]
        for (int tap = -2; tap <= 2; ++tap)
        {
            // 入力画像を読む画面UV
            const float2 uv = clamp(
                input.textureCoordinate + axis * ((float)tap * stride),
                0.0f,
                1.0f);
            // 中心からの距離による採取重み
            const float weight = 1.0f - abs((float)tap) * 0.22f;
            // 光条に付ける色分散の強さ
            const float dispersion = saturate(PassSecondary.z);
            // 採取点による赤青の強度差
            const float shift = (float)tap / 2.0f * dispersion;
            // 光条の採取RGB
            float3 sample = firstPass
                ? LensFlareBright(uv)
                : SpriteTexture.SampleLevel(SpriteSampler, uv, 0.0f).rgb;
            if (dispersion > 0.0f)
            {
                sample *= float3(1.0f + shift, 1.0f, 1.0f - shift);
            }
            total += sample * weight;
            weightTotal += weight;
        }
    }
    total /= max(weightTotal, 0.0001f);
    return float4(total, 1.0f);
}

// 4個のゴースト・ハロー・光条を主画像へ加算します(input: 色・画面UV・射影位置)。
float4 LensFlareCompositePixelShader(PixelInput input) : SV_Target
{
    // 入力画像を読む画面UV
    const float2 uv = input.textureCoordinate;
    // フレアを加える元のRGBA
    const float4 source = SpriteTexture.Sample(SpriteSampler, uv);
    // 画面中心のUV座標
    const float2 center = float2(0.5f, 0.5f);
    // 画面中心からのUV差
    const float2 fromCenter = uv - center;
    // 画面中心からのUV距離
    const float radius = length(fromCenter);
    // 画面中心からの単位UV方向
    const float2 direction = radius > 0.0001f
        ? fromCenter / radius
        : float2(1.0f, 0.0f);
    // ゴースト・ハロー・光条のRGB
    float3 flare = LensFlareBright(uv) * 0.22f;
    // ゴーストの広がり倍率
    const float dispersal = max(PassSecondary.x, 0.01f);
    // 光条方向またはゴーストの番号
    [unroll]
    for (int index = 1; index <= 4; ++index)
    {
        // 対象ゴーストの中心反転倍率
        const float scale = dispersal * (float)index;
        // 対象ゴーストを読むUV
        const float2 ghostUv = center - fromCenter * scale;
        // 対象ゴーストのRGB混合重み
        const float ghostWeight = 0.23f - (float)index * 0.025f;
        flare += LensFlareChromaticSample(ghostUv, direction)
            * max(ghostWeight, 0.05f);
    }
    // ハロー円周のUV半径
    const float haloRadius = clamp(PassSecondary.y, 0.05f, 1.5f);
    // ハロー円周からのUV距離
    const float haloDistance = abs(radius - haloRadius);
    // ハロー円周近傍の混合率
    const float halo = 1.0f - smoothstep(
        0.015f,
        0.10f + haloRadius * 0.18f,
        haloDistance);
    // ハローの反対側を読むUV
    const float2 haloUv = center - direction * haloRadius;
    flare += LensFlareChromaticSample(haloUv, direction) * halo * 0.32f;
    // 別パスで生成した光条RGB
    const float3 streak = TemporalHistoryTexture.SampleLevel(
        SpriteSampler,
        uv,
        0.0f).rgb;
    flare += streak * PassSecondary.w;
    return float4(
        source.rgb + flare * max(PassPrimary.w, 0.0f),
        source.a);
}

// 深度をビュー距離へ変換してHi-Z最下段へ出力します(input: 色・画面UV・射影位置)。
float4 ReflectionDepthLinearizePixelShader(PixelInput input) : SV_Target
{
    // 深度を読む整数画素座標
    const int2 pixel = int2(input.position.xy);
    // 入力画像のデバイス深度
    const float deviceDepth = SpriteTexture.Load(int3(pixel, 0)).r;
    // 深度を距離へ戻す分母
    const float denominator = deviceDepth + PassPrimary.x;
    if (denominator > -1e-6f)
    {
        return float4(1e6f, 1e6f, 1e6f, 1e6f);
    }
    // 復元した正のビュー距離
    const float sceneDistance = PassPrimary.y / denominator;
    return float4(
        sceneDistance,
        sceneDistance,
        sceneDistance,
        sceneDistance);
}

// 親段の最小距離を求め、奇数寸法の余剰行列も取り込みます(input: 色・縮小先UV・射影位置)。
float4 ReflectionDepthDownsamplePixelShader(PixelInput input) : SV_Target
{
    // 親ミップの幅と高さ
    const int2 parentSize = int2(PassPrimary.xy);
    // 縮小元の2×2領域の左上
    const int2 parent = int2(input.position.xy) * 2;
    // 親ミップの最後の画素座標
    const int2 last = parentSize - 1;
    // 縮小領域の左上の距離
    const float a = SpriteTexture.Load(int3(min(parent, last), 0)).r;
    // 縮小領域の右上の距離
    const float b = SpriteTexture.Load(
        int3(min(parent + int2(1, 0), last), 0)).r;
    // 縮小領域の左下の距離
    const float c = SpriteTexture.Load(
        int3(min(parent + int2(0, 1), last), 0)).r;
    // 縮小領域の右下の距離
    const float d = SpriteTexture.Load(
        int3(min(parent + int2(1, 1), last), 0)).r;
    // 領域内で最も手前の距離
    float nearest = min(min(a, b), min(c, d));
    // 親段の幅が奇数の状態
    const bool oddWidth = (parentSize.x & 1) != 0;
    // 親段の高さが奇数の状態
    const bool oddHeight = (parentSize.y & 1) != 0;
    if (oddWidth)
    {
        nearest = min(nearest, SpriteTexture.Load(
            int3(min(parent + int2(2, 0), last), 0)).r);
        nearest = min(nearest, SpriteTexture.Load(
            int3(min(parent + int2(2, 1), last), 0)).r);
    }
    if (oddHeight)
    {
        nearest = min(nearest, SpriteTexture.Load(
            int3(min(parent + int2(0, 2), last), 0)).r);
        nearest = min(nearest, SpriteTexture.Load(
            int3(min(parent + int2(1, 2), last), 0)).r);
    }
    if (oddWidth && oddHeight)
    {
        nearest = min(nearest, SpriteTexture.Load(
            int3(min(parent + int2(2, 2), last), 0)).r);
    }
    return float4(nearest, nearest, nearest, nearest);
}
)";

    // b1の4色・b2の視線・t1の空HLSL
    constexpr char SkyShaderSource[] = R"(
// 空のb1へ渡す4色の定数
cbuffer SkyColors : register(b1)
{
    // 天頂RGBと空の強度
    float4 TopColor;
    // 水平線のRGB
    float4 HorizonColor;
    // 地面側のRGB
    float4 GroundColor;
    // 太陽RGBと円盤の有無
    float4 SunDiskColor;
};

// 空のb2へ渡す視線用32定数
cbuffer SkyView : register(b2)
{
    // 視線復元用の逆合成射影行列
    row_major float4x4 InverseViewProjection;
    // 視点のワールド位置
    float4 CameraPosition;
    // 太陽方向XYZと角半径
    float4 SunDirection;
    // キューブ使用指定と予約成分
    float4 SkyOptions;
};

// t1の空のキューブ画像
TextureCube SkyCubemap : register(t1);
// s0の線形端固定サンプラー
SamplerState SkySampler : register(s0);

struct PixelInput
{
    // 頂点色または空の合成RGB
    float4 color : COLOR;
    // 画面全体を覆うUV
    float2 textureCoordinate : TEXCOORD;
    // 頂点のクリップ座標
    float4 position : SV_Position;
};

// 視線方向のキューブまたは空グラデーションと太陽を描きます(input: 色・画面UV・射影位置)。
float4 SkyPixelShader(PixelInput input) : SV_Target
{
    // 画面UVから得るNDCのXY
    const float2 clip = float2(
        input.textureCoordinate.x * 2.0f - 1.0f,
        1.0f - input.textureCoordinate.y * 2.0f);
    // 遠平面の同次ワールド座標
    const float4 farPosition = mul(
        float4(clip, 1.0f, 1.0f),
        InverseViewProjection);
    // 遠平面のワールド位置
    const float3 worldPosition =
        farPosition.xyz / max(abs(farPosition.w), 0.00001f);
    // 視点から遠平面への単位方向
    const float3 direction = normalize(
        worldPosition - CameraPosition.xyz);
    if (SkyOptions.x > 0.5f)
    {
        // キューブから読んだ空のRGB
        const float3 cubeColor = SkyCubemap.SampleLevel(
            SkySampler,
            direction,
            0.0f).rgb;
        return float4(
            cubeColor * max(TopColor.a, 0.0f),
            1.0f);
    }
    // 水平線から天頂への混合率
    const float above = smoothstep(
        -0.03f, 0.85f, direction.y);
    // 水平線から地面側への混合率
    const float below = smoothstep(
        0.0f, 0.65f, -direction.y);
    // 頂点色または空の合成RGB
    float3 color = lerp(
        HorizonColor.rgb,
        TopColor.rgb,
        above);
    color = lerp(color, GroundColor.rgb, below);

    if (SunDiskColor.a > 0.0f)
    {
        // 視線と太陽方向のcos
        const float cosine = dot(direction, SunDirection.xyz);
        // 太陽円盤の角半径
        const float radius = max(SunDirection.w, 0.0001f);
        // 太陽円盤の内側の混合率
        const float disk = smoothstep(
            cos(radius * 1.05f),
            cos(radius * 0.95f),
            cosine);
        // 太陽周囲の光彩の強度
        const float glow = pow(
            saturate(
                (cosine - cos(radius * 30.0f))
                / max(1.0f - cos(radius * 30.0f), 0.0001f)),
            4.0f);
        // 太陽高度に応じた表示率
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
)";

    // 失敗HRESULTを操作名付きの例外へ変換します(result: 呼出し結果, operation: 失敗を報告する操作名)。
    void ThrowIfFailed(
        const HRESULT result,
        const char* const operation)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(
                std::string(operation)
                + " failed with HRESULT "
                + std::to_string(static_cast<unsigned long>(result)));
        }
    }

    // 埋め込みHLSLを最適化付きでコンパイルし、診断を例外にします(entryPoint: 入口関数名, target: シェーダー形式, source: HLSLソース文字列)。
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob> CompileSpriteShader(
        // 描画元のRGB・透過度の係数
        const char* const entryPoint,
        const char* const target,
        const std::string_view source = std::string_view{
            SpriteShaderSource,
            sizeof(SpriteShaderSource) - 1u })
    {
        // コンパイル済みシェーダー
        Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
        // コンパイラーの診断文字列
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        // コンパイル結果または交差矩形
        const HRESULT result = D3DCompile(
            source.data(),
            source.size(),
            "LamaPonD3D12Sprite",
            nullptr,
            nullptr,
            entryPoint,
            target,
            D3DCOMPILE_ENABLE_STRICTNESS
                | D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0,
            bytecode.GetAddressOf(),
            errors.GetAddressOf());
        if (FAILED(result))
        {
            // 例外へ渡すコンパイル診断
            std::string message =
                std::string("D3DCompile(") + entryPoint + ") failed";
            if (errors != nullptr && errors->GetBufferSize() > 0)
            {
                message += ": ";
                message.append(
                    static_cast<const char*>(errors->GetBufferPointer()),
                    errors->GetBufferSize());
            }
            throw std::runtime_error(message);
        }
        return bytecode;
    }

    // DirectXTK互換の合成係数をRGBと透過度に設定します(blend: 合成方式)。
    [[nodiscard]] D3D12_BLEND_DESC MakeBlendDescription(
        const LamaPon::SpriteBlendMode blend)
    {
        // 描画元のRGB・透過度の係数
        D3D12_BLEND source = D3D12_BLEND_ONE;
        // 描画先のRGB・透過度の係数
        D3D12_BLEND destination = D3D12_BLEND_ZERO;
        switch (blend)
        {
        case LamaPon::SpriteBlendMode::NonPremultiplied:
            source = D3D12_BLEND_SRC_ALPHA;
            destination = D3D12_BLEND_INV_SRC_ALPHA;
            break;
        case LamaPon::SpriteBlendMode::AlphaBlend:
            destination = D3D12_BLEND_INV_SRC_ALPHA;
            break;
        case LamaPon::SpriteBlendMode::Additive:
            source = D3D12_BLEND_SRC_ALPHA;
            destination = D3D12_BLEND_ONE;
            break;
        case LamaPon::SpriteBlendMode::Opaque:
            break;
        default:
            throw std::invalid_argument(
                "The sprite blend mode is invalid.");
        }

        // 描画先1枚の合成状態
        D3D12_RENDER_TARGET_BLEND_DESC target{};
        target.BlendEnable =
            source != D3D12_BLEND_ONE || destination != D3D12_BLEND_ZERO;
        target.LogicOpEnable = FALSE;
        target.SrcBlend = source;
        target.DestBlend = destination;
        target.BlendOp = D3D12_BLEND_OP_ADD;
        target.SrcBlendAlpha = source;
        target.DestBlendAlpha = destination;
        target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        // 作成する合成・面・深度の状態
        D3D12_BLEND_DESC description{};
        description.AlphaToCoverageEnable = FALSE;
        description.IndependentBlendEnable = FALSE;
        // 合成状態を設定する描画先
        for (auto& renderTarget : description.RenderTarget)
        {
            renderTarget = target;
        }
        return description;
    }

    // 通常は裏面を除去し、クリップ時は両面を描きます(scissored: クリップを使う指定)。
    [[nodiscard]] D3D12_RASTERIZER_DESC MakeRasterizerDescription(
        const bool scissored) noexcept
    {
        // 作成する合成・面・深度の状態
        D3D12_RASTERIZER_DESC description{};
        description.FillMode = D3D12_FILL_MODE_SOLID;
        description.CullMode = scissored
            ? D3D12_CULL_MODE_NONE
            : D3D12_CULL_MODE_BACK;
        description.FrontCounterClockwise = FALSE;
        description.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        description.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        description.SlopeScaledDepthBias =
            D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        description.DepthClipEnable = TRUE;
        description.MultisampleEnable = FALSE;
        description.AntialiasedLineEnable = FALSE;
        description.ForcedSampleCount = 0;
        description.ConservativeRaster =
            D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
        return description;
    }

    // 深度とステンシルを読まず書かない状態を作ります。
    [[nodiscard]] D3D12_DEPTH_STENCIL_DESC
        MakeDepthStencilDescription() noexcept
    {
        // ステンシル値を維持する設定
        const D3D12_DEPTH_STENCILOP_DESC keep{
            D3D12_STENCIL_OP_KEEP,
            D3D12_STENCIL_OP_KEEP,
            D3D12_STENCIL_OP_KEEP,
            D3D12_COMPARISON_FUNC_ALWAYS
        };
        // 作成する合成・面・深度の状態
        D3D12_DEPTH_STENCIL_DESC description{};
        description.DepthEnable = FALSE;
        description.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        description.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        description.StencilEnable = FALSE;
        description.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        description.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        description.FrontFace = keep;
        description.BackFace = keep;
        return description;
    }

    // XY成分がすべて有限か判定します(value: 検査する2成分)。
    [[nodiscard]] bool IsFinite(
        const DirectX::XMFLOAT2& value) noexcept
    {
        return std::isfinite(value.x)
            && std::isfinite(value.y);
    }

    // RGBA成分がすべて有限か判定します(value: 検査する4成分)。
    [[nodiscard]] bool IsFinite(
        const DirectX::XMFLOAT4& value) noexcept
    {
        return std::isfinite(value.x)
            && std::isfinite(value.y)
            && std::isfinite(value.z)
            && std::isfinite(value.w);
    }

    // クリップ矩形の全境界が有限か判定します(value: 検査する矩形)。
    [[nodiscard]] bool IsFinite(
        const LamaPon::SpriteClipRectangle& value) noexcept
    {
        return std::isfinite(value.minimumX)
            && std::isfinite(value.minimumY)
            && std::isfinite(value.maximumX)
            && std::isfinite(value.maximumY);
    }

    // 非負整数のクリップ矩形を作り、外側の範囲と交差させます(rectangle: 新しいクリップ指定, stack: 外側のクリップの積み重ね)。
    [[nodiscard]] D3D12_RECT MakeScissorRectangle(
        const LamaPon::SpriteClipRectangle& rectangle,
        const std::vector<D3D12_RECT>& stack)
    {
        // 非負LONGの範囲へ制限して整数に変換します(value: 画素境界の浮動小数値)。
        const auto clampLong = [](const float value) noexcept
        {
            return static_cast<LONG>(
                std::clamp(
                    static_cast<double>(value),
                    0.0,
                    static_cast<double>(
                        (std::numeric_limits<LONG>::max)())));
        };
        // コンパイル結果または交差矩形
        D3D12_RECT result{
            clampLong(rectangle.minimumX),
            clampLong(rectangle.minimumY),
            clampLong(rectangle.maximumX),
            clampLong(rectangle.maximumY) };
        if (!stack.empty())
        {
            // 交差させる外側のクリップ矩形
            const auto& outer = stack.back();
            result.left = std::max(result.left, outer.left);
            result.top = std::max(result.top, outer.top);
            result.right = std::min(result.right, outer.right);
            result.bottom = std::min(result.bottom, outer.bottom);
        }
        result.right = std::max(result.right, result.left);
        result.bottom = std::max(result.bottom, result.top);
        return result;
    }

    // 対象の幅と高さの逆数をUV単位で返します(target: 寸法の取得元)。
    [[nodiscard]] std::array<float, 2> TexelSize(
        const LamaPon::RenderTarget& target) noexcept
    {
        return {
            1.0f / static_cast<float>(std::max(target.Width(), 1u)),
            1.0f / static_cast<float>(std::max(target.Height(), 1u))
        };
    }
}

namespace LamaPon::Detail
{
    D3D12SpriteRenderer::D3D12SpriteRenderer(D3D12Backend& backend)
        : m_backend(&backend)
    {
        if (!backend.IsInitialized())
        {
            throw std::invalid_argument(
                "The DirectX 12 sprite renderer requires an initialized "
                "backend.");
        }
        // 初期化済みバックエンドのデバイス
        auto* const device = backend.Device();

        m_vertexShader = CompileSpriteShader(
            "SpriteVertexShader",
            "vs_5_0");
        m_pixelShader = CompileSpriteShader(
            "SpritePixelShader",
            "ps_5_0");
        m_toneMapPixelShader = CompileSpriteShader(
            "ToneMappedPixelShader",
            "ps_5_0");
        m_bloomPixelShader = CompileSpriteShader(
            "BloomPixelShader",
            "ps_5_0");
        m_fxaaPixelShader = CompileSpriteShader(
            "FxaaPixelShader",
            "ps_5_0");
        m_luminancePixelShader = CompileSpriteShader(
            "LuminancePixelShader",
            "ps_5_0");
        m_temporalPixelShader = CompileSpriteShader(
            "TemporalPixelShader",
            "ps_5_0");
        m_screenOutlinePixelShader = CompileSpriteShader(
            "ScreenOutlinePixelShader",
            "ps_5_0");
        m_motionBlurPixelShader = CompileSpriteShader(
            "MotionBlurPixelShader",
            "ps_5_0");
        m_depthOfFieldPixelShader = CompileSpriteShader(
            "DepthOfFieldPixelShader",
            "ps_5_0");
        m_ambientOcclusionPixelShader = CompileSpriteShader(
            "AmbientOcclusionPixelShader",
            "ps_5_0");
        m_ambientOcclusionBlurPixelShader = CompileSpriteShader(
            "AmbientOcclusionBlurPixelShader",
            "ps_5_0");
        m_volumetricLightPixelShader = CompileSpriteShader(
            "VolumetricLightPixelShader",
            "ps_5_0");
        m_lensFlareStreakPixelShader = CompileSpriteShader(
            "LensFlareStreakPixelShader",
            "ps_5_0");
        m_lensFlareCompositePixelShader = CompileSpriteShader(
            "LensFlareCompositePixelShader",
            "ps_5_0");
        m_reflectionDepthLinearizePixelShader = CompileSpriteShader(
            "ReflectionDepthLinearizePixelShader",
            "ps_5_0");
        m_reflectionDepthDownsamplePixelShader = CompileSpriteShader(
            "ReflectionDepthDownsamplePixelShader",
            "ps_5_0");
        m_skyPixelShader = CompileSpriteShader(
            "SkyPixelShader",
            "ps_5_0",
            std::string_view{
                SkyShaderSource,
                sizeof(SkyShaderSource) - 1u });

        // 主画像t0のSRV範囲
        D3D12_DESCRIPTOR_RANGE textureRange{};
        textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        textureRange.NumDescriptors = 1;
        textureRange.BaseShaderRegister = 0;
        textureRange.RegisterSpace = 0;
        textureRange.OffsetInDescriptorsFromTableStart = 0;

        // 履歴・空画像t1のSRV範囲
        D3D12_DESCRIPTOR_RANGE historyRange = textureRange;
        historyRange.BaseShaderRegister = 1;
        // 深度画像t2のSRV範囲
        D3D12_DESCRIPTOR_RANGE depthRange = textureRange;
        depthRange.BaseShaderRegister = 2;
        // 影配列t3のSRV範囲
        D3D12_DESCRIPTOR_RANGE shadowRange = textureRange;
        shadowRange.BaseShaderRegister = 3;

        // 標準・全画面の9ルート引数
        std::array<D3D12_ROOT_PARAMETER, 9> parameters{};
        parameters[0].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants.ShaderRegister = 0;
        parameters[0].Constants.RegisterSpace = 0;
        parameters[0].Constants.Num32BitValues = 2;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        parameters[1].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable.NumDescriptorRanges = 1;
        parameters[1].DescriptorTable.pDescriptorRanges = &textureRange;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[2].Descriptor.ShaderRegister = 1;
        parameters[2].Descriptor.RegisterSpace = 0;
        parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[3].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[3].DescriptorTable.NumDescriptorRanges = 1;
        parameters[3].DescriptorTable.pDescriptorRanges = &historyRange;
        parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[4].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[4].DescriptorTable.NumDescriptorRanges = 1;
        parameters[4].DescriptorTable.pDescriptorRanges = &depthRange;
        parameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[5].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[5].Constants.ShaderRegister = 2;
        parameters[5].Constants.RegisterSpace = 0;
        parameters[5].Constants.Num32BitValues = 32;
        parameters[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[6].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[6].DescriptorTable.NumDescriptorRanges = 1;
        parameters[6].DescriptorTable.pDescriptorRanges = &shadowRange;
        parameters[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[7].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[7].Descriptor.ShaderRegister = 3;
        parameters[7].Descriptor.RegisterSpace = 0;
        parameters[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[8].Descriptor.ShaderRegister = 0;
        parameters[8].Descriptor.RegisterSpace = 0;
        parameters[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // 素材と影比較の2サンプラー
        std::array<D3D12_STATIC_SAMPLER_DESC, 2> samplers{};
        // s0の線形端固定サンプラー
        auto& sampler = samplers[0];
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MipLODBias = 0.0f;
        sampler.MaxAnisotropy = D3D12_MAX_MAXANISOTROPY;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        sampler.MinLOD = 0.0f;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.RegisterSpace = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        // s1の白境界の深度比較
        auto& shadowSampler = samplers[1];
        shadowSampler.Filter =
            D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        shadowSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        shadowSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        shadowSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        shadowSampler.MipLODBias = 0.0f;
        shadowSampler.MaxAnisotropy = D3D12_MAX_MAXANISOTROPY;
        shadowSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        shadowSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        shadowSampler.MinLOD = 0.0f;
        shadowSampler.MaxLOD = 0.0f;
        shadowSampler.ShaderRegister = 1;
        shadowSampler.RegisterSpace = 0;
        shadowSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // 標準描画のルート署名の定義
        D3D12_ROOT_SIGNATURE_DESC rootDescription{};
        rootDescription.NumParameters =
            static_cast<UINT>(parameters.size());
        rootDescription.pParameters = parameters.data();
        rootDescription.NumStaticSamplers =
            static_cast<UINT>(samplers.size());
        rootDescription.pStaticSamplers = samplers.data();
        rootDescription.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        // 標準描画の直列化ルート署名
        Microsoft::WRL::ComPtr<ID3DBlob> serializedRoot;
        // 標準署名の直列化診断
        Microsoft::WRL::ComPtr<ID3DBlob> rootErrors;
        // 標準ルート署名の直列化結果
        const HRESULT serialized = D3D12SerializeRootSignature(
            &rootDescription,
            D3D_ROOT_SIGNATURE_VERSION_1,
            serializedRoot.GetAddressOf(),
            rootErrors.GetAddressOf());
        if (FAILED(serialized))
        {
            // 例外へ渡す署名直列化診断
            std::string message =
                "D3D12SerializeRootSignature(sprite) failed";
            if (rootErrors != nullptr && rootErrors->GetBufferSize() > 0)
            {
                message += ": ";
                message.append(
                    static_cast<const char*>(
                        rootErrors->GetBufferPointer()),
                    rootErrors->GetBufferSize());
            }
            throw std::runtime_error(message);
        }
        ThrowIfFailed(
            device->CreateRootSignature(
                0,
                serializedRoot->GetBufferPointer(),
                serializedRoot->GetBufferSize(),
                IID_PPV_ARGS(m_rootSignature.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateRootSignature(sprite)");

        // 画面効果t0～t3のSRV範囲
        std::array<D3D12_DESCRIPTOR_RANGE, 4> screenTextureRanges{};
        // 画面効果のルート引数番号
        for (UINT index{}; index < screenTextureRanges.size(); ++index)
        {
            // 設定する画面効果SRV範囲
            auto& range = screenTextureRanges[index];
            range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            range.NumDescriptors = 1;
            range.BaseShaderRegister = index;
        }
        // 画面効果の5ルート引数
        std::array<D3D12_ROOT_PARAMETER, 5> screenParameters{};
        screenParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        screenParameters[0].Descriptor.ShaderRegister = 0;
        screenParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        // 画面効果のルート引数番号
        for (std::size_t index{}; index < screenTextureRanges.size(); ++index)
        {
            // 設定する画面効果ルート引数
            auto& parameter = screenParameters[index + 1u];
            parameter.ParameterType =
                D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameter.DescriptorTable.NumDescriptorRanges = 1;
            parameter.DescriptorTable.pDescriptorRanges =
                &screenTextureRanges[index];
            parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }
        // 画面効果のs0線形繰返し
        D3D12_STATIC_SAMPLER_DESC screenSampler{};
        screenSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        screenSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        screenSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        screenSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        screenSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        screenSampler.MaxLOD = D3D12_FLOAT32_MAX;
        screenSampler.ShaderRegister = 0;
        screenSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        // 画面効果のルート署名の定義
        D3D12_ROOT_SIGNATURE_DESC screenRootDescription{};
        screenRootDescription.NumParameters =
            static_cast<UINT>(screenParameters.size());
        screenRootDescription.pParameters = screenParameters.data();
        screenRootDescription.NumStaticSamplers = 1;
        screenRootDescription.pStaticSamplers = &screenSampler;
        screenRootDescription.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        // 画面効果の直列化ルート署名
        Microsoft::WRL::ComPtr<ID3DBlob> serializedScreenRoot;
        // 画面効果署名の直列化診断
        Microsoft::WRL::ComPtr<ID3DBlob> screenRootErrors;
        // 画面効果署名の直列化結果
        const HRESULT serializedScreen = D3D12SerializeRootSignature(
            &screenRootDescription,
            D3D_ROOT_SIGNATURE_VERSION_1,
            serializedScreenRoot.GetAddressOf(),
            screenRootErrors.GetAddressOf());
        if (FAILED(serializedScreen))
        {
            // 例外へ渡す署名直列化診断
            std::string message =
                "D3D12SerializeRootSignature(screen effect) failed";
            if (screenRootErrors != nullptr
                && screenRootErrors->GetBufferSize() > 0)
            {
                message += ": ";
                message.append(
                    static_cast<const char*>(
                        screenRootErrors->GetBufferPointer()),
                    screenRootErrors->GetBufferSize());
            }
            throw std::runtime_error(message);
        }
        ThrowIfFailed(
            device->CreateRootSignature(
                0,
                serializedScreenRoot->GetBufferPointer(),
                serializedScreenRoot->GetBufferSize(),
                IID_PPV_ARGS(
                    m_screenEffectRootSignature.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateRootSignature(screen effect)");

        // 2048枚共通索引のバイト数
        const auto indexBytes =
            MaximumSpritesPerDraw * IndicesPerSprite
            * sizeof(std::uint16_t);
        // CPU書込み可能な索引用ヒープ
        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        uploadHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        uploadHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        uploadHeap.CreationNodeMask = 1;
        uploadHeap.VisibleNodeMask = 1;
        // 共通索引バッファの作成情報
        D3D12_RESOURCE_DESC indexDescription{};
        indexDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        indexDescription.Width = indexBytes;
        indexDescription.Height = 1;
        indexDescription.DepthOrArraySize = 1;
        indexDescription.MipLevels = 1;
        indexDescription.Format = DXGI_FORMAT_UNKNOWN;
        indexDescription.SampleDesc.Count = 1;
        indexDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(
            device->CreateCommittedResource(
                &uploadHeap,
                D3D12_HEAP_FLAG_NONE,
                &indexDescription,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(m_indexBuffer.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateCommittedResource(sprite indices)");
        // 索引バッファのCPU書込み先
        void* mapped{};
        // CPUから読まないMap範囲
        const D3D12_RANGE noRead{};
        ThrowIfFailed(
            m_indexBuffer->Map(0, &noRead, &mapped),
            "ID3D12Resource::Map(sprite indices)");
        // 16ビットの共通頂点索引列
        auto* const indices = static_cast<std::uint16_t*>(mapped);
        // 索引を生成する矩形番号
        for (std::size_t sprite{}; sprite < MaximumSpritesPerDraw; ++sprite)
        {
            // 矩形の先頭頂点番号
            const auto first =
                static_cast<std::uint16_t>(sprite * VerticesPerSprite);
            // 矩形1枚分の索引書込み先
            auto* const quad = indices + sprite * IndicesPerSprite;
            quad[0] = first;
            quad[1] = static_cast<std::uint16_t>(first + 1u);
            quad[2] = static_cast<std::uint16_t>(first + 2u);
            quad[3] = static_cast<std::uint16_t>(first + 1u);
            quad[4] = static_cast<std::uint16_t>(first + 3u);
            quad[5] = static_cast<std::uint16_t>(first + 2u);
        }
        m_indexBuffer->Unmap(0, nullptr);
    }

    D3D12SpriteRenderer::~D3D12SpriteRenderer() noexcept = default;

    std::uint64_t D3D12SpriteRenderer::Begin(
        const SpritePassDescription& description,
        const GraphicsViewHandle& fallbackTexture,
        AssetManager* const assets,
        SpriteShaderStatus& status)
    {
        if (m_failed)
        {
            throw std::logic_error(
                "The DirectX 12 sprite renderer failed and must be "
                "recovered by reinitializing the graphics device.");
        }
        if (m_activeToken != 0)
        {
            throw std::logic_error(
                "A sprite render pass is already active.");
        }
        if (description.blend > SpriteBlendMode::Opaque)
        {
            throw std::invalid_argument(
                "The sprite blend mode is invalid.");
        }

        // 開始時のカスタムPS準備結果
        SpriteShaderStatus preparedStatus;
        m_activeCustomShader = nullptr;
        if (!description.pixelShader.empty())
        {
            try
            {
                if (assets == nullptr)
                {
                    throw std::logic_error(
                        "A custom DirectX 12 sprite shader requires an "
                        "asset manager.");
                }
                // 正規化したカスタムPSのパス
                const auto shaderPath = assets->ResolvePath(
                    description.pixelShader).lexically_normal();
                if (!assets->FileExists(shaderPath))
                {
                    throw std::runtime_error(
                        "Sprite shader file was not found: "
                        + PathToUtf8(shaderPath));
                }
                // 今回取得したコンパイル済みPS
                auto byteCode = CompileShaderCached(
                    *assets,
                    shaderPath,
                    "PSMain",
                    "ps_5_0");
                // パス別のカスタムPSキャッシュ
                auto& entry = m_customShaders[shaderPath];
                // PSバイトコードが変わった状態
                const bool changed = entry.pixelShader == nullptr
                    || byteCode->GetBufferSize()
                        != entry.pixelShader->GetBufferSize()
                    || std::memcmp(
                        byteCode->GetBufferPointer(),
                        entry.pixelShader->GetBufferPointer(),
                        byteCode->GetBufferSize()) != 0;
                if (changed)
                {
                    entry.pixelShader = std::move(byteCode);
                    // 変更されたPSに依存するPSO
                    for (auto& pipeline : entry.pipelineStates)
                    {
                        pipeline.Reset();
                    }
                    entry.generation = m_nextCustomShaderGeneration++;
                    if (m_nextCustomShaderGeneration == 0)
                    {
                        m_nextCustomShaderGeneration = 1;
                    }
                }
                m_customParameters = description.customParameters;
                m_spriteLighting = description.lighting;
                m_activeCustomShader = &entry;

                // PSO作成もBegin中に検証し、ShaderStatusが成功を返した後にEndで初めて失敗する状態を作らないようにします。
                static_cast<void>(PipelineState(
                    description.blend,
                    false,
                    m_backend->ActiveColorFormat(),
                    m_backend->ActiveDepthFormat(),
                    FullscreenProgram::None));
                preparedStatus.generation = entry.generation;
            }
            // exception: カスタムPSの読込み・PSO作成失敗。
            catch (const std::exception& exception)
            {
                m_activeCustomShader = nullptr;
                preparedStatus.fallback =
                    SpriteShaderFallback::DefaultPipeline;
                preparedStatus.error = exception.what();
            }
        }
        // 今回のパスの非ゼロ識別番号
        auto token = m_nextToken++;
        if (token == 0)
        {
            token = m_nextToken++;
        }

        status = std::move(preparedStatus);
        m_blend = description.blend;
        m_program = FullscreenProgram::None;
        m_fallbackTexture = fallbackTexture;
        m_sprites.clear();
        m_scissorStack.clear();
        m_activeToken = token;
        return token;
    }

    bool D3D12SpriteRenderer::Draw(
        const std::uint64_t token,
        const SpriteDrawRequest& request)
    {
        RequireOwner(token);
        if (!IsFinite(request.position)
            || !IsFinite(request.tint)
            || !std::isfinite(request.rotation)
            || !IsFinite(request.origin)
            || !IsFinite(request.scale)
            || !std::isfinite(request.layerDepth))
        {
            return false;
        }
        if (request.hasSourceRectangle
            && (request.sourceRectangle.right
                    <= request.sourceRectangle.left
                || request.sourceRectangle.bottom
                    <= request.sourceRectangle.top))
        {
            return false;
        }
        if (request.flip > SpriteFlip::Both)
        {
            return false;
        }

        // 指定画像または未指定時の代替
        const auto& view = request.texture
            ? request.texture
            : m_fallbackTexture;
        // 描画画像のGPU参照
        const auto binding = m_backend->TryResolveShaderResource(view);
        if (!binding)
        {
            return false;
        }

        // 描画画像の幅の画素数
        const float textureWidth = static_cast<float>(binding->width);
        // 描画画像の高さの画素数
        const float textureHeight = static_cast<float>(binding->height);
        // 描画画像の幅の逆数
        const float inverseTextureWidth = 1.0f / textureWidth;
        // 描画画像の高さの逆数
        const float inverseTextureHeight = 1.0f / textureHeight;
        // 入力領域の左端UV
        float sourceX = 0.0f;
        // 入力領域の上端UV
        float sourceY = 0.0f;
        // 入力領域のUV幅
        float sourceWidth = 1.0f;
        // 入力領域のUV高さ
        float sourceHeight = 1.0f;
        // 入力幅で正規化した横原点
        float originX{};
        // 入力高さで正規化した縦原点
        float originY{};
        // 倍率を適用した描画幅
        float destinationWidth{};
        // 倍率を適用した描画高さ
        float destinationHeight{};
        if (request.hasSourceRectangle)
        {
            // 入力領域の左端画素
            const auto left =
                static_cast<float>(request.sourceRectangle.left);
            // 入力領域の上端画素
            const auto top =
                static_cast<float>(request.sourceRectangle.top);
            // 入力領域の幅の画素数
            const float texelWidth =
                static_cast<float>(request.sourceRectangle.right) - left;
            // 入力領域の高さの画素数
            const float texelHeight =
                static_cast<float>(request.sourceRectangle.bottom) - top;
            destinationWidth = request.scale.x * texelWidth;
            destinationHeight = request.scale.y * texelHeight;
            originX = request.origin.x / texelWidth;
            originY = request.origin.y / texelHeight;
            sourceX = left * inverseTextureWidth;
            sourceY = top * inverseTextureHeight;
            sourceWidth = texelWidth * inverseTextureWidth;
            sourceHeight = texelHeight * inverseTextureHeight;
        }
        else
        {
            destinationWidth = request.scale.x * textureWidth;
            destinationHeight = request.scale.y * textureHeight;
            originX = request.origin.x * inverseTextureWidth;
            originY = request.origin.y * inverseTextureHeight;
        }

        // 描画回転角のsin
        float rotationSin = 0.0f;
        // 描画回転角のcos
        float rotationCos = 1.0f;
        // 回転変換を使う状態
        const bool rotated = request.rotation != 0.0f;
        if (rotated)
        {
            DirectX::XMScalarSinCos(
                &rotationSin,
                &rotationCos,
                request.rotation);
        }

        // 左上・右上・左下・右下の隅座標
        static constexpr std::array<DirectX::XMFLOAT2, VerticesPerSprite>
            CornerOffsets{ {
                { 0.0f, 0.0f },
                { 1.0f, 0.0f },
                { 0.0f, 1.0f },
                { 1.0f, 1.0f }
            } };
        // UV隅を交換する反転ビット
        const auto mirrorBits =
            static_cast<std::size_t>(request.flip) & 3u;

        // 命令記録まで画像を保持する矩形
        QueuedSprite sprite;
        sprite.texture = binding->descriptor;
        sprite.view = view;
        // 矩形の隅の番号
        for (std::size_t corner{}; corner < VerticesPerSprite; ++corner)
        {
            // 原点基準の隅の横座標
            const float cornerX =
                (CornerOffsets[corner].x - originX) * destinationWidth;
            // 原点基準の隅の縦座標
            const float cornerY =
                (CornerOffsets[corner].y - originY) * destinationHeight;
            // 設定する矩形の頂点
            auto& vertex = sprite.vertices[corner];
            if (rotated)
            {
                vertex.position.x =
                    (cornerX * rotationCos + cornerY * -rotationSin)
                    + request.position.x;
                vertex.position.y =
                    (cornerX * rotationSin + cornerY * rotationCos)
                    + request.position.y;
            }
            else
            {
                vertex.position.x = cornerX + request.position.x;
                vertex.position.y = cornerY + request.position.y;
            }
            vertex.position.z = request.layerDepth;
            vertex.color = request.tint;
            // 反転を適用したUV側の隅
            const auto& textureCorner = CornerOffsets[corner ^ mirrorBits];
            vertex.textureCoordinate = {
                textureCorner.x * sourceWidth + sourceX,
                textureCorner.y * sourceHeight + sourceY
            };
        }
        m_sprites.push_back(std::move(sprite));
        return true;
    }

    bool D3D12SpriteRenderer::PushScissor(
        const std::uint64_t token,
        const SpriteClipRectangle& rectangle)
    {
        RequireOwner(token);
        if (!IsFinite(rectangle))
        {
            return false;
        }
        // 送信成功後に置換するクリップ列
        auto nextScissors = m_scissorStack;
        nextScissors.push_back(
            MakeScissorRectangle(rectangle, nextScissors));
        // 予約矩形を現在のクリップで送信できた場合だけ、新しいクリップへ切り替えます。
        FlushOrFail();
        m_scissorStack.swap(nextScissors);
        return true;
    }

    bool D3D12SpriteRenderer::PopScissor(const std::uint64_t token)
    {
        RequireOwner(token);
        if (m_scissorStack.empty())
        {
            return false;
        }
        FlushOrFail();
        m_scissorStack.pop_back();
        return true;
    }

    void D3D12SpriteRenderer::End(const std::uint64_t token)
    {
        RequireOwner(token);
        FlushOrFail();
        ClearPass();
    }

    void D3D12SpriteRenderer::Abort(const std::uint64_t token) noexcept
    {
        if (token == 0 || token != m_activeToken)
        {
            return;
        }
        try
        {
            Flush();
        }
        catch (...)
        {
            m_failed = true;
        }
        ClearPass();
    }

    void D3D12SpriteRenderer::CompositeScene(
        const GraphicsViewHandle& texture,
        const GraphicsViewHandle& fallbackTexture)
    {
        DrawFullscreen(
            texture,
            fallbackTexture,
            FullscreenProgram::None,
            {});
    }

    void D3D12SpriteRenderer::ApplyToneMapping(
        RenderTarget& target,
        const GraphicsViewHandle& fallbackTexture,
        const ColorGradingSettings& colorGrading)
    {
        // D3D11は無効時に単純copyして交換するだけで、内容は変わりません。
        if (!colorGrading.toneMappingEnabled)
        {
            return;
        }
        ApplyPostProcessPass(
            target,
            fallbackTexture,
            FullscreenProgram::ToneMap,
            {
                std::clamp(colorGrading.exposure, -8.0f, 8.0f),
                std::clamp(colorGrading.contrast, 0.0f, 4.0f),
                std::clamp(colorGrading.saturation, 0.0f, 4.0f),
                std::clamp(colorGrading.temperature, -2.0f, 2.0f),
                std::clamp(colorGrading.tint, -2.0f, 2.0f),
                std::clamp(colorGrading.vignette, 0.0f, 1.0f),
                colorGrading.enabled ? 1.0f : 0.0f,
                std::clamp(colorGrading.autoExposureStops, -16.0f, 16.0f)
            });
    }

    void D3D12SpriteRenderer::ApplyBloom(
        RenderTarget& target,
        const GraphicsViewHandle& fallbackTexture,
        const BloomSettings& settings)
    {
        if (!settings.enabled)
        {
            return;
        }
        // 描画先1画素のUV幅
        const auto texel = TexelSize(target);
        ApplyPostProcessPass(
            target,
            fallbackTexture,
            FullscreenProgram::Bloom,
            {
                texel[0],
                texel[1],
                std::clamp(settings.threshold, 0.0f, 4.0f),
                std::clamp(settings.intensity, 0.0f, 8.0f),
                std::clamp(settings.radius, 0.25f, 12.0f),
                0.0f,
                0.0f,
                0.0f
            });
    }

    void D3D12SpriteRenderer::ApplyScreenSpaceLensFlare(
        RenderTarget& target,
        const GraphicsViewHandle& fallbackTexture,
        const ScreenSpaceLensFlareSettings& settings)
    {
        if (!settings.enabled)
        {
            return;
        }
        // 描画先1画素のUV幅
        const auto texel = TexelSize(target);
        // 制限済みの光条の最長UV距離
        const float longest = std::clamp(settings.streakLength, 0.0f, 1.0f);
        // 光条第1段の採取間隔
        const float baseStride = longest / 42.0f;
        // 光条と合成で共通の12定数
        const std::array<float, 12> shared{
            texel[0],
            texel[1],
            std::clamp(settings.threshold, 0.0f, 16.0f),
            std::clamp(settings.intensity, 0.0f, 8.0f),
            std::clamp(settings.ghostDispersal, 0.01f, 2.0f),
            std::clamp(settings.haloWidth, 0.05f, 1.5f),
            std::clamp(settings.chromaticAberration, 0.0f, 1.0f),
            std::clamp(settings.streakIntensity, 0.0f, 4.0f),
            longest,
            0.0f,
            0.0f,
            0.0f
        };
        try
        {
            // 光条を4倍ずつ広げる段番号
            for (std::uint32_t pass{}; pass < 3u; ++pass)
            {
                // 処理前の主入力画像
                const auto source =
                    m_backend->BeginOffscreenLensFlareStreakPass(
                        target,
                        pass);
                // 効果別の16個のb1定数
                std::array<float, 16> constants{};
                std::copy(shared.begin(), shared.end(), constants.begin());
                constants[12] = baseStride
                    * std::pow(4.0f, static_cast<float>(pass));
                constants[13] = static_cast<float>(
                    std::clamp(settings.streakDirections, 1u, 4u));
                constants[14] = settings.streakAngleDegrees
                    * 3.14159265358979323846f / 180.0f;
                constants[15] = pass == 0u ? 1.0f : 0.0f;
                DrawFullscreen(
                    source,
                    fallbackTexture,
                    FullscreenProgram::LensFlareStreak,
                    constants);
            }
        }
        catch (...)
        {
            m_backend->AbortOffscreenLensFlareStreaks(target);
            throw;
        }
        // 3段の伸長で作成した光条画像
        const auto streak =
            m_backend->EndOffscreenLensFlareStreaks(target);
        // 処理前の主入力画像
        const auto source = m_backend->BeginOffscreenPostProcess(target);
        try
        {
            // 効果別の16個のb1定数
            std::array<float, 16> constants{};
            std::copy(shared.begin(), shared.end(), constants.begin());
            DrawFullscreen(
                source,
                fallbackTexture,
                FullscreenProgram::LensFlareComposite,
                constants,
                { streak, GraphicsViewHandle{} });
        }
        catch (...)
        {
            m_backend->AbortOffscreenPostProcess(target);
            throw;
        }
        m_backend->EndOffscreenPostProcess(target);
    }

    void D3D12SpriteRenderer::ApplyFXAA(
        RenderTarget& target,
        const GraphicsViewHandle& fallbackTexture)
    {
        // 描画先1画素のUV幅
        const auto texel = TexelSize(target);
        ApplyPostProcessPass(
            target,
            fallbackTexture,
            FullscreenProgram::Fxaa,
            { texel[0], texel[1], 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f });
    }

    void D3D12SpriteRenderer::ApplyTemporalAntiAliasing(
        RenderTarget& target,
        const GraphicsViewHandle& fallbackTexture,
        const TemporalAntiAliasingSettings& settings,
        const TemporalAntiAliasingInputs& inputs)
    {
        if (!settings.enabled)
        {
            return;
        }
        // 描画先の履歴とGPU資源状態
        const auto* const state =
            RenderTargetBackendAccess::Get(target);
        // TAA用の前フレーム画像
        const auto history = target.TemporalHistoryViewHandle();
        // 深度効果の入力デバイス深度
        const auto depth = target.DepthViewHandle();
        if (state == nullptr
            || !state->m_temporalHistoryValid
            || !m_backend->TryResolveShaderResource(history)
            || !m_backend->TryResolveShaderResource(depth))
        {
            return;
        }

        // 現在の逆射影と履歴射影の32値
        std::array<float, 32> matrices{};
        static_assert(
            sizeof(inputs.inverseViewProjection) == sizeof(float) * 16u);
        static_assert(
            sizeof(state->m_temporalHistoryViewProjection)
                == sizeof(float) * 16u);
        std::memcpy(
            matrices.data(),
            &inputs.inverseViewProjection,
            sizeof(inputs.inverseViewProjection));
        std::memcpy(
            matrices.data() + 16u,
            &state->m_temporalHistoryViewProjection,
            sizeof(state->m_temporalHistoryViewProjection));
        // 描画先1画素のUV幅
        const auto texel = TexelSize(target);
        // 処理前の主入力画像
        const auto source = m_backend->BeginOffscreenPostProcess(target);
        try
        {
            DrawFullscreen(
                source,
                fallbackTexture,
                FullscreenProgram::Temporal,
                {
                    std::clamp(settings.historyWeight, 0.0f, 0.98f),
                    std::max(settings.clampTolerance, 0.0f),
                    texel[0],
                    texel[1],
                    0.0f,
                    0.0f,
                    0.0f,
                    0.0f
                },
                { history, depth },
                matrices);
        }
        catch (...)
        {
            m_backend->AbortOffscreenPostProcess(target);
            throw;
        }
        m_backend->EndOffscreenPostProcess(target);
    }

    void D3D12SpriteRenderer::ApplyVolumetricLight(
        RenderTarget& target,
        const GraphicsViewHandle& fallbackTexture,
        const VolumetricLightSettings& settings,
        const VolumetricLightInputs& inputs)
    {
        // 深度効果の入力デバイス深度
        const auto depth = target.DepthViewHandle();
        if (!settings.enabled
            || settings.intensity <= 0.0f
            || inputs.cascadeCount == 0u
            || inputs.cascadeCount > 4u
            || !std::isfinite(inputs.shadowResolution)
            || inputs.shadowResolution < 1.0f
            || inputs.shadowResolution
                > static_cast<float>(
                    D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
            || !m_backend->TryResolveShaderResource(depth)
            || !m_backend->TryResolveShaderResource(inputs.cascadeShadow))
        {
            return;
        }

        m_volumetricConstants = {};
        m_volumetricConstants.inverseViewProjection =
            inputs.inverseViewProjection;
        m_volumetricConstants.cameraPosition = {
            inputs.cameraPosition.x,
            inputs.cameraPosition.y,
            inputs.cameraPosition.z,
            std::max(settings.maximumDistance, 0.1f) };
        m_volumetricConstants.lightDirection = {
            inputs.lightDirection.x,
            inputs.lightDirection.y,
            inputs.lightDirection.z,
            static_cast<float>(
                std::clamp(settings.sampleCount, 1u, 128u)) };
        m_volumetricConstants.lightColor = {
            inputs.lightColor.x * settings.intensity,
            inputs.lightColor.y * settings.intensity,
            inputs.lightColor.z * settings.intensity,
            std::clamp(settings.scattering, 0.0f, 0.95f) };
        m_volumetricConstants.cascades = inputs.cascadeViewProjections;
        m_volumetricConstants.shadowParameters = {
            static_cast<float>(inputs.cascadeCount),
            inputs.shadowBias,
            1.0f / std::max(inputs.shadowResolution, 1.0f),
            0.0f };

        // 処理前の主入力画像
        const auto source = m_backend->BeginOffscreenPostProcess(target);
        try
        {
            DrawFullscreen(
                source,
                fallbackTexture,
                FullscreenProgram::VolumetricLight,
                {},
                { GraphicsViewHandle{}, depth, inputs.cascadeShadow });
        }
        catch (...)
        {
            m_backend->AbortOffscreenPostProcess(target);
            throw;
        }
        m_backend->EndOffscreenPostProcess(target);
    }

    void D3D12SpriteRenderer::ApplyScreenOutline(
        RenderTarget& target,
        const GraphicsViewHandle& fallbackTexture,
        const ScreenOutlineSettings& settings,
        const DirectX::XMFLOAT4X4& projection)
    {
        if (!settings.enabled
            || std::abs(projection._11) < 1e-6f
            || std::abs(projection._22) < 1e-6f)
        {
            return;
        }
        // 深度効果の入力デバイス深度
        const auto depth = target.DepthViewHandle();
        if (!m_backend->TryResolveShaderResource(depth))
        {
            return;
        }
        // 描画先1画素のUV幅
        const auto texel = TexelSize(target);
        // 処理前の主入力画像
        const auto source = m_backend->BeginOffscreenPostProcess(target);
        try
        {
            DrawFullscreen(
                source,
                fallbackTexture,
                FullscreenProgram::ScreenOutline,
                {
                    std::clamp(settings.color.x, 0.0f, 1.0f),
                    std::clamp(settings.color.y, 0.0f, 1.0f),
                    std::clamp(settings.color.z, 0.0f, 1.0f),
                    std::clamp(settings.intensity, 0.0f, 1.0f),
                    std::clamp(settings.thickness, 1.0f, 4.0f),
                    std::clamp(settings.depthThreshold, 0.0001f, 1.0f),
                    std::clamp(settings.normalThreshold, 0.0f, 1.0f),
                    0.0f,
                    projection._33,
                    projection._43,
                    1.0f / projection._11,
                    1.0f / projection._22,
                    texel[0],
                    texel[1],
                    static_cast<float>(target.Width()),
                    static_cast<float>(target.Height())
                },
                { GraphicsViewHandle{}, depth });
        }
        catch (...)
        {
            m_backend->AbortOffscreenPostProcess(target);
            throw;
        }
        m_backend->EndOffscreenPostProcess(target);
    }

    void D3D12SpriteRenderer::ApplyMotionBlur(
        RenderTarget& target,
        const GraphicsViewHandle& fallbackTexture,
        const MotionBlurSettings& settings,
        const DirectX::XMFLOAT4X4& inverseViewProjection,
        const DirectX::XMFLOAT4X4& previousViewProjection,
        const std::uint32_t sampleCount)
    {
        if (!settings.enabled
            || settings.intensity <= 0.0f
            || settings.maximumRadius <= 0.0f)
        {
            return;
        }
        // 深度効果の入力デバイス深度
        const auto depth = target.DepthViewHandle();
        if (!m_backend->TryResolveShaderResource(depth))
        {
            return;
        }
        // 現在の逆射影と履歴射影の32値
        std::array<float, 32> matrices{};
        static_assert(
            sizeof(inverseViewProjection) == sizeof(float) * 16u);
        static_assert(
            sizeof(previousViewProjection) == sizeof(float) * 16u);
        std::memcpy(
            matrices.data(),
            &inverseViewProjection,
            sizeof(inverseViewProjection));
        std::memcpy(
            matrices.data() + 16u,
            &previousViewProjection,
            sizeof(previousViewProjection));
        // 描画先1画素のUV幅
        const auto texel = TexelSize(target);
        // 処理前の主入力画像
        const auto source = m_backend->BeginOffscreenPostProcess(target);
        try
        {
            DrawFullscreen(
                source,
                fallbackTexture,
                FullscreenProgram::MotionBlur,
                {
                    std::clamp(settings.intensity, 0.0f, 4.0f),
                    std::clamp(settings.maximumRadius, 0.0f, 64.0f),
                    static_cast<float>(std::clamp(sampleCount, 2u, 32u)),
                    texel[0],
                    texel[1],
                    0.0f,
                    0.0f,
                    0.0f
                },
                { GraphicsViewHandle{}, depth },
                matrices);
        }
        catch (...)
        {
            m_backend->AbortOffscreenPostProcess(target);
            throw;
        }
        m_backend->EndOffscreenPostProcess(target);
    }

    void D3D12SpriteRenderer::ApplyDepthOfField(
        RenderTarget& target,
        const GraphicsViewHandle& fallbackTexture,
        const DepthOfFieldSettings& settings,
        const DirectX::XMFLOAT4X4& projection,
        const std::uint32_t sampleCount)
    {
        if (!settings.enabled
            || settings.maximumRadius <= 0.0f
            || std::abs(projection._11) < 1e-6f
            || std::abs(projection._22) < 1e-6f)
        {
            return;
        }
        // 深度効果の入力デバイス深度
        const auto depth = target.DepthViewHandle();
        if (!m_backend->TryResolveShaderResource(depth))
        {
            return;
        }
        // 描画先1画素のUV幅
        const auto texel = TexelSize(target);
        // 処理前の主入力画像
        const auto source = m_backend->BeginOffscreenPostProcess(target);
        try
        {
            DrawFullscreen(
                source,
                fallbackTexture,
                FullscreenProgram::DepthOfField,
                {
                    std::max(settings.focusDistance, 0.01f),
                    std::clamp(settings.focusRange, 0.0f, 1000.0f),
                    std::clamp(settings.blurStrength, 0.0f, 8.0f),
                    std::clamp(settings.maximumRadius, 0.0f, 32.0f),
                    static_cast<float>(std::clamp(sampleCount, 4u, 64u)),
                    texel[0],
                    texel[1],
                    0.0f,
                    projection._33,
                    projection._43,
                    0.0f,
                    0.0f
                },
                { GraphicsViewHandle{}, depth });
        }
        catch (...)
        {
            m_backend->AbortOffscreenPostProcess(target);
            throw;
        }
        m_backend->EndOffscreenPostProcess(target);
    }

    bool D3D12SpriteRenderer::ResolveAmbientOcclusion(
        RenderTarget& target,
        const GraphicsViewHandle& fallbackTexture,
        const AmbientOcclusionSettings& settings,
        const DirectX::XMFLOAT4X4& projection,
        const std::uint32_t sampleCount)
    {
        // 正投影などで距離を戻せない射影は、D3D11と同じく遮蔽なしです。
        if (!settings.enabled
            || std::abs(projection._11) < 1e-6f
            || std::abs(projection._22) < 1e-6f)
        {
            return false;
        }
        // 深度効果の入力デバイス深度
        const auto depth = target.DepthViewHandle();
        if (!m_backend->TryResolveShaderResource(depth))
        {
            return false;
        }
        try
        {
            // 処理前の主入力画像
            const auto source =
                m_backend->BeginOffscreenAmbientOcclusionPass(target, false);
            // 半解像度AOのビューポート
            const auto& viewport = m_backend->ActiveViewport();
            // 効果別の16個のb1定数
            const std::array<float, 16> constants{
                1.0f / viewport.Width,
                1.0f / viewport.Height,
                std::clamp(settings.radius, 0.01f, 10.0f),
                std::clamp(settings.strength, 0.0f, 1.0f),
                static_cast<float>(std::clamp(sampleCount, 4u, 32u)),
                0.0f,
                0.0f,
                0.0f,
                projection._33,
                projection._43,
                1.0f / projection._11,
                1.0f / projection._22,
                0.0f,
                0.0f,
                0.0f,
                0.0f
            };
            DrawFullscreen(
                source,
                fallbackTexture,
                FullscreenProgram::AmbientOcclusion,
                constants,
                { GraphicsViewHandle{}, depth });
            // ブラー前の遮蔽率画像
            const auto occlusion =
                m_backend->BeginOffscreenAmbientOcclusionPass(target, true);
            DrawFullscreen(
                occlusion,
                fallbackTexture,
                FullscreenProgram::AmbientOcclusionBlur,
                constants,
                { GraphicsViewHandle{}, depth });
        }
        catch (...)
        {
            m_backend->AbortOffscreenAmbientOcclusion(target);
            throw;
        }
        m_backend->EndOffscreenAmbientOcclusion(target);
        return true;
    }

    bool D3D12SpriteRenderer::BuildReflectionDepthPyramid(
        RenderTarget& target,
        const GraphicsViewHandle& fallbackTexture,
        const float projectionZ,
        const float projectionW)
    {
        // Hi-Z距離画像の全段数
        const auto mipCount = target.ReflectionDepthPyramidMipCount();
        if (mipCount == 0u
            || !m_backend->TryResolveShaderResource(target.DepthViewHandle())
            || !m_backend->TryResolveShaderResource(
                target.ReflectionDepthPyramidViewHandle()))
        {
            return false;
        }
        // 開始前が深度専用の描画状態
        const bool depthOnly =
            m_backend->IsOffscreenTargetBoundDepthOnly(target);
        // Hi-Z最下段の幅
        const std::uint32_t width = std::max(target.Width(), 1u);
        // Hi-Z最下段の高さ
        const std::uint32_t height = std::max(target.Height(), 1u);
        try
        {
            // Hi-Zを作成するミップ番号
            for (std::uint32_t mip{}; mip < mipCount; ++mip)
            {
                // 処理前の主入力画像
                const auto source =
                    m_backend->BeginOffscreenReflectionDepthPass(target, mip);
                // 最下段は射影係数・以降は親幅
                const float parameterX = mip == 0u
                    ? projectionZ
                    : static_cast<float>(std::max(width >> (mip - 1u), 1u));
                // 最下段は距離係数・以降は親高
                const float parameterY = mip == 0u
                    ? projectionW
                    : static_cast<float>(
                        std::max(height >> (mip - 1u), 1u));
                DrawFullscreen(
                    source,
                    fallbackTexture,
                    mip == 0u
                        ? FullscreenProgram::ReflectionDepthLinearize
                        : FullscreenProgram::ReflectionDepthDownsample,
                    { parameterX, parameterY });
            }
        }
        catch (...)
        {
            m_backend->AbortOffscreenReflectionDepthPyramid(
                target,
                depthOnly);
            throw;
        }
        m_backend->EndOffscreenReflectionDepthPyramid(target, depthOnly);
        return true;
    }

    void D3D12SpriteRenderer::DrawSky(
        const DirectX::FXMMATRIX view,
        const DirectX::CXMMATRIX projection,
        const SkySettings& settings,
        const GraphicsViewHandle& cubemap,
        const SkySunDescription* const sun,
        const GraphicsViewHandle& fallbackTexture)
    {
        // 深度プリパス中のように色の描画先が無いときは、D3D11でRTV無しに描いた場合と同じく何も残しません。
        if (!settings.enabled
            || m_backend->ActiveColorFormat() == DXGI_FORMAT_UNKNOWN)
        {
            return;
        }
        using namespace DirectX;
        // 逆行列計算で受け取る行列式
        XMVECTOR determinant{};
        // 空の視線を復元する逆射影
        XMFLOAT4X4 inverseViewProjection{};
        XMStoreFloat4x4(
            &inverseViewProjection,
            XMMatrixInverse(&determinant, view * projection));
        // 空のカメラ位置を得る逆ビュー
        const XMMATRIX inverseView = XMMatrixInverse(&determinant, view);
        // 空を描く視点のワールド位置
        XMFLOAT4 cameraPosition{};
        XMStoreFloat4(&cameraPosition, inverseView.r[3]);

        // 天頂・水平線・地面・太陽の色
        std::array<float, 16> colors{
            settings.topColor.x,
            settings.topColor.y,
            settings.topColor.z,
            settings.intensity,
            settings.horizonColor.x,
            settings.horizonColor.y,
            settings.horizonColor.z,
            1.0f,
            settings.groundColor.x,
            settings.groundColor.y,
            settings.groundColor.z,
            1.0f,
            0.0f,
            0.0f,
            0.0f,
            0.0f
        };
        // 空の逆射影・視点・太陽の32値
        std::array<float, 32> viewConstants{};
        std::memcpy(
            viewConstants.data(),
            &inverseViewProjection,
            sizeof(inverseViewProjection));
        std::memcpy(
            viewConstants.data() + 16,
            &cameraPosition,
            sizeof(cameraPosition));
        if (sun != nullptr)
        {
            // 太陽方向ベクトルの長さ
            const auto length = std::sqrt(
                sun->directionToSun.x * sun->directionToSun.x
                + sun->directionToSun.y * sun->directionToSun.y
                + sun->directionToSun.z * sun->directionToSun.z);
            // 太陽方向を正規化する倍率
            const float scale =
                length > 0.0001f ? 1.0f / length : 0.0f;
            viewConstants[20] = sun->directionToSun.x * scale;
            viewConstants[21] = sun->directionToSun.y * scale;
            viewConstants[22] = sun->directionToSun.z * scale;
            // D3D11と同じく、角半径0でも太陽円盤が消えない最小値です。
            viewConstants[23] = std::max(sun->angularRadius, 0.004625f);
            colors[12] = sun->color.x;
            colors[13] = sun->color.y;
            colors[14] = sun->color.z;
            colors[15] = 1.0f;
        }
        // 空キューブ画像のGPU参照
        const auto cubemapBinding =
            m_backend->TryResolveShaderResource(cubemap);
        // 有効キューブ画像を使う状態
        const bool useCubemap = cubemapBinding.has_value()
            && cubemapBinding->dimension == D3D12_SRV_DIMENSION_TEXTURECUBE;
        viewConstants[24] = useCubemap ? 1.0f : 0.0f;
        DrawFullscreen(
            fallbackTexture,
            fallbackTexture,
            FullscreenProgram::Sky,
            colors,
            {
                useCubemap ? cubemap : GraphicsViewHandle{},
                GraphicsViewHandle{},
                GraphicsViewHandle{}
            },
            viewConstants);
    }

    void D3D12SpriteRenderer::MeasureLuminance(
        RenderTarget& target,
        const GraphicsViewHandle& fallbackTexture)
    {
        // 対数輝度を縮小する段数
        const auto levelCount =
            m_backend->OffscreenLuminanceLevelCount(target);
        try
        {
            // 対数輝度の縮小段番号
            for (std::uint32_t level{}; level < levelCount; ++level)
            {
                // 処理前の主入力画像
                const auto source =
                    m_backend->BeginOffscreenLuminancePass(target, level);
                // 半解像度AOのビューポート
                const auto& viewport = m_backend->ActiveViewport();
                // 1段目はPSLuminance、以降はGenerateMipsと同じく前段の2x2をbilinear 1回で平均します。
                DrawFullscreen(
                    source,
                    fallbackTexture,
                    level == 0u
                        ? FullscreenProgram::Luminance
                        : FullscreenProgram::None,
                    {
                        1.0f / viewport.Width,
                        1.0f / viewport.Height,
                        0.0f,
                        0.0f,
                        0.0f,
                        0.0f,
                        0.0f,
                        0.0f
                    });
            }
        }
        catch (...)
        {
            m_backend->AbortOffscreenPostProcess(target);
            throw;
        }
        m_backend->BindOffscreenTarget(target);
    }

    bool D3D12SpriteRenderer::PrepareScreenEffect(
        AssetManager& assets,
        const std::filesystem::path& shaderPath,
        const std::function<std::string(const char*)>& describeFailure,
        std::uint64_t* const generation,
        std::string* const error)
    {
        if (generation != nullptr)
        {
            *generation = 0;
        }
        if (error != nullptr)
        {
            error->clear();
        }
        if (shaderPath.empty())
        {
            return false;
        }

        // 正規化した画面効果HLSLパス
        const auto absolutePath =
            assets.ResolvePath(shaderPath).lexically_normal();
        // パス別の画面効果キャッシュ
        auto& entry = m_screenShaders[absolutePath];

        // 変更を確認する現在時刻
        const auto now = std::chrono::steady_clock::now();
        if (!entry.observed
            || entry.forceReload
            || now >= entry.nextCheck)
        {
            entry.nextCheck = now + std::chrono::milliseconds(250);
            // 資産がアーカイブ内にある状態
            const bool archived = assets.IsArchived();
            // ソース更新時刻取得のエラー
            std::error_code fileError;
            // 画面効果ソースが存在する状態
            const bool sourceExists = assets.FileExists(absolutePath);
            // 今回観測したソース更新時刻
            const auto writeTime = (sourceExists && !archived)
                ? std::filesystem::last_write_time(absolutePath, fileError)
                : std::filesystem::file_time_type{};
            // 画面効果を再コンパイルする条件
            const bool changed = !entry.observed
                || entry.forceReload
                || entry.sourceExists != sourceExists
                || (sourceExists
                    && !archived
                    && entry.writeTime != writeTime);
            if (changed)
            {
                entry.observed = true;
                entry.forceReload = false;
                entry.sourceExists = sourceExists;
                entry.writeTime = writeTime;
                if (!sourceExists)
                {
                    entry.error =
                        "Screen effect shader file was not found: "
                        + PathToUtf8(absolutePath);
                }
                else
                {
                    try
                    {
                        // 候補のコンパイル済みVS
                        auto vertexShader = CompileShaderCached(
                            assets,
                            absolutePath,
                            "VSMain",
                            "vs_5_0");
                        // 候補のコンパイル済みPS
                        auto pixelShader = CompileShaderCached(
                            assets,
                            absolutePath,
                            "PSMain",
                            "ps_5_0");
                        entry.vertexShader = std::move(vertexShader);
                        entry.pixelShader = std::move(pixelShader);
                        // 旧シェーダーに依存するPSO
                        for (auto& pipeline : entry.pipelineStates)
                        {
                            pipeline.Reset();
                        }
                        entry.generation = m_nextScreenShaderGeneration++;
                        if (m_nextScreenShaderGeneration == 0)
                        {
                            m_nextScreenShaderGeneration = 1;
                        }
                        entry.error.clear();
                    }
                    // exception: 候補VS・PSの読込み失敗。
                    catch (const std::exception& exception)
                    {
                        // 再compileに失敗しても、直前の正常版は維持します。
                        entry.error = describeFailure
                            ? describeFailure(exception.what())
                            : std::string(exception.what());
                    }
                }
            }
        }

        if (generation != nullptr)
        {
            *generation = entry.generation;
        }
        if (error != nullptr)
        {
            *error = entry.error;
        }
        return entry.vertexShader != nullptr
            && entry.pixelShader != nullptr;
    }

    void D3D12SpriteRenderer::ApplyScreenEffect(
        RenderTarget& target,
        const GraphicsViewHandle& fallbackTexture,
        AssetManager& assets,
        const std::filesystem::path& shaderPath,
        const std::array<GraphicsViewHandle, 2>& auxiliaryTextures,
        const std::array<DirectX::XMFLOAT4, 8>& parameters,
        const DirectX::XMFLOAT4& depthParameters,
        const DirectX::XMFLOAT4& depthUnprojection)
    {
        // 正規化した画面効果HLSLパス
        const auto absolutePath =
            assets.ResolvePath(shaderPath).lexically_normal();
        // 準備済み画面効果の検索結果
        const auto found = m_screenShaders.find(absolutePath);
        if (found == m_screenShaders.end()
            || found->second.vertexShader == nullptr
            || found->second.pixelShader == nullptr)
        {
            throw std::logic_error(
                "The DirectX 12 screen effect was not prepared.");
        }

        // 処理前の主入力画像
        const auto source = m_backend->BeginOffscreenPostProcess(target);
        try
        {
            // 主画像・補助2画像・深度の参照
            std::array<GraphicsViewHandle, 4> views{
                source,
                auxiliaryTextures[0]
                    ? auxiliaryTextures[0]
                    : fallbackTexture,
                auxiliaryTextures[1]
                    ? auxiliaryTextures[1]
                    : fallbackTexture,
                target.DepthViewHandle()
                    ? target.DepthViewHandle()
                    : fallbackTexture
            };
            // t0～t3へ渡すGPU画像枠
            std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 4> descriptors{};
            // t0～t3の画像参照番号
            for (std::size_t index{}; index < views.size(); ++index)
            {
                // 入力画像のGPU参照の解決結果
                const auto binding =
                    m_backend->TryResolveShaderResource(views[index]);
                if (!binding)
                {
                    throw std::invalid_argument(
                        "A DirectX 12 screen effect requires current "
                        "source, auxiliary, and depth views.");
                }
                descriptors[index] = binding->descriptor;
            }
            // 画像SRVを持つGPUヒープ
            auto* const descriptorHeap =
                m_backend->ShaderResourceDescriptorHeap();
            if (descriptorHeap == nullptr)
            {
                throw std::logic_error(
                    "A DirectX 12 screen effect requires a shader resource "
                    "descriptor heap.");
            }

            // b0の176バイト画面効果定数
            ScreenEffectConstants constants;
            constants.parameters = parameters;
            constants.screenSize = {
                static_cast<float>(std::max(target.Width(), 1u)),
                static_cast<float>(std::max(target.Height(), 1u)),
                1.0f / static_cast<float>(std::max(target.Width(), 1u)),
                1.0f / static_cast<float>(std::max(target.Height(), 1u))
            };
            constants.depthParameters = depthParameters;
            constants.depthUnprojection = depthUnprojection;
            // フレームが保持するb0転送領域
            const auto upload = m_backend->AllocateFrameUpload(
                sizeof(constants),
                D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            std::memcpy(upload.data, &constants, sizeof(constants));

            // 画面効果を記録するコマンド列
            auto* const commandList = m_backend->BeginFrameCommands();
            // 描画に設定するSRVヒープ
            ID3D12DescriptorHeap* heaps[]{ descriptorHeap };
            commandList->SetGraphicsRootSignature(
                m_screenEffectRootSignature.Get());
            commandList->SetPipelineState(ScreenEffectPipelineState(
                found->second,
                m_backend->ActiveColorFormat()));
            commandList->SetDescriptorHeaps(1, heaps);
            commandList->SetGraphicsRootConstantBufferView(
                0,
                upload.gpuAddress);
            // t0～t3の画像参照番号
            for (std::size_t index{}; index < descriptors.size(); ++index)
            {
                commandList->SetGraphicsRootDescriptorTable(
                    static_cast<UINT>(index + 1u),
                    descriptors[index]);
            }
            // 現在の描画先のビューポート
            const auto& viewport = m_backend->ActiveViewport();
            // 現在の描画先のクリップ矩形
            const auto& scissor = m_backend->ActiveScissorRectangle();
            commandList->RSSetViewports(1, &viewport);
            commandList->RSSetScissorRects(1, &scissor);
            commandList->IASetPrimitiveTopology(
                D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commandList->IASetVertexBuffers(0, 0, nullptr);
            commandList->IASetIndexBuffer(nullptr);
            commandList->DrawInstanced(3, 1, 0, 0);
        }
        catch (...)
        {
            m_backend->AbortOffscreenPostProcess(target);
            throw;
        }
        m_backend->EndOffscreenPostProcess(target);
    }

    void D3D12SpriteRenderer::InvalidateScreenEffect(
        AssetManager& assets,
        const std::filesystem::path& shaderPath) noexcept
    {
        if (shaderPath.empty())
        {
            return;
        }
        try
        {
            // 準備済み画面効果の検索結果
            const auto found = m_screenShaders.find(
                assets.ResolvePath(shaderPath).lexically_normal());
            if (found != m_screenShaders.end())
            {
                found->second.forceReload = true;
            }
        }
        catch (...)
        {
        }
    }

    void D3D12SpriteRenderer::ApplyPostProcessPass(
        RenderTarget& target,
        const GraphicsViewHandle& fallbackTexture,
        const FullscreenProgram program,
        const std::array<float, 16>& constants)
    {
        // 処理前の主入力画像
        const auto source = m_backend->BeginOffscreenPostProcess(target);
        try
        {
            DrawFullscreen(
                source,
                fallbackTexture,
                program,
                constants);
        }
        catch (...)
        {
            m_backend->AbortOffscreenPostProcess(target);
            throw;
        }
        m_backend->EndOffscreenPostProcess(target);
    }

    void D3D12SpriteRenderer::DrawFullscreen(
        const GraphicsViewHandle& texture,
        const GraphicsViewHandle& fallbackTexture,
        const FullscreenProgram program,
        const std::array<float, 16>& constants,
        const std::array<GraphicsViewHandle, 3>& auxiliaryViews,
        const std::array<float, 32>& matrixConstants)
    {
        // 主入力画像のGPU参照
        const auto binding = m_backend->TryResolveShaderResource(texture);
        // 全画面を覆う描画先の寸法
        const auto& viewport = m_backend->ActiveViewport();
        if (!binding
            || binding->width == 0u
            || binding->height == 0u
            || viewport.Width <= 0.0f
            || viewport.Height <= 0.0f)
        {
            throw std::invalid_argument(
                "A DirectX 12 fullscreen pass requires a current texture "
                "and output viewport.");
        }
        // 不透明合成の全画面パス指定
        SpritePassDescription description;
        description.blend = SpriteBlendMode::Opaque;
        // 全画面パスの開始結果
        SpriteShaderStatus status;
        // 開始した全画面パスの識別番号
        const auto token = Begin(
            description,
            fallbackTexture,
            nullptr,
            status);
        m_program = program;
        m_passConstants = constants;
        m_matrixConstants = matrixConstants;
        m_auxiliaryViews = auxiliaryViews;
        if (program == FullscreenProgram::Temporal)
        {
            // TAAの履歴または深度の枠番号
            for (std::size_t index{}; index < 2u; ++index)
            {
                // TAA補助画像のGPU参照
                const auto auxiliary = m_backend->TryResolveShaderResource(
                    m_auxiliaryViews[index]);
                if (!auxiliary)
                {
                    Abort(token);
                    throw std::invalid_argument(
                        "The DirectX 12 temporal pass requires current "
                        "history and depth views.");
                }
                m_auxiliaryTextures[index] = auxiliary->descriptor;
            }
        }
        else if (program == FullscreenProgram::LensFlareComposite)
        {
            // レンズフレア光条のGPU参照
            const auto streak = m_backend->TryResolveShaderResource(
                m_auxiliaryViews[0]);
            if (!streak)
            {
                Abort(token);
                throw std::invalid_argument(
                    "The DirectX 12 lens flare pass requires a current "
                    "streak view.");
            }
            m_auxiliaryTextures[0] = streak->descriptor;
        }
        else if (program == FullscreenProgram::VolumetricLight)
        {
            // 深度効果画像のGPU参照
            const auto depth = m_backend->TryResolveShaderResource(
                m_auxiliaryViews[1]);
            // 光の筋用の影配列のGPU参照
            const auto shadow = m_backend->TryResolveShaderResource(
                m_auxiliaryViews[2]);
            if (!depth || !shadow)
            {
                Abort(token);
                throw std::invalid_argument(
                    "The DirectX 12 volumetric light pass requires current "
                    "depth and cascade shadow views.");
            }
            m_auxiliaryTextures[1] = depth->descriptor;
            m_auxiliaryTextures[2] = shadow->descriptor;
        }
        else if (program == FullscreenProgram::Sky)
        {
            // 空キューブ画像のGPU参照
            const auto cubemap = m_backend->TryResolveShaderResource(
                m_auxiliaryViews[0]);
            try
            {
                m_auxiliaryTextures[0] = cubemap.has_value()
                    ? cubemap->descriptor
                    : m_backend->NullShaderResourceDescriptor(
                        D3D12_SRV_DIMENSION_TEXTURECUBE);
            }
            catch (...)
            {
                Abort(token);
                throw;
            }
        }
        else if (program == FullscreenProgram::ScreenOutline
            || program == FullscreenProgram::MotionBlur
            || program == FullscreenProgram::DepthOfField
            || program == FullscreenProgram::AmbientOcclusion
            || program == FullscreenProgram::AmbientOcclusionBlur)
        {
            // 深度効果画像のGPU参照
            const auto depth = m_backend->TryResolveShaderResource(
                m_auxiliaryViews[1]);
            if (!depth)
            {
                Abort(token);
                throw std::invalid_argument(
                    "The DirectX 12 depth post-process requires a current "
                    "depth view.");
            }
            m_auxiliaryTextures[1] = depth->descriptor;
        }
        try
        {
            // 描画先全体を覆う矩形要求
            SpriteDrawRequest request;
            request.texture = texture;
            request.scale = {
                viewport.Width / static_cast<float>(binding->width),
                viewport.Height / static_cast<float>(binding->height) };
            if (!Draw(token, request))
            {
                throw std::runtime_error(
                    "The texture was rejected by the DirectX 12 "
                    "fullscreen pass.");
            }
            End(token);
        }
        catch (...)
        {
            Abort(token);
            throw;
        }
    }

    void D3D12SpriteRenderer::RequireOwner(
        const std::uint64_t token) const
    {
        if (token == 0 || token != m_activeToken)
        {
            throw std::logic_error(
                "The sprite render pass does not own the active DirectX 12 "
                "batch.");
        }
    }

    void D3D12SpriteRenderer::FlushOrFail()
    {
        try
        {
            Flush();
        }
        catch (...)
        {
            m_failed = true;
            ClearPass();
            throw;
        }
    }

    void D3D12SpriteRenderer::ClearPass() noexcept
    {
        m_sprites.clear();
        m_scissorStack.clear();
        m_fallbackTexture.Reset();
        // 解放する補助入力の保持参照
        for (auto& view : m_auxiliaryViews)
        {
            view.Reset();
        }
        m_auxiliaryTextures = {};
        m_matrixConstants = {};
        m_volumetricConstants = {};
        m_customParameters = {};
        m_spriteLighting = {};
        m_program = FullscreenProgram::None;
        m_activeCustomShader = nullptr;
        m_activeToken = 0;
    }

    void D3D12SpriteRenderer::Flush()
    {
        if (m_sprites.empty())
        {
            return;
        }

        // 予約矩形を記録するコマンド列
        auto* const commandList = m_backend->BeginFrameCommands();
        // 画像SRVを持つGPUヒープ
        auto* const descriptorHeap =
            m_backend->ShaderResourceDescriptorHeap();
        if (descriptorHeap == nullptr)
        {
            throw std::logic_error(
                "The DirectX 12 sprite renderer requires a shader resource "
                "descriptor heap.");
        }
        // 予約した全矩形の頂点バイト数
        const std::uint64_t vertexBytes =
            static_cast<std::uint64_t>(m_sprites.size())
            * sizeof(QueuedSprite::vertices);
        if (vertexBytes > std::numeric_limits<UINT>::max())
        {
            throw std::length_error(
                "The DirectX 12 sprite batch is too large.");
        }
        // 合成・クリップ・効果に対応するPSO
        auto* const pipelineState = PipelineState(
            m_blend,
            !m_scissorStack.empty(),
            m_backend->ActiveColorFormat(),
            m_backend->ActiveDepthFormat(),
            m_program);

        // フレームが保持する全頂点の領域
        const auto upload = m_backend->AllocateFrameUpload(
            vertexBytes,
            alignof(Vertex));
        // 矩形頂点の転送先ポインター
        auto* vertexData = upload.data;
        // 順序を保って転送する予約矩形
        for (const auto& sprite : m_sprites)
        {
            std::memcpy(
                vertexData,
                sprite.vertices.data(),
                sizeof(sprite.vertices));
            vertexData += sizeof(sprite.vertices);
        }

        // 現在の描画先のビューポート
        const auto& viewport = m_backend->ActiveViewport();
        // 画素座標をNDCへ移すXY倍率
        const std::array<float, 2> viewportScale{
            viewport.Width > 0.0f ? 2.0f / viewport.Width : 0.0f,
            viewport.Height > 0.0f ? 2.0f / viewport.Height : 0.0f
        };
        // 現在のクリップまたは全体矩形
        const D3D12_RECT scissor = m_scissorStack.empty()
            ? m_backend->ActiveScissorRectangle()
            : m_scissorStack.back();
        // 予約矩形の頂点バッファビュー
        const D3D12_VERTEX_BUFFER_VIEW vertexBufferView{
            upload.gpuAddress,
            static_cast<UINT>(vertexBytes),
            static_cast<UINT>(sizeof(Vertex))
        };
        // 2048枚共通の16ビット索引ビュー
        const D3D12_INDEX_BUFFER_VIEW indexBufferView{
            m_indexBuffer->GetGPUVirtualAddress(),
            static_cast<UINT>(
                MaximumSpritesPerDraw * IndicesPerSprite
                * sizeof(std::uint16_t)),
            DXGI_FORMAT_R16_UINT
        };
        // 描画に設定するSRVヒープ
        ID3D12DescriptorHeap* descriptorHeaps[]{ descriptorHeap };

        commandList->SetGraphicsRootSignature(m_rootSignature.Get());
        commandList->SetPipelineState(pipelineState);
        commandList->SetDescriptorHeaps(
            static_cast<UINT>(std::size(descriptorHeaps)),
            descriptorHeaps);
        commandList->SetGraphicsRoot32BitConstants(
            0,
            static_cast<UINT>(viewportScale.size()),
            viewportScale.data(),
            0);
        if (m_activeCustomShader != nullptr)
        {
            // カスタムPSのb0転送領域
            const auto parameters = m_backend->AllocateFrameUpload(
                sizeof(m_customParameters),
                D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            std::memcpy(
                parameters.data,
                m_customParameters.data(),
                sizeof(m_customParameters));
            // カスタムPSのb1照明転送領域
            const auto lighting = m_backend->AllocateFrameUpload(
                sizeof(m_spriteLighting),
                D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            std::memcpy(
                lighting.data,
                &m_spriteLighting,
                sizeof(m_spriteLighting));
            commandList->SetGraphicsRootConstantBufferView(
                8,
                parameters.gpuAddress);
            commandList->SetGraphicsRootConstantBufferView(
                2,
                lighting.gpuAddress);
        }
        else if (m_program != FullscreenProgram::None)
        {
            // 効果別のb1またはb3転送領域
            const auto constants = m_backend->AllocateFrameUpload(
                sizeof(m_passConstants),
                D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            std::memcpy(
                constants.data,
                m_passConstants.data(),
                sizeof(m_passConstants));
            commandList->SetGraphicsRootConstantBufferView(
                2,
                constants.gpuAddress);
        }
        if (m_program == FullscreenProgram::Temporal)
        {
            commandList->SetGraphicsRootDescriptorTable(
                3,
                m_auxiliaryTextures[0]);
            commandList->SetGraphicsRootDescriptorTable(
                4,
                m_auxiliaryTextures[1]);
            commandList->SetGraphicsRoot32BitConstants(
                5,
                static_cast<UINT>(m_matrixConstants.size()),
                m_matrixConstants.data(),
                0);
        }
        else if (m_program == FullscreenProgram::LensFlareComposite)
        {
            commandList->SetGraphicsRootDescriptorTable(
                3,
                m_auxiliaryTextures[0]);
        }
        else if (m_program == FullscreenProgram::VolumetricLight)
        {
            // 効果別のb1またはb3転送領域
            const auto constants = m_backend->AllocateFrameUpload(
                sizeof(m_volumetricConstants),
                D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            std::memcpy(
                constants.data,
                &m_volumetricConstants,
                sizeof(m_volumetricConstants));
            commandList->SetGraphicsRootDescriptorTable(
                4,
                m_auxiliaryTextures[1]);
            commandList->SetGraphicsRootDescriptorTable(
                6,
                m_auxiliaryTextures[2]);
            commandList->SetGraphicsRootConstantBufferView(
                7,
                constants.gpuAddress);
        }
        else if (m_program == FullscreenProgram::ScreenOutline
            || m_program == FullscreenProgram::MotionBlur
            || m_program == FullscreenProgram::DepthOfField
            || m_program == FullscreenProgram::AmbientOcclusion
            || m_program == FullscreenProgram::AmbientOcclusionBlur)
        {
            commandList->SetGraphicsRootDescriptorTable(
                4,
                m_auxiliaryTextures[1]);
            if (m_program == FullscreenProgram::MotionBlur)
            {
                commandList->SetGraphicsRoot32BitConstants(
                    5,
                    static_cast<UINT>(m_matrixConstants.size()),
                    m_matrixConstants.data(),
                    0);
            }
        }
        else if (m_program == FullscreenProgram::Sky)
        {
            commandList->SetGraphicsRootDescriptorTable(
                3,
                m_auxiliaryTextures[0]);
            commandList->SetGraphicsRoot32BitConstants(
                5,
                static_cast<UINT>(m_matrixConstants.size()),
                m_matrixConstants.data(),
                0);
        }
        commandList->IASetPrimitiveTopology(
            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList->IASetVertexBuffers(0, 1, &vertexBufferView);
        commandList->IASetIndexBuffer(&indexBufferView);
        commandList->RSSetScissorRects(1, &scissor);

        // 同じ画像が続く範囲の先頭番号
        std::size_t first{};
        while (first < m_sprites.size())
        {
            // 最大2048枚で区切る範囲の末尾
            std::size_t end = first + 1u;
            while (end < m_sprites.size()
                && end - first < MaximumSpritesPerDraw
                && m_sprites[end].texture.ptr
                    == m_sprites[first].texture.ptr)
            {
                ++end;
            }
            commandList->SetGraphicsRootDescriptorTable(
                1,
                m_sprites[first].texture);
            // 16ビット索引をBaseVertexLocationでずらし、各送信範囲に再利用します。
            commandList->DrawIndexedInstanced(
                static_cast<UINT>((end - first) * IndicesPerSprite),
                1,
                0,
                static_cast<INT>(first * VerticesPerSprite),
                0);
            first = end;
        }
        // 後続描画へ戻す全体クリップ
        const auto& fullScissor = m_backend->ActiveScissorRectangle();
        commandList->RSSetScissorRects(1, &fullScissor);
        m_sprites.clear();
    }

    ID3D12PipelineState* D3D12SpriteRenderer::ScreenEffectPipelineState(
        ScreenShaderEntry& shader,
        const DXGI_FORMAT colorFormat)
    {
        // 出力色形式のPSO配列番号
        const std::size_t formatIndex = colorFormat
                == D3D12Backend::PrimaryColorFormat
            ? 0u
            : colorFormat == DXGI_FORMAT_R16G16B16A16_FLOAT
                ? 1u
                : colorFormat == DXGI_FORMAT_R8_UNORM
                    ? 2u
                    : colorFormat == DXGI_FORMAT_R32_FLOAT
                        ? 3u
                        : throw std::invalid_argument(
                            "The active DirectX 12 screen effect target "
                            "format is unsupported.");
        // 描画条件に対応するPSOキャッシュ
        auto& pipeline = shader.pipelineStates[formatIndex];
        if (pipeline != nullptr)
        {
            return pipeline.Get();
        }

        // GPUへ渡すPSO作成情報
        D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
        description.pRootSignature = m_screenEffectRootSignature.Get();
        description.VS = {
            shader.vertexShader->GetBufferPointer(),
            shader.vertexShader->GetBufferSize()
        };
        description.PS = {
            shader.pixelShader->GetBufferPointer(),
            shader.pixelShader->GetBufferSize()
        };
        description.BlendState =
            MakeBlendDescription(SpriteBlendMode::Opaque);
        description.SampleMask = std::numeric_limits<UINT>::max();
        description.RasterizerState = MakeRasterizerDescription(true);
        description.DepthStencilState = MakeDepthStencilDescription();
        description.PrimitiveTopologyType =
            D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        description.NumRenderTargets = 1;
        description.RTVFormats[0] = colorFormat;
        description.DSVFormat = DXGI_FORMAT_UNKNOWN;
        description.SampleDesc.Count = 1;
        ThrowIfFailed(
            m_backend->Device()->CreateGraphicsPipelineState(
                &description,
                IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateGraphicsPipelineState(screen effect)");
        return pipeline.Get();
    }

    ID3D12PipelineState* D3D12SpriteRenderer::PipelineState(
        const SpriteBlendMode blend,
        const bool scissored,
        const DXGI_FORMAT colorFormat,
        const DXGI_FORMAT depthFormat,
        const FullscreenProgram program)
    {
        // 合成方式の配列番号
        const auto blendIndex = static_cast<std::size_t>(blend);
        if (blend > SpriteBlendMode::Opaque)
        {
            throw std::invalid_argument(
                "The sprite blend mode is invalid.");
        }
        if (program > FullscreenProgram::Sky)
        {
            throw std::invalid_argument(
                "The sprite pixel program is invalid.");
        }
        // 出力色形式のPSO配列番号
        const std::size_t formatIndex = colorFormat
                == D3D12Backend::PrimaryColorFormat
            ? 0u
            : colorFormat == DXGI_FORMAT_R16G16B16A16_FLOAT
                ? 1u
                : colorFormat == DXGI_FORMAT_R8_UNORM
                    ? 2u
                    : colorFormat == DXGI_FORMAT_R32_FLOAT
                        ? 3u
                        : throw std::invalid_argument(
                            "The active DirectX 12 sprite target format "
                            "is unsupported.");
        // 深度あり・なしの配列番号
        const std::size_t depthIndex = depthFormat
                == D3D12Backend::PrimaryDepthFormat
            ? 0u
            : depthFormat == DXGI_FORMAT_UNKNOWN
                ? 1u
                : throw std::invalid_argument(
                    "The active DirectX 12 sprite depth format is "
                    "unsupported.");
        // 深度・色・合成・クリップの番号
        const auto variantIndex =
            (depthIndex * ColorFormatVariants + formatIndex)
                * BlendVariants
            + blendIndex * 2u
            + (scissored ? 1u : 0u);
        // 描画条件に対応するPSOキャッシュ
        auto& pipeline = m_activeCustomShader != nullptr
                && program == FullscreenProgram::None
            ? m_activeCustomShader->pipelineStates[variantIndex]
            : m_pipelineStates[
                static_cast<std::size_t>(program)
                    * DepthFormatVariants
                    * ColorFormatVariants
                    * BlendVariants
                + variantIndex];
        if (pipeline != nullptr)
        {
            return pipeline.Get();
        }

        // 位置・色・UVのスプライト入力配置
        static const std::array<D3D12_INPUT_ELEMENT_DESC, 3>
            InputElements{ {
                {
                    "POSITION",
                    0,
                    DXGI_FORMAT_R32G32B32_FLOAT,
                    0,
                    offsetof(Vertex, position),
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                    0
                },
                {
                    "COLOR",
                    0,
                    DXGI_FORMAT_R32G32B32A32_FLOAT,
                    0,
                    offsetof(Vertex, color),
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                    0
                },
                {
                    "TEXCOORD",
                    0,
                    DXGI_FORMAT_R32G32_FLOAT,
                    0,
                    offsetof(Vertex, textureCoordinate),
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                    0
                }
            } };

        // 効果またはカスタムPSの借用参照
        ID3DBlob* pixelShader = m_activeCustomShader != nullptr
                && program == FullscreenProgram::None
            ? m_activeCustomShader->pixelShader.Get()
            : m_pixelShader.Get();
        switch (program)
        {
        case FullscreenProgram::ToneMap:
            pixelShader = m_toneMapPixelShader.Get();
            break;
        case FullscreenProgram::Bloom:
            pixelShader = m_bloomPixelShader.Get();
            break;
        case FullscreenProgram::Fxaa:
            pixelShader = m_fxaaPixelShader.Get();
            break;
        case FullscreenProgram::Luminance:
            pixelShader = m_luminancePixelShader.Get();
            break;
        case FullscreenProgram::Temporal:
            pixelShader = m_temporalPixelShader.Get();
            break;
        case FullscreenProgram::ScreenOutline:
            pixelShader = m_screenOutlinePixelShader.Get();
            break;
        case FullscreenProgram::MotionBlur:
            pixelShader = m_motionBlurPixelShader.Get();
            break;
        case FullscreenProgram::DepthOfField:
            pixelShader = m_depthOfFieldPixelShader.Get();
            break;
        case FullscreenProgram::AmbientOcclusion:
            pixelShader = m_ambientOcclusionPixelShader.Get();
            break;
        case FullscreenProgram::AmbientOcclusionBlur:
            pixelShader = m_ambientOcclusionBlurPixelShader.Get();
            break;
        case FullscreenProgram::VolumetricLight:
            pixelShader = m_volumetricLightPixelShader.Get();
            break;
        case FullscreenProgram::LensFlareStreak:
            pixelShader = m_lensFlareStreakPixelShader.Get();
            break;
        case FullscreenProgram::LensFlareComposite:
            pixelShader = m_lensFlareCompositePixelShader.Get();
            break;
        case FullscreenProgram::ReflectionDepthLinearize:
            pixelShader = m_reflectionDepthLinearizePixelShader.Get();
            break;
        case FullscreenProgram::ReflectionDepthDownsample:
            pixelShader = m_reflectionDepthDownsamplePixelShader.Get();
            break;
        case FullscreenProgram::Sky:
            pixelShader = m_skyPixelShader.Get();
            break;
        case FullscreenProgram::None:
            break;
        }
        // GPUへ渡すPSO作成情報
        D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
        description.pRootSignature = m_rootSignature.Get();
        description.VS = {
            m_vertexShader->GetBufferPointer(),
            m_vertexShader->GetBufferSize()
        };
        description.PS = {
            pixelShader->GetBufferPointer(),
            pixelShader->GetBufferSize()
        };
        description.BlendState = MakeBlendDescription(blend);
        description.SampleMask = std::numeric_limits<UINT>::max();
        description.RasterizerState = MakeRasterizerDescription(scissored);
        description.DepthStencilState = MakeDepthStencilDescription();
        description.InputLayout = {
            InputElements.data(),
            static_cast<UINT>(InputElements.size())
        };
        description.PrimitiveTopologyType =
            D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        description.NumRenderTargets = 1;
        // Primary outputには深度bufferもbindされるため、深度testを使わないSpriteでもformatだけは一致させます。
        description.RTVFormats[0] = colorFormat;
        description.DSVFormat = depthFormat;
        description.SampleDesc.Count = 1;
        ThrowIfFailed(
            m_backend->Device()->CreateGraphicsPipelineState(
                &description,
                IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateGraphicsPipelineState(sprite)");
        return pipeline.Get();
    }
}
