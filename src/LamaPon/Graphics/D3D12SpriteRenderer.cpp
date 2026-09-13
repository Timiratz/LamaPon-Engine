#include "LamaPon/Graphics/D3D12SpriteRenderer.h"

#include "LamaPon/Graphics/D3D12Backend.h"
#include "LamaPon/Graphics/EnvironmentSettings.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/RenderTargetBackendState.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
    // DirectXTK SpriteBatchと同じく、1回のdrawで送るquad数の上限です。
    // 16bit indexをBaseVertexLocationと組み合わせて全quadへ再利用します。
    constexpr std::size_t MaximumSpritesPerDraw = 2048u;
    constexpr std::size_t VerticesPerSprite = 4u;
    constexpr std::size_t IndicesPerSprite = 6u;

    // SpriteBatchのviewport変換と同じ式で、pixel座標をclip空間へ移します。
    constexpr char SpriteShaderSource[] = R"(
cbuffer SpriteViewport : register(b0)
{
    float2 ViewportScale;
};

// 最終合成とpost-processのfullscreen passが共用するroot constantsです。
// 意味はpixel shaderごとに異なります。
cbuffer FullscreenPass : register(b1)
{
    float4 PassPrimary;
    float4 PassSecondary;
    float4 PassTertiary;
    float4 PassQuaternary;
};

cbuffer TemporalPass : register(b2)
{
    row_major float4x4 TemporalInverseViewProjection;
    row_major float4x4 TemporalPreviousViewProjection;
};

cbuffer VolumetricPass : register(b3)
{
    row_major float4x4 VolumetricInverseViewProjection;
    float4 VolumetricCameraPosition;
    float4 VolumetricLightDirection;
    float4 VolumetricLightColor;
    row_major float4x4 VolumetricCascades[4];
    float4 VolumetricShadowParameters;
};

Texture2D SpriteTexture : register(t0);
Texture2D TemporalHistoryTexture : register(t1);
Texture2D DepthTexture : register(t2);
Texture2DArray<float> VolumetricShadowTexture : register(t3);
SamplerState SpriteSampler : register(s0);
SamplerComparisonState VolumetricShadowSampler : register(s1);

struct VertexInput
{
    float3 position : POSITION;
    float4 color : COLOR;
    float2 textureCoordinate : TEXCOORD;
};

struct PixelInput
{
    float4 position : SV_Position;
    float4 color : COLOR;
    float2 textureCoordinate : TEXCOORD;
};

PixelInput SpriteVertexShader(VertexInput input)
{
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

float4 SpritePixelShader(PixelInput input) : SV_Target
{
    return SpriteTexture.Sample(SpriteSampler, input.textureCoordinate)
        * input.color;
}

// LamaPonEnvironment.hlslと同じRec.601の係数です。トーンマップの彩度と
// FXAAの縁検出が共用するため、D3D11と同じ値を保ちます。
float Luminance(float3 color)
{
    return dot(color, float3(0.299f, 0.587f, 0.114f));
}

float3 ACESFilm(float3 color)
{
    return saturate(
        color * (2.51f * color + 0.03f)
        / (color * (2.43f * color + 0.59f) + 0.14f));
}

float4 ToneMappedPixelShader(PixelInput input) : SV_Target
{
    // 露出、コントラスト、彩度、色温度と、色合い、周辺減光、有効状態、
    // 自動露出の補正（段数）です。
    const float4 ColorGradePrimary = PassPrimary;
    const float4 ColorGradeSecondary = PassSecondary;
    float3 color = max(
        SpriteTexture.Sample(
            SpriteSampler,
            input.textureCoordinate).rgb * input.color.rgb,
        0.0f);
    const float gradingEnabled = saturate(ColorGradeSecondary.z);
    const float exposure = lerp(
        0.0f,
        ColorGradePrimary.x,
        gradingEnabled);
    const float temperature = lerp(
        0.0f,
        ColorGradePrimary.w,
        gradingEnabled);
    const float tint = lerp(
        0.0f,
        ColorGradeSecondary.x,
        gradingEnabled);
    const float3 whiteBalance = max(float3(
        1.0f + temperature * 0.16f - tint * 0.05f,
        1.0f + tint * 0.10f,
        1.0f - temperature * 0.16f - tint * 0.05f),
        0.05f);
    color *= exp2(exposure + ColorGradeSecondary.w) * whiteBalance;
    color = ACESFilm(color);

    const float luminance = Luminance(color);
    color = lerp(
        luminance.xxx,
        color,
        lerp(1.0f, max(ColorGradePrimary.z, 0.0f), gradingEnabled));
    color = (color - 0.5f)
        * lerp(1.0f, max(ColorGradePrimary.y, 0.0f), gradingEnabled)
        + 0.5f;

    const float2 centered = input.textureCoordinate * 2.0f - 1.0f;
    const float vignetteShape = saturate(
        1.0f - dot(centered, centered) * 0.42f);
    color *= lerp(
        1.0f,
        vignetteShape,
        lerp(
            0.0f,
            saturate(ColorGradeSecondary.y),
            gradingEnabled));
    // D3D11のPSToneMapと同じく、UNORMのバックバッファへgamma変換なしで
    // 書きます。Tone Mapping無効時の単純copyとも明るさの基準が揃います。
    return float4(saturate(color), 1.0f);
}

// LamaPonEnvironment.hlslのBrightColor / PSBloomと同じ9tapです。
// PassPrimary.xy=1/出力サイズ, z=しきい値, w=強さ, PassSecondary.x=半径。
float3 BrightColor(float2 uv)
{
    const float3 color = SpriteTexture.Sample(SpriteSampler, uv).rgb;
    const float brightness = max(
        color.r,
        max(color.g, color.b));
    return color * saturate(
        (brightness - PassPrimary.z)
        / max(brightness, 0.0001f));
}

float4 BloomPixelShader(PixelInput input) : SV_Target
{
    const float2 uv = input.textureCoordinate;
    const float4 source = SpriteTexture.Sample(SpriteSampler, uv);
    const float2 offset = PassPrimary.xy * PassSecondary.x;
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

// LamaPonEnvironment.hlslのPSFXAAと同じ式です。PassPrimary.xy=1/出力サイズ。
float4 FxaaPixelShader(PixelInput input) : SV_Target
{
    const float2 uv = input.textureCoordinate;
    const float2 texel = PassPrimary.xy;
    const float3 center = SpriteTexture.Sample(SpriteSampler, uv).rgb;
    const float lumaCenter = Luminance(center);
    const float lumaNorth = Luminance(
        SpriteTexture.Sample(
            SpriteSampler,
            uv + float2(0.0f, -texel.y)).rgb);
    const float lumaSouth = Luminance(
        SpriteTexture.Sample(
            SpriteSampler,
            uv + float2(0.0f, texel.y)).rgb);
    const float lumaWest = Luminance(
        SpriteTexture.Sample(
            SpriteSampler,
            uv + float2(-texel.x, 0.0f)).rgb);
    const float lumaEast = Luminance(
        SpriteTexture.Sample(
            SpriteSampler,
            uv + float2(texel.x, 0.0f)).rgb);
    const float lumaMinimum = min(
        lumaCenter,
        min(min(lumaNorth, lumaSouth), min(lumaWest, lumaEast)));
    const float lumaMaximum = max(
        lumaCenter,
        max(max(lumaNorth, lumaSouth), max(lumaWest, lumaEast)));
    if (lumaMaximum - lumaMinimum < 0.0312f)
    {
        return float4(center, 1.0f);
    }

    float2 direction = float2(
        -(lumaNorth - lumaSouth),
        lumaWest - lumaEast);
    const float reduction = max(
        (lumaNorth + lumaSouth + lumaWest + lumaEast)
            * 0.03125f,
        0.0078125f);
    const float inverseMinimum =
        1.0f / (min(abs(direction.x), abs(direction.y)) + reduction);
    direction = clamp(
        direction * inverseMinimum,
        -8.0f,
        8.0f) * texel;

    const float3 first =
        0.5f * (
            SpriteTexture.Sample(
                SpriteSampler,
                uv + direction * (1.0f / 3.0f - 0.5f)).rgb
            + SpriteTexture.Sample(
                SpriteSampler,
                uv + direction * (2.0f / 3.0f - 0.5f)).rgb);
    const float3 second =
        first * 0.5f
        + 0.25f * (
            SpriteTexture.Sample(
                SpriteSampler,
                uv + direction * -0.5f).rgb
            + SpriteTexture.Sample(
                SpriteSampler,
                uv + direction * 0.5f).rgb);
    const float secondLuma = Luminance(second);
    return float4(
        secondLuma < lumaMinimum || secondLuma > lumaMaximum
            ? first
            : second,
        1.0f);
}

// LamaPonEnvironment.hlslのPSLuminanceと同じく、1/4解像度の1画素が覆う
// 4x4を4回のbilinearで平均し、対数輝度を書きます。以降の段で平均すると
// 幾何平均輝度になります。PassPrimary.xy=1/出力サイズ。
float4 LuminancePixelShader(PixelInput input) : SV_Target
{
    const float2 uv = input.textureCoordinate;
    // 1/4解像度の1テクセルの1/4＝フル解像度の1テクセルぶん。
    const float2 offset = PassPrimary.xy * 0.25f;
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
    const float average = Luminance(max(total * 0.25f, 0.0f));
    return log(max(average, 1e-4f)).xxxx;
}

float4 TemporalPixelShader(PixelInput input) : SV_Target
{
    const float3 current = SpriteTexture.Sample(
        SpriteSampler,
        input.textureCoordinate).rgb;
    const float depth = DepthTexture.Sample(
        SpriteSampler,
        input.textureCoordinate).r;
    const float2 clip = float2(
        input.textureCoordinate.x * 2.0f - 1.0f,
        1.0f - input.textureCoordinate.y * 2.0f);
    const float4 worldHomogeneous = mul(
        float4(clip, depth, 1.0f),
        TemporalInverseViewProjection);
    if (worldHomogeneous.w <= 0.0001f)
    {
        return float4(current, 1.0f);
    }
    const float3 worldPosition =
        worldHomogeneous.xyz / worldHomogeneous.w;
    const float4 previousClip = mul(
        float4(worldPosition, 1.0f),
        TemporalPreviousViewProjection);
    if (previousClip.w <= 0.0001f)
    {
        return float4(current, 1.0f);
    }
    const float3 previousProjected = previousClip.xyz / previousClip.w;
    const float2 previousUv = float2(
        previousProjected.x * 0.5f + 0.5f,
        0.5f - previousProjected.y * 0.5f);
    if (previousUv.x < 0.0f || previousUv.x > 1.0f
        || previousUv.y < 0.0f || previousUv.y > 1.0f)
    {
        return float4(current, 1.0f);
    }

    const float2 texel = PassPrimary.zw;
    float3 minimumColor = current;
    float3 maximumColor = current;
    [unroll]
    for (int offsetY = -1; offsetY <= 1; ++offsetY)
    {
        [unroll]
        for (int offsetX = -1; offsetX <= 1; ++offsetX)
        {
            if (offsetX == 0 && offsetY == 0)
            {
                continue;
            }
            const float3 neighbour = SpriteTexture.Sample(
                SpriteSampler,
                input.textureCoordinate
                    + float2(offsetX, offsetY) * texel).rgb;
            minimumColor = min(minimumColor, neighbour);
            maximumColor = max(maximumColor, neighbour);
        }
    }
    const float3 middle = (minimumColor + maximumColor) * 0.5f;
    const float3 extent = (maximumColor - minimumColor)
        * 0.5f * max(PassPrimary.y, 0.0f);
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

float4 MotionBlurPixelShader(PixelInput input) : SV_Target
{
    const float4 source = SpriteTexture.Sample(
        SpriteSampler,
        input.textureCoordinate);
    const float depth = DepthTexture.Sample(
        SpriteSampler,
        input.textureCoordinate).r;
    const float2 clip = float2(
        input.textureCoordinate.x * 2.0f - 1.0f,
        1.0f - input.textureCoordinate.y * 2.0f);
    const float4 worldHomogeneous = mul(
        float4(clip, depth, 1.0f),
        TemporalInverseViewProjection);
    if (worldHomogeneous.w <= 0.0001f)
    {
        return source;
    }
    const float3 worldPosition =
        worldHomogeneous.xyz / worldHomogeneous.w;
    const float4 previousClip = mul(
        float4(worldPosition, 1.0f),
        TemporalPreviousViewProjection);
    if (previousClip.w <= 0.0001f)
    {
        return source;
    }
    const float2 previousUv = float2(
        previousClip.x / previousClip.w * 0.5f + 0.5f,
        0.5f - previousClip.y / previousClip.w * 0.5f);

    const float2 texel = float2(PassPrimary.w, PassSecondary.x);
    float2 velocity = (input.textureCoordinate - previousUv)
        * max(PassPrimary.x, 0.0f);
    const float2 velocityPixels = velocity / max(texel, 1e-6f);
    const float lengthPixels = length(velocityPixels);
    const float limitPixels = max(PassPrimary.y, 0.0f);
    if (lengthPixels < 0.5f || limitPixels <= 0.0f)
    {
        return source;
    }
    if (lengthPixels > limitPixels)
    {
        velocity *= limitPixels / lengthPixels;
    }

    const int sampleCount = clamp((int)PassPrimary.z, 2, 32);
    float3 total = source.rgb;
    float totalWeight = 1.0f;
    [loop]
    for (int index = 0; index < sampleCount; ++index)
    {
        const float offset =
            ((float)index + 0.5f) / (float)sampleCount - 0.5f;
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

float DepthOfFieldSceneDistance(float deviceDepth)
{
    const float denominator = deviceDepth + PassTertiary.x;
    return denominator > -1e-6f
        ? 1e6f
        : PassTertiary.y / denominator;
}

float DepthOfFieldSignedCircleOfConfusion(float viewDepth)
{
    const float focus = max(PassPrimary.x, 0.01f);
    const float halfRange = max(PassPrimary.y, 0.0f) * 0.5f;
    const float nearEdge = max(focus - halfRange, 0.01f);
    const float farEdge = focus + halfRange;
    float reference;
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
    const float relative = abs(
        1.0f - reference / max(viewDepth, 0.001f));
    return direction * saturate(relative * max(PassPrimary.z, 0.0f));
}

float DepthOfFieldNoise(float2 pixel)
{
    return frac(
        52.9829189f
        * frac(dot(pixel, float2(0.06711056f, 0.00583715f))));
}

float4 DepthOfFieldPixelShader(PixelInput input) : SV_Target
{
    const float4 sharp = SpriteTexture.Sample(
        SpriteSampler,
        input.textureCoordinate);
    const float centerDepth = DepthTexture.Sample(
        SpriteSampler,
        input.textureCoordinate).r;
    const float centerSignedCoc = DepthOfFieldSignedCircleOfConfusion(
        DepthOfFieldSceneDistance(centerDepth));
    const float centerCoc = abs(centerSignedCoc);
    const float maximumRadius = max(PassPrimary.w, 0.0f);
    const float mixAmount = saturate(centerCoc * maximumRadius);
    if (mixAmount <= 0.0f || maximumRadius <= 0.0f)
    {
        return sharp;
    }

    const int sampleCount = clamp((int)PassSecondary.x, 4, 64);
    const float2 texel = PassSecondary.yz;
    const float rotation = DepthOfFieldNoise(input.position.xy)
        * 6.28318531f;
    float3 total = sharp.rgb;
    float totalWeight = 1.0f;
    [loop]
    for (int index = 0; index < sampleCount; ++index)
    {
        const float angle = (float)index * 2.39996323f + rotation;
        const float radius = sqrt(
            ((float)index + 0.5f) / (float)sampleCount)
            * maximumRadius;
        const float2 uv = clamp(
            input.textureCoordinate
                + float2(cos(angle), sin(angle)) * radius * texel,
            0.0f,
            1.0f);
        const float tapDepth = DepthTexture.SampleLevel(
            SpriteSampler,
            uv,
            0.0f).r;
        const float tapSignedCoc = DepthOfFieldSignedCircleOfConfusion(
            DepthOfFieldSceneDistance(tapDepth));
        const float tapCoc = abs(tapSignedCoc);
        const float spread = tapSignedCoc < 0.0f
            ? tapCoc
            : min(tapCoc, centerCoc);
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

float OutlineSceneDistance(float deviceDepth)
{
    const float denominator = deviceDepth + PassTertiary.x;
    return denominator > -1e-6f
        ? 1e6f
        : PassTertiary.y / denominator;
}

int2 OutlineClampPixel(int2 pixel)
{
    const int2 size = max(int2(PassQuaternary.zw), int2(1, 1));
    return clamp(pixel, int2(0, 0), size - 1);
}

float3 OutlineViewPosition(int2 pixel)
{
    const int2 safePixel = OutlineClampPixel(pixel);
    const float depth = DepthTexture.Load(int3(safePixel, 0)).r;
    const float distance = OutlineSceneDistance(depth);
    const float2 uv = (float2(safePixel) + 0.5f) * PassQuaternary.xy;
    const float2 ndc = float2(
        uv.x * 2.0f - 1.0f,
        1.0f - uv.y * 2.0f);
    return float3(
        ndc.x * PassTertiary.z * distance,
        ndc.y * PassTertiary.w * distance,
        distance);
}

float3 OutlineNormal(int2 pixel)
{
    const int2 size = max(int2(PassQuaternary.zw), int2(3, 3));
    const int2 safePixel = clamp(
        pixel,
        int2(1, 1),
        max(size - 2, int2(1, 1)));
    const float3 origin = OutlineViewPosition(safePixel);
    const float3 left = OutlineViewPosition(safePixel + int2(-1, 0));
    const float3 right = OutlineViewPosition(safePixel + int2(1, 0));
    const float3 up = OutlineViewPosition(safePixel + int2(0, -1));
    const float3 down = OutlineViewPosition(safePixel + int2(0, 1));
    const float3 horizontal = abs(left.z - origin.z)
            < abs(right.z - origin.z)
        ? origin - left
        : right - origin;
    const float3 vertical = abs(up.z - origin.z)
            < abs(down.z - origin.z)
        ? up - origin
        : origin - down;
    const float3 normal = cross(vertical, horizontal);
    const float lengthSquared = dot(normal, normal);
    return lengthSquared < 1e-12f
        ? float3(0.0f, 0.0f, -1.0f)
        : normal * rsqrt(lengthSquared);
}

static const int2 OutlineDirections[8] = {
    int2(-1, -1), int2(0, -1), int2(1, -1), int2(-1, 0),
    int2(1, 0), int2(-1, 1), int2(0, 1), int2(1, 1)
};

float4 ScreenOutlinePixelShader(PixelInput input) : SV_Target
{
    const float4 source = SpriteTexture.Sample(
        SpriteSampler,
        input.textureCoordinate);
    const int2 screenSize = int2(PassQuaternary.zw);
    if (screenSize.x < 3 || screenSize.y < 3)
    {
        return source;
    }
    const int2 pixel = int2(input.position.xy);
    const float centerDistance = OutlineSceneDistance(
        DepthTexture.Load(int3(OutlineClampPixel(pixel), 0)).r);
    const float3 centerNormal = OutlineNormal(pixel);
    const int radius = clamp((int)PassSecondary.x, 1, 4);
    const float depthThreshold = max(PassSecondary.y, 0.0001f);
    const float normalThreshold = max(PassSecondary.z, 0.0001f);
    float depthEdge = 0.0f;
    float normalEdge = 0.0f;
    [unroll]
    for (int index = 0; index < 8; ++index)
    {
        const int2 samplePixel = pixel + OutlineDirections[index] * radius;
        const float sampleDistance = OutlineSceneDistance(
            DepthTexture.Load(
                int3(OutlineClampPixel(samplePixel), 0)).r);
        const bool centerIsSky = centerDistance >= 999999.0f;
        const bool sampleIsSky = sampleDistance >= 999999.0f;
        if (centerIsSky != sampleIsSky)
        {
            depthEdge = 1.0f;
        }
        else if (!centerIsSky)
        {
            const float relativeDifference = abs(
                sampleDistance - centerDistance)
                / max(centerDistance, 0.001f);
            depthEdge = max(
                depthEdge,
                smoothstep(
                    0.35f,
                    1.0f,
                    relativeDifference / depthThreshold));
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
    const float edge = saturate(
        max(depthEdge, normalEdge) * saturate(PassPrimary.w));
    return float4(
        lerp(source.rgb, PassPrimary.rgb, edge),
        source.a);
}

// LamaPonEnvironment.hlslのPSAmbientOcclusion / PSAmbientOcclusionBlurと
// 同じ式です。PassPrimaryは遮蔽textureの1 texel・探索半径・強さ、
// PassSecondary.xはサンプル数、PassTertiaryは深度から距離とビュー空間位置を
// 戻す射影値（xy=_33/_43、zw=1/_11・1/_22）です。
float AmbientOcclusionSceneDistance(float deviceDepth)
{
    const float denominator = deviceDepth + PassTertiary.x;
    if (denominator > -1e-6f)
    {
        return 1e6f;
    }
    return PassTertiary.y / denominator;
}

float3 AmbientOcclusionViewPosition(float2 uv, float deviceDepth)
{
    const float viewZ = AmbientOcclusionSceneDistance(deviceDepth);
    const float2 ndc = float2(
        uv.x * 2.0f - 1.0f,
        1.0f - uv.y * 2.0f);
    return float3(
        ndc.x * PassTertiary.z * viewZ,
        ndc.y * PassTertiary.w * viewZ,
        viewZ);
}

float3 AmbientOcclusionViewPositionAt(float2 uv)
{
    return AmbientOcclusionViewPosition(
        uv,
        DepthTexture.SampleLevel(SpriteSampler, uv, 0.0f).r);
}

// 上下左右のうち中心との段差が小さい側を軸ごとに選び、輪郭で手前と奥を
// またがない法線を作ります。
float3 AmbientOcclusionReconstructNormal(
    float2 uv,
    float3 origin,
    float2 texelSize)
{
    const float2 offsetX = float2(texelSize.x, 0.0f);
    const float2 offsetY = float2(0.0f, texelSize.y);
    const float3 left = AmbientOcclusionViewPositionAt(uv - offsetX);
    const float3 right = AmbientOcclusionViewPositionAt(uv + offsetX);
    const float3 up = AmbientOcclusionViewPositionAt(uv - offsetY);
    const float3 down = AmbientOcclusionViewPositionAt(uv + offsetY);
    const float3 horizontal = abs(left.z - origin.z)
            < abs(right.z - origin.z)
        ? origin - left
        : right - origin;
    const float3 vertical = abs(up.z - origin.z)
            < abs(down.z - origin.z)
        ? up - origin
        : origin - down;
    const float3 normal = cross(vertical, horizontal);
    const float lengthSquared = dot(normal, normal);
    if (lengthSquared < 1e-12f)
    {
        return float3(0.0f, 0.0f, -1.0f);
    }
    return normal * rsqrt(lengthSquared);
}

float AmbientOcclusionNoise(float2 pixel)
{
    return frac(
        52.9829189f
        * frac(dot(pixel, float2(0.06711056f, 0.00583715f))));
}

// 半解像度のRへ「残る明るさ」（1.0=遮蔽なし）を書きます。
float4 AmbientOcclusionPixelShader(PixelInput input) : SV_Target
{
    const float2 uv = input.textureCoordinate;
    const float depth = DepthTexture.SampleLevel(
        SpriteSampler,
        uv,
        0.0f).r;
    if (depth >= 0.999999f)
    {
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    const float3 origin = AmbientOcclusionViewPosition(uv, depth);
    const float2 texelSize = PassPrimary.xy;
    const float radius = PassPrimary.z;
    const float strength = PassPrimary.w;
    const float3 normal = AmbientOcclusionReconstructNormal(
        uv,
        origin,
        texelSize);
    const float projectedRadius = radius / max(origin.z, 0.001f);
    const float rotation = AmbientOcclusionNoise(
        uv / max(texelSize.x, 1e-6f)
            * float2(1.0f, texelSize.x / max(texelSize.y, 1e-6f)))
        * 6.2831853f;
    const int sampleCount = clamp(int(PassSecondary.x), 4, 32);
    float occlusion = 0.0f;
    [loop]
    for (int index = 0; index < sampleCount; ++index)
    {
        // 黄金角のらせんで、少ない回数でも偏らせません。
        const float fraction =
            (float(index) + 0.5f) / float(sampleCount);
        const float angle = rotation + fraction * 18.849556f;
        const float spiralDistance = sqrt(fraction);
        const float2 sampleUv = uv
            + float2(cos(angle), sin(angle))
                * spiralDistance * projectedRadius * 0.5f;
        if (any(sampleUv < 0.0f) || any(sampleUv > 1.0f))
        {
            continue;
        }
        const float sampleDepth = DepthTexture.SampleLevel(
            SpriteSampler,
            sampleUv,
            0.0f).r;
        if (sampleDepth >= 0.999999f)
        {
            continue;
        }
        float3 difference = AmbientOcclusionViewPosition(
            sampleUv,
            sampleDepth) - origin;
        const float length2 = dot(difference, difference);
        if (length2 < 1e-8f)
        {
            continue;
        }
        difference *= rsqrt(length2);
        const float sampleDistance = sqrt(length2);
        // 法線より手前側にある分だけを遮蔽とし、半径の外は効かせません。
        const float facing = saturate(dot(normal, difference) - 0.06f);
        const float falloff = saturate(
            1.0f - sampleDistance / max(radius, 0.001f));
        occlusion += facing * falloff;
    }
    occlusion = saturate(
        occlusion / float(sampleCount) * 2.4f * strength);
    const float visibility = 1.0f - occlusion;
    return float4(visibility, visibility, visibility, 1.0f);
}

// 中心と深度の近い画素だけを混ぜるbilateral blurで、少ないサンプルの
// ザラつきを輪郭を越えずに均します。
float4 AmbientOcclusionBlurPixelShader(PixelInput input) : SV_Target
{
    const float2 uv = input.textureCoordinate;
    const float2 texelSize = PassPrimary.xy;
    const float centerDepth = AmbientOcclusionSceneDistance(
        DepthTexture.SampleLevel(SpriteSampler, uv, 0.0f).r);
    const float depthScale = max(centerDepth * 0.08f, 0.05f);
    float total = 0.0f;
    float weightSum = 0.0f;
    [unroll]
    for (int y = -2; y <= 1; ++y)
    {
        [unroll]
        for (int x = -2; x <= 1; ++x)
        {
            const float2 sampleUv = uv + float2(
                (float(x) + 0.5f) * texelSize.x,
                (float(y) + 0.5f) * texelSize.y);
            const float sampleDepth = AmbientOcclusionSceneDistance(
                DepthTexture.SampleLevel(
                    SpriteSampler,
                    sampleUv,
                    0.0f).r);
            const float depthRatio =
                (sampleDepth - centerDepth) / depthScale;
            const float weight =
                1.0f / (1.0f + depthRatio * depthRatio);
            total += SpriteTexture.SampleLevel(
                SpriteSampler,
                sampleUv,
                0.0f).r * weight;
            weightSum += weight;
        }
    }
    const float visibility = weightSum > 0.0f
        ? total / weightSum
        : SpriteTexture.SampleLevel(SpriteSampler, uv, 0.0f).r;
    return float4(visibility, visibility, visibility, 1.0f);
}

static const float VolumetricPi = 3.14159265f;

float VolumetricPhase(float cosineAngle, float scattering)
{
    const float g = clamp(scattering, 0.0f, 0.95f);
    const float gSquared = g * g;
    const float denominator =
        1.0f + gSquared - 2.0f * g * cosineAngle;
    return (1.0f - gSquared)
        / (4.0f * VolumetricPi
            * pow(max(denominator, 0.0001f), 1.5f));
}

float VolumetricShadowAt(float3 worldPosition)
{
    const int cascadeCount = (int)VolumetricShadowParameters.x;
    [loop]
    for (int cascade = 0; cascade < 4; ++cascade)
    {
        if (cascade >= cascadeCount)
        {
            break;
        }
        const float4 lightPosition = mul(
            float4(worldPosition, 1.0f),
            VolumetricCascades[cascade]);
        if (lightPosition.w <= 0.0001f)
        {
            continue;
        }
        const float3 projected = lightPosition.xyz / lightPosition.w;
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

float4 VolumetricLightPixelShader(PixelInput input) : SV_Target
{
    const float4 sceneColor = SpriteTexture.Sample(
        SpriteSampler,
        input.textureCoordinate);
    const float depth = DepthTexture.Sample(
        SpriteSampler,
        input.textureCoordinate).r;
    const float2 clip = float2(
        input.textureCoordinate.x * 2.0f - 1.0f,
        1.0f - input.textureCoordinate.y * 2.0f);
    const float4 worldHomogeneous = mul(
        float4(clip, depth, 1.0f),
        VolumetricInverseViewProjection);
    if (worldHomogeneous.w <= 0.0001f)
    {
        return sceneColor;
    }
    const float3 worldPosition =
        worldHomogeneous.xyz / worldHomogeneous.w;
    const float3 cameraPosition = VolumetricCameraPosition.xyz;
    const float3 toPixel = worldPosition - cameraPosition;
    const float pixelDistance = length(toPixel);
    if (pixelDistance <= 0.0001f)
    {
        return sceneColor;
    }
    const float3 rayDirection = toPixel / pixelDistance;
    const float marchDistance = min(
        pixelDistance,
        VolumetricCameraPosition.w);
    const int sampleCount = (int)max(VolumetricLightDirection.w, 1.0f);
    const float stepLength = marchDistance / (float)sampleCount;
    const float dither = frac(
        52.9829189f
        * frac(dot(
            input.position.xy,
            float2(0.06711056f, 0.00583715f))));
    float travelled = stepLength * (0.5f + dither * 0.5f);
    const float cosineAngle = dot(
        rayDirection,
        -normalize(VolumetricLightDirection.xyz));
    const float phase = VolumetricPhase(
        cosineAngle,
        VolumetricLightColor.w);
    float accumulated = 0.0f;
    [loop]
    for (int step = 0; step < sampleCount; ++step)
    {
        const float3 samplePosition =
            cameraPosition + rayDirection * travelled;
        accumulated += VolumetricShadowAt(samplePosition);
        travelled += stepLength;
    }
    const float visibility = accumulated / (float)sampleCount;
    const float3 scatter = VolumetricLightColor.rgb
        * visibility
        * phase
        * (marchDistance / max(VolumetricCameraPosition.w, 0.0001f));
    return float4(sceneColor.rgb + max(scatter, 0.0f), sceneColor.a);
}

// LamaPonEnvironment.hlslのScreen Space Lens Flareと同じ処理です。
// PassPrimary=texel/threshold/intensity、PassSecondary=ghost/halo/
// chromatic/streak intensity、PassTertiary.x=streak length、
// PassQuaternary=stride/directions/angle/first passです。
float3 LensFlareBright(float2 uv)
{
    if (any(uv < 0.0f) || any(uv > 1.0f))
    {
        return 0.0f;
    }
    const float3 color = SpriteTexture.Sample(SpriteSampler, uv).rgb;
    const float brightness = max(color.r, max(color.g, color.b));
    const float threshold = max(PassPrimary.z, 0.0f);
    const float gate = smoothstep(
        threshold,
        threshold + max(threshold * 0.35f, 0.25f),
        brightness);
    return color * gate;
}

float3 LensFlareChromaticSample(float2 uv, float2 direction)
{
    const float chromatic = saturate(PassSecondary.z) * 0.015f;
    const float2 offset = direction * chromatic;
    const float3 red = LensFlareBright(uv + offset);
    const float3 green = LensFlareBright(uv);
    const float3 blue = LensFlareBright(uv - offset);
    return float3(red.r, green.g, blue.b);
}

float4 LensFlareStreakPixelShader(PixelInput input) : SV_Target
{
    const float stride = PassQuaternary.x;
    const int directionCount = clamp((int)PassQuaternary.y, 1, 4);
    const float baseAngle = PassQuaternary.z;
    const bool firstPass = PassQuaternary.w > 0.5f;
    float3 total = 0.0f;
    float weightTotal = 0.0f;
    [loop]
    for (int index = 0; index < directionCount; ++index)
    {
        const float angle = baseAngle
            + 3.14159265f * (float)index / (float)directionCount;
        const float2 axis = float2(cos(angle), sin(angle));
        [unroll]
        for (int tap = -2; tap <= 2; ++tap)
        {
            const float2 uv = clamp(
                input.textureCoordinate + axis * ((float)tap * stride),
                0.0f,
                1.0f);
            const float weight = 1.0f - abs((float)tap) * 0.22f;
            const float dispersion = saturate(PassSecondary.z);
            const float shift = (float)tap / 2.0f * dispersion;
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

float4 LensFlareCompositePixelShader(PixelInput input) : SV_Target
{
    const float2 uv = input.textureCoordinate;
    const float4 source = SpriteTexture.Sample(SpriteSampler, uv);
    const float2 center = float2(0.5f, 0.5f);
    const float2 fromCenter = uv - center;
    const float radius = length(fromCenter);
    const float2 direction = radius > 0.0001f
        ? fromCenter / radius
        : float2(1.0f, 0.0f);
    float3 flare = LensFlareBright(uv) * 0.22f;
    const float dispersal = max(PassSecondary.x, 0.01f);
    [unroll]
    for (int index = 1; index <= 4; ++index)
    {
        const float scale = dispersal * (float)index;
        const float2 ghostUv = center - fromCenter * scale;
        const float ghostWeight = 0.23f - (float)index * 0.025f;
        flare += LensFlareChromaticSample(ghostUv, direction)
            * max(ghostWeight, 0.05f);
    }
    const float haloRadius = clamp(PassSecondary.y, 0.05f, 1.5f);
    const float haloDistance = abs(radius - haloRadius);
    const float halo = 1.0f - smoothstep(
        0.015f,
        0.10f + haloRadius * 0.18f,
        haloDistance);
    const float2 haloUv = center - direction * haloRadius;
    flare += LensFlareChromaticSample(haloUv, direction) * halo * 0.32f;
    const float3 streak = TemporalHistoryTexture.SampleLevel(
        SpriteSampler,
        uv,
        0.0f).rgb;
    flare += streak * PassSecondary.w;
    return float4(
        source.rgb + flare * max(PassPrimary.w, 0.0f),
        source.a);
}

// LamaPonEnvironment.hlslのPSReflectionDepthLinearize /
// PSReflectionDepthDownsampleと同じSSRのHi-Z深度ピラミッドです。
// mip 0はPassPrimary.xy=射影の_33/_43で深度をカメラからの距離へ直し、
// 空（遠平面）は十分遠い値にします。
float4 ReflectionDepthLinearizePixelShader(PixelInput input) : SV_Target
{
    const int2 pixel = int2(input.position.xy);
    const float deviceDepth = SpriteTexture.Load(int3(pixel, 0)).r;
    const float denominator = deviceDepth + PassPrimary.x;
    if (denominator > -1e-6f)
    {
        return float4(1e6f, 1e6f, 1e6f, 1e6f);
    }
    const float sceneDistance = PassPrimary.y / denominator;
    return float4(
        sceneDistance,
        sceneDistance,
        sceneDistance,
        sceneDistance);
}

// mip N+1は親ミップの2x2の最小値です。PassPrimary.xyは親ミップの
// 大きさで、辺が奇数のときは端の子が余った列（行）も読み、最も手前の
// 面を取りこぼしません。
float4 ReflectionDepthDownsamplePixelShader(PixelInput input) : SV_Target
{
    const int2 parentSize = int2(PassPrimary.xy);
    const int2 parent = int2(input.position.xy) * 2;
    const int2 last = parentSize - 1;
    const float a = SpriteTexture.Load(int3(min(parent, last), 0)).r;
    const float b = SpriteTexture.Load(
        int3(min(parent + int2(1, 0), last), 0)).r;
    const float c = SpriteTexture.Load(
        int3(min(parent + int2(0, 1), last), 0)).r;
    const float d = SpriteTexture.Load(
        int3(min(parent + int2(1, 1), last), 0)).r;
    float nearest = min(min(a, b), min(c, d));
    const bool oddWidth = (parentSize.x & 1) != 0;
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

    [[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob> CompileSpriteShader(
        const char* const entryPoint,
        const char* const target)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        const HRESULT result = D3DCompile(
            SpriteShaderSource,
            sizeof(SpriteShaderSource) - 1u,
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

    [[nodiscard]] D3D12_BLEND_DESC MakeBlendDescription(
        const LamaPon::SpriteBlendMode blend)
    {
        // DirectXTK CommonStatesと同じ係数を、色とalphaの両方へ使います。
        D3D12_BLEND source = D3D12_BLEND_ONE;
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

        D3D12_BLEND_DESC description{};
        description.AlphaToCoverageEnable = FALSE;
        description.IndependentBlendEnable = FALSE;
        for (auto& renderTarget : description.RenderTarget)
        {
            renderTarget = target;
        }
        return description;
    }

    [[nodiscard]] D3D12_RASTERIZER_DESC MakeRasterizerDescription(
        const bool scissored) noexcept
    {
        // 通常passはSpriteBatch既定の反時計回りcull、scissor passはD3D11の
        // UI clipping rasterizerと同じくcullしません。
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

    [[nodiscard]] D3D12_DEPTH_STENCIL_DESC
        MakeDepthStencilDescription() noexcept
    {
        const D3D12_DEPTH_STENCILOP_DESC keep{
            D3D12_STENCIL_OP_KEEP,
            D3D12_STENCIL_OP_KEEP,
            D3D12_STENCIL_OP_KEEP,
            D3D12_COMPARISON_FUNC_ALWAYS
        };
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

    [[nodiscard]] bool IsFinite(
        const DirectX::XMFLOAT2& value) noexcept
    {
        return std::isfinite(value.x)
            && std::isfinite(value.y);
    }

    [[nodiscard]] bool IsFinite(
        const DirectX::XMFLOAT4& value) noexcept
    {
        return std::isfinite(value.x)
            && std::isfinite(value.y)
            && std::isfinite(value.z)
            && std::isfinite(value.w);
    }

    [[nodiscard]] bool IsFinite(
        const LamaPon::SpriteClipRectangle& value) noexcept
    {
        return std::isfinite(value.minimumX)
            && std::isfinite(value.minimumY)
            && std::isfinite(value.maximumX)
            && std::isfinite(value.maximumY);
    }

    // D3D11のUI scissorと同じく、外側のclipとの交差へ畳み込みます。
    [[nodiscard]] D3D12_RECT MakeScissorRectangle(
        const LamaPon::SpriteClipRectangle& rectangle,
        const std::vector<D3D12_RECT>& stack)
    {
        const auto clampLong = [](const float value) noexcept
        {
            return static_cast<LONG>(
                std::clamp(
                    static_cast<double>(value),
                    0.0,
                    static_cast<double>(
                        (std::numeric_limits<LONG>::max)())));
        };
        D3D12_RECT result{
            clampLong(rectangle.minimumX),
            clampLong(rectangle.minimumY),
            clampLong(rectangle.maximumX),
            clampLong(rectangle.maximumY) };
        if (!stack.empty())
        {
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

    // EnvironmentRenderer::ApplyBloom / ApplyFXAAと同じく、target全体の
    // 1 texel寸法をUV単位で求めます。
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

        D3D12_DESCRIPTOR_RANGE textureRange{};
        textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        textureRange.NumDescriptors = 1;
        textureRange.BaseShaderRegister = 0;
        textureRange.RegisterSpace = 0;
        textureRange.OffsetInDescriptorsFromTableStart = 0;

        D3D12_DESCRIPTOR_RANGE historyRange = textureRange;
        historyRange.BaseShaderRegister = 1;
        D3D12_DESCRIPTOR_RANGE depthRange = textureRange;
        depthRange.BaseShaderRegister = 2;
        D3D12_DESCRIPTOR_RANGE shadowRange = textureRange;
        shadowRange.BaseShaderRegister = 3;

        std::array<D3D12_ROOT_PARAMETER, 8> parameters{};
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
        parameters[2].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[2].Constants.ShaderRegister = 1;
        parameters[2].Constants.RegisterSpace = 0;
        parameters[2].Constants.Num32BitValues = 16;
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

        // SpriteBatch既定のLinearClampと、D3D11 Volumetric Lightと同じ
        // 範囲外を照射済みとする比較samplerです。
        std::array<D3D12_STATIC_SAMPLER_DESC, 2> samplers{};
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

        D3D12_ROOT_SIGNATURE_DESC rootDescription{};
        rootDescription.NumParameters =
            static_cast<UINT>(parameters.size());
        rootDescription.pParameters = parameters.data();
        rootDescription.NumStaticSamplers =
            static_cast<UINT>(samplers.size());
        rootDescription.pStaticSamplers = samplers.data();
        rootDescription.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        Microsoft::WRL::ComPtr<ID3DBlob> serializedRoot;
        Microsoft::WRL::ComPtr<ID3DBlob> rootErrors;
        const HRESULT serialized = D3D12SerializeRootSignature(
            &rootDescription,
            D3D_ROOT_SIGNATURE_VERSION_1,
            serializedRoot.GetAddressOf(),
            rootErrors.GetAddressOf());
        if (FAILED(serialized))
        {
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

        const auto indexBytes =
            MaximumSpritesPerDraw * IndicesPerSprite
            * sizeof(std::uint16_t);
        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        uploadHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        uploadHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        uploadHeap.CreationNodeMask = 1;
        uploadHeap.VisibleNodeMask = 1;
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
        void* mapped{};
        const D3D12_RANGE noRead{};
        ThrowIfFailed(
            m_indexBuffer->Map(0, &noRead, &mapped),
            "ID3D12Resource::Map(sprite indices)");
        auto* const indices = static_cast<std::uint16_t*>(mapped);
        for (std::size_t sprite{}; sprite < MaximumSpritesPerDraw; ++sprite)
        {
            // SpriteBatchと同じ三角形の並びです。
            const auto first =
                static_cast<std::uint16_t>(sprite * VerticesPerSprite);
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

        SpriteShaderStatus preparedStatus;
        if (!description.pixelShader.empty())
        {
            // D3D11でcompileに失敗した場合と同じく、既定pipelineで描いて
            // 呼び出し側へ理由を返します。
            preparedStatus.fallback =
                SpriteShaderFallback::DefaultPipeline;
            preparedStatus.error =
                "Custom sprite pixel shaders are not implemented for "
                "DirectX 12 Experimental.";
        }
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

        const auto& view = request.texture
            ? request.texture
            : m_fallbackTexture;
        const auto binding = m_backend->TryResolveShaderResource(view);
        if (!binding)
        {
            return false;
        }

        // DirectXTK SpriteBatchと同じ演算順で、source rectangleの有無ごとに
        // 正規化UV、原点、表示寸法を求めます。
        const float textureWidth = static_cast<float>(binding->width);
        const float textureHeight = static_cast<float>(binding->height);
        const float inverseTextureWidth = 1.0f / textureWidth;
        const float inverseTextureHeight = 1.0f / textureHeight;
        float sourceX = 0.0f;
        float sourceY = 0.0f;
        float sourceWidth = 1.0f;
        float sourceHeight = 1.0f;
        float originX{};
        float originY{};
        float destinationWidth{};
        float destinationHeight{};
        if (request.hasSourceRectangle)
        {
            const auto left =
                static_cast<float>(request.sourceRectangle.left);
            const auto top =
                static_cast<float>(request.sourceRectangle.top);
            const float texelWidth =
                static_cast<float>(request.sourceRectangle.right) - left;
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

        float rotationSin = 0.0f;
        float rotationCos = 1.0f;
        const bool rotated = request.rotation != 0.0f;
        if (rotated)
        {
            DirectX::XMScalarSinCos(
                &rotationSin,
                &rotationCos,
                request.rotation);
        }

        // flipはcorner表のindexをbitで入れ替え、頂点位置は変えずにUVだけを
        // 反転します（Horizontal=1、Vertical=2）。
        static constexpr std::array<DirectX::XMFLOAT2, VerticesPerSprite>
            CornerOffsets{ {
                { 0.0f, 0.0f },
                { 1.0f, 0.0f },
                { 0.0f, 1.0f },
                { 1.0f, 1.0f }
            } };
        const auto mirrorBits =
            static_cast<std::size_t>(request.flip) & 3u;

        QueuedSprite sprite;
        sprite.texture = binding->descriptor;
        sprite.view = view;
        for (std::size_t corner{}; corner < VerticesPerSprite; ++corner)
        {
            const float cornerX =
                (CornerOffsets[corner].x - originX) * destinationWidth;
            const float cornerY =
                (CornerOffsets[corner].y - originY) * destinationHeight;
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
        auto nextScissors = m_scissorStack;
        nextScissors.push_back(
            MakeScissorRectangle(rectangle, nextScissors));
        // D3D11と同じく、それまでのSpriteを現在のclipで確定してから
        // 次のclipへ切り替えます。
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
        // EnvironmentRenderer::ApplyBloomと同じ範囲へ丸めます。
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
        const auto texel = TexelSize(target);
        const float longest = std::clamp(settings.streakLength, 0.0f, 1.0f);
        const float baseStride = longest / 42.0f;
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
            for (std::uint32_t pass{}; pass < 3u; ++pass)
            {
                const auto source =
                    m_backend->BeginOffscreenLensFlareStreakPass(
                        target,
                        pass);
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
        const auto streak =
            m_backend->EndOffscreenLensFlareStreaks(target);
        const auto source = m_backend->BeginOffscreenPostProcess(target);
        try
        {
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
        const auto* const state =
            RenderTargetBackendAccess::Get(target);
        const auto history = target.TemporalHistoryViewHandle();
        const auto depth = target.DepthViewHandle();
        if (state == nullptr
            || !state->m_temporalHistoryValid
            || !m_backend->TryResolveShaderResource(history)
            || !m_backend->TryResolveShaderResource(depth))
        {
            return;
        }

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
        const auto texel = TexelSize(target);
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
        const auto depth = target.DepthViewHandle();
        if (!m_backend->TryResolveShaderResource(depth))
        {
            return;
        }
        const auto texel = TexelSize(target);
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
        const auto depth = target.DepthViewHandle();
        if (!m_backend->TryResolveShaderResource(depth))
        {
            return;
        }
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
        const auto texel = TexelSize(target);
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
        const auto depth = target.DepthViewHandle();
        if (!m_backend->TryResolveShaderResource(depth))
        {
            return;
        }
        const auto texel = TexelSize(target);
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
        const auto depth = target.DepthViewHandle();
        if (!m_backend->TryResolveShaderResource(depth))
        {
            return false;
        }
        try
        {
            const auto source =
                m_backend->BeginOffscreenAmbientOcclusionPass(target, false);
            const auto& viewport = m_backend->ActiveViewport();
            // D3D11のRenderAmbientOcclusionと同じく1 texelは遮蔽textureの
            // 寸法で、ブラーも同じ定数を読みます。
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
        const auto mipCount = target.ReflectionDepthPyramidMipCount();
        if (mipCount == 0u
            || !m_backend->TryResolveShaderResource(target.DepthViewHandle())
            || !m_backend->TryResolveShaderResource(
                target.ReflectionDepthPyramidViewHandle()))
        {
            return false;
        }
        const bool depthOnly =
            m_backend->IsOffscreenTargetBoundDepthOnly(target);
        const std::uint32_t width = std::max(target.Width(), 1u);
        const std::uint32_t height = std::max(target.Height(), 1u);
        try
        {
            for (std::uint32_t mip{}; mip < mipCount; ++mip)
            {
                const auto source =
                    m_backend->BeginOffscreenReflectionDepthPass(target, mip);
                // mip 0は射影の_33/_43、以降は1段細かい親ミップの
                // 大きさです。
                const float parameterX = mip == 0u
                    ? projectionZ
                    : static_cast<float>(std::max(width >> (mip - 1u), 1u));
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

    void D3D12SpriteRenderer::MeasureLuminance(
        RenderTarget& target,
        const GraphicsViewHandle& fallbackTexture)
    {
        const auto levelCount =
            m_backend->OffscreenLuminanceLevelCount(target);
        try
        {
            for (std::uint32_t level{}; level < levelCount; ++level)
            {
                const auto source =
                    m_backend->BeginOffscreenLuminancePass(target, level);
                const auto& viewport = m_backend->ActiveViewport();
                // 1段目はPSLuminance、以降はGenerateMipsと同じく前段の
                // 2x2をbilinear 1回で平均します。
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

    void D3D12SpriteRenderer::ApplyPostProcessPass(
        RenderTarget& target,
        const GraphicsViewHandle& fallbackTexture,
        const FullscreenProgram program,
        const std::array<float, 16>& constants)
    {
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
        const auto binding = m_backend->TryResolveShaderResource(texture);
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
        SpritePassDescription description;
        description.blend = SpriteBlendMode::Opaque;
        SpriteShaderStatus status;
        const auto token = Begin(
            description,
            fallbackTexture,
            status);
        m_program = program;
        m_passConstants = constants;
        m_matrixConstants = matrixConstants;
        m_auxiliaryViews = auxiliaryViews;
        if (program == FullscreenProgram::Temporal)
        {
            for (std::size_t index{}; index < 2u; ++index)
            {
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
            const auto depth = m_backend->TryResolveShaderResource(
                m_auxiliaryViews[1]);
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
        else if (program == FullscreenProgram::ScreenOutline
            || program == FullscreenProgram::MotionBlur
            || program == FullscreenProgram::DepthOfField
            || program == FullscreenProgram::AmbientOcclusion
            || program == FullscreenProgram::AmbientOcclusionBlur)
        {
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
        for (auto& view : m_auxiliaryViews)
        {
            view.Reset();
        }
        m_auxiliaryTextures = {};
        m_matrixConstants = {};
        m_volumetricConstants = {};
        m_program = FullscreenProgram::None;
        m_activeToken = 0;
    }

    void D3D12SpriteRenderer::Flush()
    {
        if (m_sprites.empty())
        {
            return;
        }

        auto* const commandList = m_backend->BeginFrameCommands();
        auto* const descriptorHeap =
            m_backend->ShaderResourceDescriptorHeap();
        if (descriptorHeap == nullptr)
        {
            throw std::logic_error(
                "The DirectX 12 sprite renderer requires a shader resource "
                "descriptor heap.");
        }
        const std::uint64_t vertexBytes =
            static_cast<std::uint64_t>(m_sprites.size())
            * sizeof(QueuedSprite::vertices);
        if (vertexBytes > std::numeric_limits<UINT>::max())
        {
            throw std::length_error(
                "The DirectX 12 sprite batch is too large.");
        }
        auto* const pipelineState = PipelineState(
            m_blend,
            !m_scissorStack.empty(),
            m_backend->ActiveColorFormat(),
            m_backend->ActiveDepthFormat(),
            m_program);

        const auto upload = m_backend->AllocateFrameUpload(
            vertexBytes,
            alignof(Vertex));
        auto* vertexData = upload.data;
        for (const auto& sprite : m_sprites)
        {
            std::memcpy(
                vertexData,
                sprite.vertices.data(),
                sizeof(sprite.vertices));
            vertexData += sizeof(sprite.vertices);
        }

        const auto& viewport = m_backend->ActiveViewport();
        const std::array<float, 2> viewportScale{
            viewport.Width > 0.0f ? 2.0f / viewport.Width : 0.0f,
            viewport.Height > 0.0f ? 2.0f / viewport.Height : 0.0f
        };
        const D3D12_RECT scissor = m_scissorStack.empty()
            ? m_backend->ActiveScissorRectangle()
            : m_scissorStack.back();
        const D3D12_VERTEX_BUFFER_VIEW vertexBufferView{
            upload.gpuAddress,
            static_cast<UINT>(vertexBytes),
            static_cast<UINT>(sizeof(Vertex))
        };
        const D3D12_INDEX_BUFFER_VIEW indexBufferView{
            m_indexBuffer->GetGPUVirtualAddress(),
            static_cast<UINT>(
                MaximumSpritesPerDraw * IndicesPerSprite
                * sizeof(std::uint16_t)),
            DXGI_FORMAT_R16_UINT
        };
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
        if (m_program != FullscreenProgram::None)
        {
            commandList->SetGraphicsRoot32BitConstants(
                2,
                static_cast<UINT>(m_passConstants.size()),
                m_passConstants.data(),
                0);
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
        commandList->IASetPrimitiveTopology(
            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList->IASetVertexBuffers(0, 1, &vertexBufferView);
        commandList->IASetIndexBuffer(&indexBufferView);
        commandList->RSSetScissorRects(1, &scissor);

        // Deferred順を保ったまま、同じtextureが連続する範囲だけをまとめます。
        std::size_t first{};
        while (first < m_sprites.size())
        {
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
            commandList->DrawIndexedInstanced(
                static_cast<UINT>((end - first) * IndicesPerSprite),
                1,
                0,
                static_cast<INT>(first * VerticesPerSprite),
                0);
            first = end;
        }
        // 後続の描画へSprite用のclipを残しません。
        const auto& fullScissor = m_backend->ActiveScissorRectangle();
        commandList->RSSetScissorRects(1, &fullScissor);
        m_sprites.clear();
    }

    ID3D12PipelineState* D3D12SpriteRenderer::PipelineState(
        const SpriteBlendMode blend,
        const bool scissored,
        const DXGI_FORMAT colorFormat,
        const DXGI_FORMAT depthFormat,
        const FullscreenProgram program)
    {
        const auto blendIndex = static_cast<std::size_t>(blend);
        if (blend > SpriteBlendMode::Opaque)
        {
            throw std::invalid_argument(
                "The sprite blend mode is invalid.");
        }
        if (program > FullscreenProgram::ReflectionDepthDownsample)
        {
            throw std::invalid_argument(
                "The sprite pixel program is invalid.");
        }
        // SSAOの遮蔽とブラーは半解像度のR8へ、SSRのHi-Z深度ピラミッドは
        // R32Fへ書きます。
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
        // 自動露出の縮小段とSSAOは深度bufferを持たないため、DSV無しの
        // PSOを別に作ります。
        const std::size_t depthIndex = depthFormat
                == D3D12Backend::PrimaryDepthFormat
            ? 0u
            : depthFormat == DXGI_FORMAT_UNKNOWN
                ? 1u
                : throw std::invalid_argument(
                    "The active DirectX 12 sprite depth format is "
                    "unsupported.");
        auto& pipeline = m_pipelineStates[
            ((static_cast<std::size_t>(program) * DepthFormatVariants
                + depthIndex)
                * ColorFormatVariants
                + formatIndex)
                * BlendVariants
            + blendIndex * 2u
            + (scissored ? 1u : 0u)];
        if (pipeline != nullptr)
        {
            return pipeline.Get();
        }

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

        ID3DBlob* pixelShader = m_pixelShader.Get();
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
        case FullscreenProgram::None:
            break;
        }
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
        // Primary outputには深度bufferもbindされるため、深度testを使わない
        // Spriteでもformatだけは一致させます。
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
