#include "LamaPon/Graphics/D3D12RenderServices.h"

#include "LamaPon/Graphics/ClusteredLights.h"
#include "LamaPon/Graphics/D3D12EnvironmentPrefilter.h"
#include "LamaPon/Graphics/D3D12Backend.h"
#include "LamaPon/Graphics/GraphicsRenderServices.h"
#include "LamaPon/Graphics/ShaderCompiler.h"
#include "LamaPon/Graphics/SpriteRendering.h"
#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/PathUtils.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    using LamaPon::PrimitiveRenderVertex;

    constexpr char PrimitiveShaderSource[] = R"(
cbuffer PrimitiveConstants : register(b0)
{
    row_major float4x4 World;
    float4 BaseColor;
    float4 CameraPosition;
    float4 MaterialProperties;
    float4 EmissiveFactor;
    float4 AmbientColorIntensity;
    float4 DirectionalDirectionIntensity[4];
    float4 DirectionalColors[4];
    uint4 LightCounts;
    float4 PointPositionRange[16];
    float4 PointColorIntensity[16];
    float4 SpotPositionRange[8];
    float4 SpotDirectionInnerCosine[8];
    float4 SpotColorIntensity[8];
    float4 SpotOuterCosine[8];
    row_major float4x4 ShadowViewProjections[4];
    float4 ShadowCascadeSplits;
    // x=shadow light index + 1, y=depth bias,
    // z=normal bias, w=strength.
    float4 ShadowParameters;
    // xyz=camera forward, w=1 / shadow resolution.
    float4 CameraForwardShadowTexel;
    row_major float4x4 SpotShadowViewProjections[4];
    // x=depth bias, y=normal bias, z=strength, w=enabled.
    float4 SpotShadowParameters[4];
    // x=target point light + 1, y=depth bias, z=strength, w=reserved.
    float4 PointShadowParameters;
    // x=1 / spot shadow resolution, y=1 / point shadow resolution.
    float4 LocalShadowTexelSizes;
    // xy=1 / SSAO target size, z=enabled, w=reserved.
    float4 ScreenAmbientOcclusionParameters;
    // 頂点変換とSSRのレイの投影に使うビュー射影です。
    row_major float4x4 ViewProjection;
    // SSR. x=strength, y=enabled, z=maximum distance, w=maximum steps.
    float4 ScreenReflectionParameters;
    // xy=1 / target size, zw=reserved.
    float4 ScreenReflectionScreen;
    // x=thickness, y=roughness cutoff, z=last Hi-Z mip, w=reserved.
    float4 ScreenReflectionQuality;
    row_major float4x4 ScreenReflectionPreviousViewProjection;
    // IBL. x=intensity, y=enabled, z=last prefiltered specular mip,
    // w=reserved.
    float4 EnvironmentParameters;
    // Fog. rgb=color, w=0 for the LitEffect range/exponential fog or 1 for
    // the DirectXTK linear view-depth fog.
    float4 FogColorModel;
    // x=start, y=end, z=density, w=enabled.
    float4 FogParameters;
    // 法線用の逆転置行列です（非一様スケールでも面に垂直なまま）。
    row_major float4x4 WorldInverseTranspose;
    // Forward+. xyz=grid size, w=enabled.
    float4 ClusteredParameters;
    // x=near, y=far, z=log(far/near), w=lights per cluster.
    float4 ClusteredDepthParameters;
    // xy=1 / target size, z=light count, w=reserved.
    float4 ClusteredScreenParameters;
    // Baked GI. xyz=volume minimum corner, w=enabled.
    float4 BakedGiVolumeMinimum;
    // xyz=1 / volume size, w=intensity.
    float4 BakedGiInverseSize;
    // xyz=probe count per axis, w=reserved.
    float4 BakedGiResolution;
    // Reflection probe box projection. xyz=box center, w=reserved.
    float4 ReflectionBoxCenter;
    // xyz=box half extents, w=enabled.
    float4 ReflectionBoxParameters;
    // The same box projection for the blended second probe.
    float4 ReflectionSecondaryBoxCenter;
    float4 ReflectionSecondaryBoxParameters;
    // x=second probe weight (0 disables blending), y=its last specular mip.
    float4 ReflectionBlendParameters;
};

Texture2D AlbedoTexture : register(t0);
Texture2D NormalTexture : register(t1);
Texture2D RoughnessTexture : register(t2);
Texture2D MetallicTexture : register(t3);
Texture2D OcclusionTexture : register(t4);
Texture2D EmissiveTexture : register(t5);
Texture2DArray<float> DirectionalShadowTexture : register(t6);
Texture2DArray<float> SpotShadowTexture : register(t7);
TextureCube<float> PointShadowTexture : register(t8);
Texture2D ScreenAmbientOcclusionTexture : register(t9);
// SSR. t10 is the previous HDR color, t11 the Hi-Z distance pyramid.
Texture2D ScreenReflectionColorTexture : register(t10);
Texture2D ScreenReflectionDepthTexture : register(t11);
// IBL. t12 is the prefiltered specular (or source) cube, t13 the irradiance.
TextureCube EnvironmentMap : register(t12);
TextureCube IrradianceMap : register(t13);
// Forward+. t14 are the lights, t15 the per-cluster light numbers and t16
// the per-cluster counts written by LamaPonLightCulling.hlsl.
struct ClusterLight
{
    float4 PositionRange;
    float4 ColorIntensity;
    float4 DirectionInnerCosine;
    // x=outer cosine, y=type (0 point, 1 spot), z=shadow reference.
    float4 ExtraParameters;
};
StructuredBuffer<ClusterLight> ClusterLights : register(t14);
StructuredBuffer<uint> ClusterLightIndexList : register(t15);
StructuredBuffer<uint> ClusterLightCounts : register(t16);
// Baked GI. t17-t19 are the red, green and blue L1 spherical harmonics
// volumes (texel = x, y, z, constant term).
Texture3D BakedGiRedTexture : register(t17);
Texture3D BakedGiGreenTexture : register(t18);
Texture3D BakedGiBlueTexture : register(t19);
// The blended second reflection probe. t20 is its prefiltered specular cube
// and t21 its irradiance cube.
TextureCube SecondaryEnvironmentMap : register(t20);
TextureCube SecondaryIrradianceMap : register(t21);
SamplerState AlbedoSampler : register(s0);
SamplerComparisonState ShadowSampler : register(s1);

// D3D11のLamaPonLit.hlslのEvaluateBakedAmbientと同じ、場所ごとの環境光です。
// ボリューム内はベイクした間接光、範囲外や無効時は通常の環境光を返し、
// ボリュームの縁5%の帯で滑らかに混ぜます。
float3 EvaluateBakedAmbient(float3 worldPosition, float3 normal)
{
    const float3 ambient = AmbientColorIntensity.rgb * AmbientColorIntensity.w;
    if (BakedGiVolumeMinimum.w < 0.5f)
    {
        return ambient;
    }
    const float3 volumeUvw =
        (worldPosition - BakedGiVolumeMinimum.xyz) * BakedGiInverseSize.xyz;
    // プローブは格子の角にあるので、テクセル中心へ寄せて読みます。
    const float3 resolution = BakedGiResolution.xyz;
    const float3 texelUvw =
        (volumeUvw * (resolution - 1.0f) + 0.5f) / resolution;
    const float4 basis = float4(normal, 1.0f);
    float3 gi;
    gi.r = dot(
        basis,
        BakedGiRedTexture.SampleLevel(AlbedoSampler, texelUvw, 0.0f));
    gi.g = dot(
        basis,
        BakedGiGreenTexture.SampleLevel(AlbedoSampler, texelUvw, 0.0f));
    gi.b = dot(
        basis,
        BakedGiBlueTexture.SampleLevel(AlbedoSampler, texelUvw, 0.0f));
    gi = max(gi, 0.0f.xxx) * BakedGiInverseSize.w;
    const float3 edge = (0.5f - abs(volumeUvw - 0.5f)) / 0.05f;
    const float weight = saturate(min(min(edge.x, edge.y), edge.z));
    return lerp(ambient, gi, weight);
}

// D3D11のLamaPonLit.hlslと同じリフレクションプローブのボックス射影です。
// 反射レイと箱の交点を、プローブ中心から見た向きにして読みます。
float3 ApplyBoxProjection(
    float3 reflection,
    float3 worldPosition,
    float3 boxCenter,
    float3 boxExtents)
{
    const float3 firstPlane =
        (boxCenter + boxExtents - worldPosition) / reflection;
    const float3 secondPlane =
        (boxCenter - boxExtents - worldPosition) / reflection;
    const float3 furthest = max(firstPlane, secondPlane);
    const float distance = min(min(furthest.x, furthest.y), furthest.z);
    const float3 intersection = worldPosition + reflection * distance;
    return intersection - boxCenter;
}

// プローブ1個ぶんのスペキュラです。反射はプローブごとの箱で補正します。
float3 SampleProbeSpecular(
    TextureCube probeMap,
    float3 normal,
    float3 viewDirection,
    float3 worldPosition,
    float roughness,
    float maximumMip,
    float4 boxCenter,
    float4 boxParameters)
{
    float3 reflection = reflect(-viewDirection, normal);
    if (boxParameters.w >= 0.5f)
    {
        reflection = ApplyBoxProjection(
            reflection,
            worldPosition,
            boxCenter.xyz,
            boxParameters.xyz);
    }
    return probeMap.SampleLevel(
        AlbedoSampler,
        reflection,
        roughness * maximumMip).rgb;
}

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 textureCoordinate : TEXCOORD;
};

struct PixelInput
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD1;
    float3 normal : NORMAL;
    float2 textureCoordinate : TEXCOORD;
    // D3D11のLamaPonLit.hlslのTintと同じく、1個ずつの描画はBaseColor、
    // インスタンス描画はinstanceの色です。
    float4 tint : COLOR0;
};

PixelInput PrimitiveVertexShader(VertexInput input)
{
    PixelInput output;
    // D3D11のLamaPonLit.hlslと同じく、Worldの後にViewProjectionを掛けます。
    // 深度の丸めまで揃い、SSRのように自分の面と深度を比べる判定も
    // D3D11と一致します。
    const float4 worldPosition = mul(float4(input.position, 1.0f), World);
    output.position = mul(worldPosition, ViewProjection);
    output.worldPosition = worldPosition.xyz;
    // D3D11のVSMainと同じく、法線は逆転置行列で変換します。
    output.normal = normalize(
        mul(float4(input.normal, 0.0f), WorldInverseTranspose).xyz);
    output.textureCoordinate = input.textureCoordinate;
    output.tint = BaseColor;
    return output;
}

struct InstancedVertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 textureCoordinate : TEXCOORD;
    float4 instanceWorld0 : INSTANCE_TRANSFORM0;
    float4 instanceWorld1 : INSTANCE_TRANSFORM1;
    float4 instanceWorld2 : INSTANCE_TRANSFORM2;
    float4 instanceWorld3 : INSTANCE_TRANSFORM3;
    float4 instanceColor : INSTANCE_COLOR0;
};

// D3D11のLamaPonLit.hlslのVSInstancedMainと同じく、slot 1のworld行列と色で
// 描きます。法線は逆転置行列の代わりにworldで変換して正規化します。
PixelInput PrimitiveInstancedVertexShader(InstancedVertexInput input)
{
    const float4x4 world = float4x4(
        input.instanceWorld0,
        input.instanceWorld1,
        input.instanceWorld2,
        input.instanceWorld3);
    PixelInput output;
    const float4 worldPosition = mul(float4(input.position, 1.0f), world);
    output.position = mul(worldPosition, ViewProjection);
    output.worldPosition = worldPosition.xyz;
    output.normal = normalize(mul(float4(input.normal, 0.0f), world).xyz);
    output.textureCoordinate = input.textureCoordinate;
    output.tint = input.instanceColor;
    return output;
}

float SampleDirectionalShadowCascade(
    float3 worldPosition,
    float3 normal,
    uint cascadeIndex)
{
    const float3 biasedPosition =
        worldPosition + normal * ShadowParameters.z;
    const float4 lightPosition = mul(
        float4(biasedPosition, 1.0f),
        ShadowViewProjections[cascadeIndex]);
    const float3 projected = lightPosition.xyz
        / max(abs(lightPosition.w), 0.00001f);
    const float2 shadowUv =
        projected.xy * float2(0.5f, -0.5f) + 0.5f;
    if (shadowUv.x < 0.0f
        || shadowUv.x > 1.0f
        || shadowUv.y < 0.0f
        || shadowUv.y > 1.0f
        || projected.z <= 0.0f
        || projected.z >= 1.0f)
    {
        return 1.0f;
    }

    float visibility = 0.0f;
    [unroll]
    for (int tapY = -1; tapY <= 1; ++tapY)
    {
        [unroll]
        for (int tapX = -1; tapX <= 1; ++tapX)
        {
            visibility += DirectionalShadowTexture.SampleCmpLevelZero(
                ShadowSampler,
                float3(
                    shadowUv
                        + float2(tapX, tapY)
                            * CameraForwardShadowTexel.w,
                    cascadeIndex),
                projected.z - ShadowParameters.y);
        }
    }
    return visibility / 9.0f;
}

float EvaluateDirectionalShadow(
    float3 worldPosition,
    float3 normal,
    uint lightIndex)
{
    if (ShadowParameters.x < 0.5f
        || lightIndex + 1u != (uint)ShadowParameters.x)
    {
        return 1.0f;
    }
    const float cameraDistance = dot(
        worldPosition - CameraPosition.xyz,
        CameraForwardShadowTexel.xyz);
    uint cascadeIndex = 0u;
    [unroll]
    for (uint index = 0u; index < 4u; ++index)
    {
        if (index >= LightCounts.w)
        {
            return 1.0f;
        }
        cascadeIndex = index;
        if (cameraDistance <= ShadowCascadeSplits[index])
        {
            break;
        }
    }
    if (cameraDistance > ShadowCascadeSplits[cascadeIndex])
    {
        return 1.0f;
    }

    float visibility = SampleDirectionalShadowCascade(
        worldPosition,
        normal,
        cascadeIndex);
    if (cascadeIndex + 1u < LightCounts.w)
    {
        const float previousSplit = cascadeIndex == 0u
            ? 0.0f
            : ShadowCascadeSplits[cascadeIndex - 1u];
        const float cascadeRange =
            ShadowCascadeSplits[cascadeIndex] - previousSplit;
        const float blendStart = ShadowCascadeSplits[cascadeIndex]
            - cascadeRange * 0.1f;
        const float blend = saturate(
            (cameraDistance - blendStart)
            / max(cascadeRange * 0.1f, 0.0001f));
        if (blend > 0.0f)
        {
            visibility = lerp(
                visibility,
                SampleDirectionalShadowCascade(
                    worldPosition,
                    normal,
                    cascadeIndex + 1u),
                blend);
        }
    }
    return lerp(
        1.0f,
        visibility,
        saturate(ShadowParameters.w));
}

float EvaluateSpotShadow(
    float3 worldPosition,
    float3 normal,
    uint slot)
{
    const float4 parameters = SpotShadowParameters[slot];
    if (parameters.w < 0.5f)
    {
        return 1.0f;
    }
    const float3 biasedPosition =
        worldPosition + normal * parameters.y;
    const float4 lightPosition = mul(
        float4(biasedPosition, 1.0f),
        SpotShadowViewProjections[slot]);
    if (lightPosition.w <= 0.0001f)
    {
        return 1.0f;
    }
    const float3 projected = lightPosition.xyz / lightPosition.w;
    const float2 shadowUv =
        projected.xy * float2(0.5f, -0.5f) + 0.5f;
    if (shadowUv.x < 0.0f
        || shadowUv.x > 1.0f
        || shadowUv.y < 0.0f
        || shadowUv.y > 1.0f
        || projected.z <= 0.0f
        || projected.z >= 1.0f)
    {
        return 1.0f;
    }

    float visibility = 0.0f;
    [unroll]
    for (int tapY = -1; tapY <= 1; ++tapY)
    {
        [unroll]
        for (int tapX = -1; tapX <= 1; ++tapX)
        {
            visibility += SpotShadowTexture.SampleCmpLevelZero(
                ShadowSampler,
                float3(
                    shadowUv
                        + float2(tapX, tapY)
                            * LocalShadowTexelSizes.x,
                    slot),
                projected.z - parameters.x);
        }
    }
    return lerp(
        1.0f,
        visibility / 9.0f,
        saturate(parameters.z));
}

float EvaluatePointShadow(
    float3 worldPosition,
    uint lightIndex,
    float3 lightPosition,
    float range)
{
    if (PointShadowParameters.x < 0.5f
        || lightIndex + 1u != (uint)PointShadowParameters.x)
    {
        return 1.0f;
    }
    const float3 fromLight = worldPosition - lightPosition;
    const float3 absoluteVector = abs(fromLight);
    const float majorAxis = max(
        absoluteVector.x,
        max(absoluteVector.y, absoluteVector.z));
    const float nearPlane = 0.1f;
    const float farPlane = max(range, nearPlane + 0.01f);
    const float depth = farPlane / (farPlane - nearPlane)
        - farPlane * nearPlane
            / ((farPlane - nearPlane) * max(majorAxis, nearPlane));
    const float3 direction = normalize(fromLight);
    const float3 axis = abs(direction.y) > 0.9f
        ? float3(1.0f, 0.0f, 0.0f)
        : float3(0.0f, 1.0f, 0.0f);
    const float3 tangent = normalize(cross(axis, direction));
    const float3 bitangent = cross(direction, tangent);
    const float pointTexel = LocalShadowTexelSizes.y * 2.0f;
    const float compareDepth = depth - PointShadowParameters.y;
    float visibility = PointShadowTexture.SampleCmpLevelZero(
        ShadowSampler,
        direction,
        compareDepth);
    visibility += PointShadowTexture.SampleCmpLevelZero(
        ShadowSampler,
        normalize(direction + (tangent + bitangent) * pointTexel),
        compareDepth);
    visibility += PointShadowTexture.SampleCmpLevelZero(
        ShadowSampler,
        normalize(direction + (tangent - bitangent) * pointTexel),
        compareDepth);
    visibility += PointShadowTexture.SampleCmpLevelZero(
        ShadowSampler,
        normalize(direction - (tangent - bitangent) * pointTexel),
        compareDepth);
    visibility += PointShadowTexture.SampleCmpLevelZero(
        ShadowSampler,
        normalize(direction - (tangent + bitangent) * pointTexel),
        compareDepth);
    return lerp(
        1.0f,
        visibility / 5.0f,
        saturate(PointShadowParameters.z));
}

// D3D11のLamaPonLit.hlslと同じSSRです。t11のHi-Z深度ピラミッドは
// カメラからの距離を持ち、ミップNは2x2の最小値（最も手前）です。輪郭を
// またいだ中間の距離を作らないよう、点で読みます。
float ScreenReflectionSceneDistance(float2 uv)
{
    const float2 screenSize =
        1.0f / max(ScreenReflectionScreen.xy, 1e-6f);
    const int2 lastPixel = max(int2(screenSize) - 1, int2(0, 0));
    const int2 pixel = clamp(
        int2(saturate(uv) * screenSize),
        int2(0, 0),
        lastPixel);
    return ScreenReflectionDepthTexture.Load(int3(pixel, 0)).r;
}

// 反射レイを画面空間のHi-Zトラバーサルで進め、当たった点の色を前
// フレームのカラーから読みます。何も無い区画は出口まで飛んで粗い
// ミップへ上がり、またぎそうなら細かいミップへ下ります。aは信頼度で、
// 画面の縁、最大距離、カメラへ戻る反射、粗さの上限に近いほど0へ落とし、
// 呼び出し側は環境光だけへ戻します。
float4 EvaluateScreenSpaceReflection(
    float3 worldPosition,
    float3 reflection,
    float3 viewDirection,
    float roughness)
{
    if (ScreenReflectionParameters.y < 0.5f)
    {
        return 0.0f.xxxx;
    }
    const float roughnessCutoff = max(ScreenReflectionQuality.y, 0.0001f);
    if (roughness >= roughnessCutoff)
    {
        return 0.0f.xxxx;
    }

    const float maximumDistance = max(ScreenReflectionParameters.z, 0.01f);
    const float thickness = max(ScreenReflectionQuality.x, 0.001f);
    // 設定のサンプル数は、Hi-Zの反復上限として働きます。
    const int maximumSteps = clamp(
        (int)ScreenReflectionParameters.w,
        4,
        128);

    // レイの両端をクリップ空間へ移します。右手系の透視射影ではwが
    // カメラからの距離です。
    const float3 rayStart = worldPosition;
    float3 rayEnd = worldPosition + reflection * maximumDistance;
    float4 clipStart = mul(float4(rayStart, 1.0f), ViewProjection);
    float4 clipEnd = mul(float4(rayEnd, 1.0f), ViewProjection);

    // カメラより手前へ回った側は、射影が破綻する前に世界空間で詰めます。
    const float nearW = 0.05f;
    if (clipStart.w <= nearW)
    {
        return 0.0f.xxxx;
    }
    if (clipEnd.w <= nearW)
    {
        const float clipRatio =
            (nearW - clipStart.w) / (clipEnd.w - clipStart.w);
        rayEnd = lerp(rayStart, rayEnd, saturate(clipRatio));
        clipEnd = mul(float4(rayEnd, 1.0f), ViewProjection);
    }

    const float2 startUv = float2(
        clipStart.x / clipStart.w * 0.5f + 0.5f,
        0.5f - clipStart.y / clipStart.w * 0.5f);
    const float2 endUv = float2(
        clipEnd.x / clipEnd.w * 0.5f + 0.5f,
        0.5f - clipEnd.y / clipEnd.w * 0.5f);
    const float2 deltaUv = endUv - startUv;

    // 画面外には情報が無いので、レイを画面端で打ち切ります。
    float limitAlpha = 1.0f;
    [unroll]
    for (int axis = 0; axis < 2; ++axis)
    {
        const float direction = axis == 0 ? deltaUv.x : deltaUv.y;
        const float origin = axis == 0 ? startUv.x : startUv.y;
        if (abs(direction) > 1e-6f)
        {
            const float exitAlpha = max(
                (0.0f - origin) / direction,
                (1.0f - origin) / direction);
            if (exitAlpha > 0.0f)
            {
                limitAlpha = min(limitAlpha, exitAlpha);
            }
        }
    }
    limitAlpha = clamp(limitAlpha, 0.0f, 1.0f);

    const float2 screenSize =
        1.0f / max(ScreenReflectionScreen.xy, 1e-6f);
    // 自己ヒットを避けるため、出発点を半画素ずらします。
    const float2 pixelDelta = deltaUv * limitAlpha * screenSize;
    const float pixelLength = max(
        max(abs(pixelDelta.x), abs(pixelDelta.y)),
        1.0f);
    // 1/wは画面空間で線形なので、補間だけで距離を求めます。
    const float inverseStartW = 1.0f / clipStart.w;
    const float inverseEndW = 1.0f / clipEnd.w;
    const int maximumLevel = max((int)ScreenReflectionQuality.z, 0);

    float alpha = 0.5f * limitAlpha / pixelLength;
    // 区画の辺の上で、丸めにより同じ区画を再訪し続けないための最小
    // 前進量です。
    const float alphaBias = limitAlpha * 1e-5f;
    int level = 0;

    [loop]
    for (int iteration = 0; iteration < maximumSteps; ++iteration)
    {
        if (alpha >= limitAlpha)
        {
            break;
        }
        const float2 uv = startUv + deltaUv * alpha;
        const float2 levelSize = max(
            floor(screenSize / exp2((float)level)),
            1.0f);
        const float2 cell = floor(clamp(uv, 0.0f, 1.0f) * levelSize);
        const float2 towardEdge = float2(
            deltaUv.x >= 0.0f ? 1.0f : 0.0f,
            deltaUv.y >= 0.0f ? 1.0f : 0.0f);
        const float2 boundaryUv = (cell + towardEdge) / levelSize;
        float2 boundaryAlpha = float2(1e9f, 1e9f);
        if (abs(deltaUv.x) > 1e-8f)
        {
            boundaryAlpha.x = (boundaryUv.x - startUv.x) / deltaUv.x;
        }
        if (abs(deltaUv.y) > 1e-8f)
        {
            boundaryAlpha.y = (boundaryUv.y - startUv.y) / deltaUv.y;
        }
        const float exitAlpha = max(
            min(boundaryAlpha.x, boundaryAlpha.y),
            alpha + alphaBias);
        const float clampedExitAlpha = min(exitAlpha, limitAlpha);

        // この区画を通るあいだの、レイの距離の範囲です。
        const float entryDistance = 1.0f / max(
            lerp(inverseStartW, inverseEndW, alpha),
            1e-6f);
        const float exitDistance = 1.0f / max(
            lerp(inverseStartW, inverseEndW, clampedExitAlpha),
            1e-6f);
        const float rayNear = min(entryDistance, exitDistance);
        const float rayFar = max(entryDistance, exitDistance);
        const float sceneDistance = ScreenReflectionDepthTexture.Load(
            int3(int2(min(cell, levelSize - 1.0f)), level)).r;

        if (rayFar <= sceneDistance)
        {
            // 区間全体が区画で最も手前の面より手前なので、出口まで
            // 飛びます。面から2%以上離れたときだけ粗いミップへ上がります。
            alpha = exitAlpha;
            if (rayFar * 1.02f <= sceneDistance)
            {
                level = min(level + 1, maximumLevel);
            }
            continue;
        }
        if (level > 0)
        {
            // またぐかもしれないので、進まずに1段細かく見ます。
            level = level - 1;
            continue;
        }

        if (rayFar > sceneDistance
            && rayNear < sceneDistance + thickness)
        {
            // 当たった区間を二分して詰め、反射の縞を防ぎます。
            float nearAlpha = alpha;
            float farAlpha = clampedExitAlpha;
            [unroll]
            for (int refine = 0; refine < 4; ++refine)
            {
                const float middleAlpha = (nearAlpha + farAlpha) * 0.5f;
                const float middleDistance = 1.0f / max(
                    lerp(inverseStartW, inverseEndW, middleAlpha),
                    1e-6f);
                const float middleScene = ScreenReflectionSceneDistance(
                    startUv + deltaUv * middleAlpha);
                if (middleDistance > middleScene)
                {
                    farAlpha = middleAlpha;
                }
                else
                {
                    nearAlpha = middleAlpha;
                }
            }
            const float hitAlpha = (nearAlpha + farAlpha) * 0.5f;
            const float2 hitUv = startUv + deltaUv * hitAlpha;
            // 画面空間の比率を、透視補間でワールド空間の比率へ直します。
            const float worldRatio = hitAlpha * clipStart.w
                / max(lerp(clipEnd.w, clipStart.w, hitAlpha), 1e-6f);
            const float3 hitPosition = lerp(
                rayStart,
                rayEnd,
                saturate(worldRatio));

            // 当たった点を前フレームの画面へ戻して色を読みます。
            const float4 previousClip = mul(
                float4(hitPosition, 1.0f),
                ScreenReflectionPreviousViewProjection);
            if (previousClip.w <= 0.0001f)
            {
                return 0.0f.xxxx;
            }
            const float2 previousUv = float2(
                previousClip.x / previousClip.w * 0.5f + 0.5f,
                0.5f - previousClip.y / previousClip.w * 0.5f);
            if (previousUv.x < 0.0f || previousUv.x > 1.0f
                || previousUv.y < 0.0f || previousUv.y > 1.0f)
            {
                return 0.0f.xxxx;
            }

            // 現在と前フレームの両方で、画面の縁に近いほど弱めます。
            const float2 currentEdge = min(hitUv, 1.0f - hitUv);
            const float2 previousEdge = min(previousUv, 1.0f - previousUv);
            const float edgeDistance = min(
                min(currentEdge.x, currentEdge.y),
                min(previousEdge.x, previousEdge.y));
            const float edgeFade = saturate(edgeDistance / 0.08f);
            // 最大距離の最後の1/4で滑らかに落とします。
            const float travelled = saturate(worldRatio)
                * length(rayEnd - rayStart);
            const float travelledFraction = saturate(
                travelled / maximumDistance);
            const float distanceFade = saturate(
                (1.0f - travelledFraction) / 0.25f);
            // 画面には物の裏側が無いため、カメラへ戻る反射ほど弱めます。
            const float towardCamera = saturate(
                dot(reflection, viewDirection));
            const float directionFade = saturate(
                (1.0f - towardCamera) / 0.5f);
            const float roughnessFade = saturate(
                1.0f - roughness / roughnessCutoff);
            const float3 color = ScreenReflectionColorTexture.SampleLevel(
                AlbedoSampler,
                previousUv,
                0.0f).rgb;
            return float4(
                color,
                edgeFade
                    * distanceFade
                    * directionFade
                    * roughnessFade
                    * saturate(ScreenReflectionParameters.x));
        }

        // 面の裏を厚みの外で通り過ぎたので、ミップを保って次の区画へ
        // 進みます。
        alpha = exitAlpha;
    }
    return 0.0f.xxxx;
}

static const float LamaPonPi = 3.14159265f;

// LamaPonLit.hlslのSourceRepresentativeDirectionと同じく、太陽の見かけの
// 大きさを反映した鏡面の代表点へ光の向きを寄せます。
float3 SourceRepresentativeDirection(
    float3 toLight,
    float3 normal,
    float3 viewDirection,
    float angularRadius)
{
    if (angularRadius <= 0.0f)
    {
        return toLight;
    }
    const float3 reflected = reflect(-viewDirection, normal);
    const float alignment = dot(toLight, reflected);
    const float diskCosine = cos(angularRadius);
    if (alignment >= diskCosine)
    {
        return reflected;
    }
    const float3 sideways = reflected - alignment * toLight;
    const float sidewaysLength = length(sideways);
    if (sidewaysLength <= 1.0e-5f)
    {
        return toLight;
    }
    return normalize(
        toLight * diskCosine
        + (sideways / sidewaysLength) * sin(angularRadius));
}

// 代表点へ寄せて広がったハイライトの明るさを戻します（LamaPonLit.hlslの
// SourceSpecularEnergyと同じ）。
float SourceSpecularEnergy(
    float roughness,
    float angularRadius)
{
    if (angularRadius <= 0.0f)
    {
        return 1.0f;
    }
    const float alpha = max(roughness * roughness, 1.0e-4f);
    const float widened =
        saturate(alpha + sin(angularRadius) * 0.5f);
    const float ratio = alpha / max(widened, 1.0e-4f);
    return ratio * ratio;
}

// LamaPonLit.hlslのEvaluateLightPbrSizedと同じCook-Torrance GGXです。拡散と
// 陰りは本当の光の向き、鏡面は代表点の向きで求めます。
float3 EvaluateLightPbrSized(
    float3 normal,
    float3 toLight,
    float3 specularToLight,
    float3 viewDirection,
    float3 albedo,
    float roughness,
    float metallic,
    float3 radiance,
    float specularEnergy)
{
    const float normalDotLight =
        saturate(dot(normal, toLight));
    if (normalDotLight <= 0.0f)
    {
        return 0.0f.xxx;
    }
    const float3 halfVector =
        normalize(specularToLight + viewDirection);
    const float normalDotView = max(
        dot(normal, viewDirection),
        0.0001f);
    const float normalDotHalf =
        saturate(dot(normal, halfVector));
    const float viewDotHalf =
        saturate(dot(viewDirection, halfVector));

    const float alpha = roughness * roughness;
    const float alphaSquared = alpha * alpha;
    const float denominator =
        normalDotHalf * normalDotHalf
            * (alphaSquared - 1.0f)
        + 1.0f;
    const float distribution =
        alphaSquared
        / max(LamaPonPi * denominator * denominator,
            1.0e-12f);

    const float k = alpha * 0.5f + 0.0001f;
    const float geometryView =
        normalDotView / (normalDotView * (1.0f - k) + k);
    const float geometryLight =
        normalDotLight
        / (normalDotLight * (1.0f - k) + k);
    const float geometry = geometryView * geometryLight;

    const float3 f0 = lerp(0.04f.xxx, albedo, metallic);
    const float3 fresnel =
        f0
        + (1.0f.xxx - f0)
            * pow(1.0f - viewDotHalf, 5.0f);

    const float3 specular =
        distribution * geometry * fresnel
        * specularEnergy
        / max(4.0f * normalDotView * normalDotLight,
            0.0001f);
    const float3 diffuse =
        (1.0f.xxx - fresnel)
        * (1.0f - metallic)
        * albedo
        / LamaPonPi;
    return (diffuse + specular)
        * radiance
        * normalDotLight;
}

float3 EvaluateLightPbr(
    float3 normal,
    float3 toLight,
    float3 viewDirection,
    float3 albedo,
    float roughness,
    float metallic,
    float3 radiance)
{
    return EvaluateLightPbrSized(
        normal,
        toLight,
        toLight,
        viewDirection,
        albedo,
        roughness,
        metallic,
        radiance,
        1.0f);
}

float4 PrimitivePixelShader(PixelInput input) : SV_Target
{
    float3 normal = normalize(input.normal);
    if (MaterialProperties.w > 0.5f)
    {
        // BC5 stores only XY. Reconstructing Z also keeps ordinary RGB
        // normal maps equivalent after their XY channels are sampled.
        // D3D11のApplyNormalMapと同じく、強さをxyへ掛けてからzを復元し、
        // 傾けても長さが1に保たれるようにします。
        const float strength = max(EmissiveFactor.w, 0.0f);
        const float2 sampledNormalXY = (NormalTexture.Sample(
            AlbedoSampler,
            input.textureCoordinate).xy * 2.0f - 1.0f) * strength;
        const float3 sampledNormal = float3(
            sampledNormalXY,
            sqrt(saturate(
                1.0f - dot(sampledNormalXY, sampledNormalXY))));
        const float3 positionDx = ddx(input.worldPosition);
        const float3 positionDy = ddy(input.worldPosition);
        const float2 uvDx = ddx(input.textureCoordinate);
        const float2 uvDy = ddy(input.textureCoordinate);
        const float3 tangentUnscaled =
            cross(positionDy, normal) * uvDx.x
            + cross(normal, positionDx) * uvDy.x;
        const float3 bitangentUnscaled =
            cross(positionDy, normal) * uvDx.y
            + cross(normal, positionDx) * uvDy.y;
        const float inverseScale = rsqrt(max(
            max(dot(tangentUnscaled, tangentUnscaled),
                dot(bitangentUnscaled, bitangentUnscaled)),
            0.000001f));
        const float3 tangent = tangentUnscaled * inverseScale;
        const float3 bitangent = bitangentUnscaled * inverseScale;
        normal = normalize(
            tangent * sampledNormal.x
            + bitangent * sampledNormal.y
            + normal * sampledNormal.z);
    }
    const float4 albedo = AlbedoTexture.Sample(
        AlbedoSampler,
        input.textureCoordinate);
    const float3 surfaceColor = albedo.rgb * input.tint.rgb;
    const float roughnessSample = RoughnessTexture.Sample(
        AlbedoSampler,
        input.textureCoordinate).g;
    const float metallicSample = MetallicTexture.Sample(
        AlbedoSampler,
        input.textureCoordinate).b;
    const float occlusionSample = OcclusionTexture.Sample(
        AlbedoSampler,
        input.textureCoordinate).r;
    const float3 emissiveSample = EmissiveTexture.Sample(
        AlbedoSampler,
        input.textureCoordinate).rgb;
    const float roughness = clamp(
        MaterialProperties.x * roughnessSample,
        0.02f,
        1.0f);
    const float metallic = saturate(
        MaterialProperties.y * metallicSample);
    float occlusion = lerp(
        1.0f,
        occlusionSample,
        saturate(MaterialProperties.z));
    // D3D11のLamaPonLit.hlslと同じく、SSAOも材質の遮蔽と同じ扱いで
    // 環境光項だけへ掛け、直接光や影の中は暗くしません。
    if (ScreenAmbientOcclusionParameters.z >= 0.5f)
    {
        const float2 screenUv =
            input.position.xy * ScreenAmbientOcclusionParameters.xy;
        occlusion *= ScreenAmbientOcclusionTexture.Sample(
            AlbedoSampler,
            screenUv).r;
    }
    const float3 specularColor = lerp(0.04f.xxx, surfaceColor, metallic);
    const float3 viewDirection = normalize(
        CameraPosition.xyz - input.worldPosition);
    // D3D11のEvaluateEnvironmentと同じく、環境光とSSRを組み合わせてから
    // 遮蔽を掛けます。粗さはD3D11のLit shaderと同じ下限で求めます。
    const float environmentRoughness =
        clamp(MaterialProperties.x * roughnessSample, 0.04f, 1.0f);
    const float4 screenReflection = EvaluateScreenSpaceReflection(
        input.worldPosition,
        reflect(-viewDirection, normal),
        viewDirection,
        environmentRoughness);
    float3 ambient = 0.0f.xxx;
    if (EnvironmentParameters.y < 0.5f)
    {
        // キューブマップが無いときは金属ほど弱め、SSRが当たった分を
        // F0の重みで足します。
        ambient = surfaceColor
            * EvaluateBakedAmbient(input.worldPosition, normal)
            * (1.0f - metallic * 0.5f);
        if (screenReflection.a > 0.0f)
        {
            ambient += screenReflection.rgb
                * specularColor
                * screenReflection.a;
        }
    }
    else
    {
        // zは事前畳み込み済みスペキュラの最終ミップ番号です。0のときは
        // cubemapを直接読み、最も粗いミップで拡散を近似します。
        const float prefilteredMaximumMip = EnvironmentParameters.z;
        float maximumMip = prefilteredMaximumMip;
        if (maximumMip <= 0.0f)
        {
            uint width;
            uint height;
            uint mipCount;
            EnvironmentMap.GetDimensions(0, width, height, mipCount);
            maximumMip = max((float)mipCount - 1.0f, 0.0f);
        }
        float3 irradiance = prefilteredMaximumMip > 0.0f
            ? IrradianceMap.SampleLevel(AlbedoSampler, normal, 0.0f).rgb
            : EnvironmentMap.SampleLevel(
                AlbedoSampler,
                normal,
                maximumMip).rgb;
        // D3D11と同じく、リフレクションプローブの箱で反射を補正し、2個目の
        // プローブがあれば比率で混ぜてからSSRを被せます。
        float3 prefiltered = SampleProbeSpecular(
            EnvironmentMap,
            normal,
            viewDirection,
            input.worldPosition,
            environmentRoughness,
            maximumMip,
            ReflectionBoxCenter,
            ReflectionBoxParameters);
        const float blendWeight = ReflectionBlendParameters.x;
        if (blendWeight > 0.0f)
        {
            irradiance = lerp(
                irradiance,
                SecondaryIrradianceMap.SampleLevel(
                    AlbedoSampler,
                    normal,
                    0.0f).rgb,
                blendWeight);
            prefiltered = lerp(
                prefiltered,
                SampleProbeSpecular(
                    SecondaryEnvironmentMap,
                    normal,
                    viewDirection,
                    input.worldPosition,
                    environmentRoughness,
                    ReflectionBlendParameters.y,
                    ReflectionSecondaryBoxCenter,
                    ReflectionSecondaryBoxParameters),
                blendWeight);
        }
        if (screenReflection.a > 0.0f)
        {
            prefiltered = lerp(
                prefiltered,
                screenReflection.rgb,
                screenReflection.a);
        }
        const float normalDotView = max(dot(normal, viewDirection), 0.0001f);
        // split-sumのBRDF項はD3D11と同じKarisの解析近似です。
        const float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
        const float4 c1 = float4(1.0f, 0.0425f, 1.04f, -0.04f);
        const float4 r = environmentRoughness * c0 + c1;
        const float a004 =
            min(r.x * r.x, exp2(-9.28f * normalDotView)) * r.x + r.y;
        const float2 brdf = float2(-1.04f, 1.04f) * a004 + r.zw;
        const float3 diffuse = irradiance * surfaceColor * (1.0f - metallic);
        const float3 specular =
            prefiltered * (specularColor * brdf.x + brdf.y);
        ambient = (diffuse + specular) * EnvironmentParameters.x
            + surfaceColor
                * EvaluateBakedAmbient(input.worldPosition, normal);
    }
    float3 result = ambient * occlusion;
    // 直接光はD3D11のLamaPonLit.hlslと同じCook-Torrance GGXです。太陽は
    // DirectionalColors.wの角半径で鏡面の代表点を寄せます。
    [loop]
    for (uint index = 0; index < min(LightCounts.x, 4u); ++index)
    {
        const float shadow = EvaluateDirectionalShadow(
            input.worldPosition,
            normal,
            index);
        const float3 toLight = normalize(
            -DirectionalDirectionIntensity[index].xyz);
        const float angularRadius = DirectionalColors[index].w;
        result += EvaluateLightPbrSized(
            normal,
            toLight,
            SourceRepresentativeDirection(
                toLight,
                normal,
                viewDirection,
                angularRadius),
            viewDirection,
            surfaceColor,
            environmentRoughness,
            metallic,
            DirectionalColors[index].rgb
                * DirectionalDirectionIntensity[index].w
                * shadow,
            SourceSpecularEnergy(environmentRoughness, angularRadius));
    }

    if (ClusteredParameters.w >= 0.5f)
    {
        // D3D11のLamaPonLit.hlslと同じForward+です。自分のピクセルが入る
        // クラスタの番号表だけを見てPoint／Spot Lightを計算します。
        const uint gridX = (uint)ClusteredParameters.x;
        const uint gridY = (uint)ClusteredParameters.y;
        const uint gridZ = (uint)ClusteredParameters.z;
        const float2 screenRatio = saturate(
            input.position.xy
            * ClusteredScreenParameters.xy);
        const uint clusterX = min(
            (uint)(screenRatio.x * gridX),
            gridX - 1u);
        const uint clusterY = min(
            (uint)(screenRatio.y * gridY),
            gridY - 1u);
        // カメラからの奥行きから、カリングと同じ指数分割のスライスを
        // 求めます。
        const float nearPlane = ClusteredDepthParameters.x;
        const float viewDepth = max(
            dot(
                CameraForwardShadowTexel.xyz,
                input.worldPosition - CameraPosition.xyz),
            nearPlane);
        const uint clusterZ = min(
            (uint)(log(viewDepth / nearPlane)
                / ClusteredDepthParameters.z
                * gridZ),
            gridZ - 1u);
        const uint cluster =
            clusterZ * gridX * gridY
            + clusterY * gridX
            + clusterX;
        const uint maximumPerCluster =
            (uint)ClusteredDepthParameters.w;
        const uint clusterOffset = cluster * maximumPerCluster;
        const uint clusterLightCount = min(
            ClusterLightCounts[cluster],
            maximumPerCluster);
        [loop]
        for (uint slot = 0; slot < clusterLightCount; ++slot)
        {
            const ClusterLight light = ClusterLights[
                ClusterLightIndexList[clusterOffset + slot]];
            const float3 delta =
                light.PositionRange.xyz - input.worldPosition;
            const float lightDistance = length(delta);
            const float range = max(light.PositionRange.w, 0.001f);
            const float distanceAttenuation =
                pow(saturate(1.0f - lightDistance / range), 2.0f);
            const float3 toLight =
                delta / max(lightDistance, 0.0001f);
            float attenuation = distanceAttenuation;
            float shadow = 1.0f;
            if (light.ExtraParameters.y < 0.5f)
            {
                // Point Light。影の参照はライト番号+1です。
                if (light.ExtraParameters.z >= 1.0f)
                {
                    shadow = EvaluatePointShadow(
                        input.worldPosition,
                        (uint)light.ExtraParameters.z - 1u,
                        light.PositionRange.xyz,
                        range);
                }
            }
            else
            {
                // Spot Light。コーン減衰は従来経路と同じく2乗します。
                const float cone = dot(
                    normalize(light.DirectionInnerCosine.xyz),
                    -toLight);
                const float coneFalloff = smoothstep(
                    light.ExtraParameters.x,
                    light.DirectionInnerCosine.w,
                    cone);
                attenuation *= coneFalloff * coneFalloff;
                if (light.ExtraParameters.z >= 1.0f)
                {
                    shadow = EvaluateSpotShadow(
                        input.worldPosition,
                        normal,
                        (uint)light.ExtraParameters.z - 1u);
                }
            }
            result += EvaluateLightPbr(
                normal,
                toLight,
                viewDirection,
                surfaceColor,
                environmentRoughness,
                metallic,
                light.ColorIntensity.rgb
                    * light.ColorIntensity.w
                    * attenuation
                    * shadow);
        }
    }
    else
    {
    // Point / SpotはD3D11の従来経路（LamaPonLit.hlsl）と同じ距離減衰と
    // コーン減衰です。
    [loop]
    for (uint pointIndex = 0; pointIndex < min(LightCounts.y, 16u); ++pointIndex)
    {
        const float3 delta = PointPositionRange[pointIndex].xyz - input.worldPosition;
        const float lightDistance = length(delta);
        const float range = max(PointPositionRange[pointIndex].w, 0.001f);
        const float attenuation = pow(saturate(1.0f - lightDistance / range), 2.0f);
        const float shadow = EvaluatePointShadow(
            input.worldPosition,
            pointIndex,
            PointPositionRange[pointIndex].xyz,
            range);
        result += EvaluateLightPbr(
            normal,
            delta / max(lightDistance, 0.0001f),
            viewDirection,
            surfaceColor,
            environmentRoughness,
            metallic,
            PointColorIntensity[pointIndex].rgb
                * PointColorIntensity[pointIndex].w
                * attenuation
                * shadow);
    }

    [loop]
    for (uint spotIndex = 0; spotIndex < min(LightCounts.z, 8u); ++spotIndex)
    {
        const float3 lightToPixel = input.worldPosition - SpotPositionRange[spotIndex].xyz;
        const float lightDistance = length(lightToPixel);
        const float range = max(SpotPositionRange[spotIndex].w, 0.001f);
        const float3 rayDirection = lightToPixel / max(lightDistance, 0.0001f);
        const float cone = dot(normalize(SpotDirectionInnerCosine[spotIndex].xyz), rayDirection);
        const float coneAttenuation = smoothstep(
            SpotOuterCosine[spotIndex].x,
            SpotDirectionInnerCosine[spotIndex].w,
            cone);
        const float distanceAttenuation = pow(saturate(1.0f - lightDistance / range), 2.0f);
        const uint shadowSlot =
            (uint)SpotOuterCosine[spotIndex].y;
        const float shadow = shadowSlot > 0u
            ? EvaluateSpotShadow(
                input.worldPosition,
                normal,
                shadowSlot - 1u)
            : 1.0f;
        result += EvaluateLightPbr(
            normal,
            -rayDirection,
            viewDirection,
            surfaceColor,
            environmentRoughness,
            metallic,
            SpotColorIntensity[spotIndex].rgb
                * SpotColorIntensity[spotIndex].w
                * distanceAttenuation
                * coneAttenuation
                * coneAttenuation
                * shadow);
    }
    } // Forward+が無効なときの従来経路の終わり
    result += emissiveSample * EmissiveFactor.rgb;
    const float outputAlpha = albedo.a * input.tint.a;
    if (FogParameters.w < 0.5f)
    {
        return float4(result, outputAlpha);
    }
    if (FogColorModel.w < 0.5f)
    {
        // D3D11のLamaPonLit.hlslと同じく、発光の後にカメラからの距離で
        // 範囲霧と指数霧の濃い方を掛けます。
        const float3 litColor = max(result, 0.0f);
        const float distanceToCamera =
            length(input.worldPosition - CameraPosition.xyz);
        const float rangeFog = smoothstep(
            FogParameters.x,
            max(FogParameters.y, FogParameters.x + 0.001f),
            distanceToCamera);
        const float exponentialFog = 1.0f
            - exp(
                -max(FogParameters.z, 0.0f)
                * max(distanceToCamera - FogParameters.x, 0.0f));
        const float fogAmount = saturate(max(rangeFog, exponentialFog));
        return float4(
            lerp(litColor, FogColorModel.rgb, fogAmount),
            outputAlpha);
    }
    // DirectXTK Effectと同じく、ビュー深度で開始から終了まで線形に霧を
    // 掛け、霧の色にはalphaを掛けます。開始と終了が同じなら全体が霧です。
    const float viewDepth = dot(
        input.worldPosition - CameraPosition.xyz,
        CameraForwardShadowTexel.xyz);
    const float fogFactor = FogParameters.y != FogParameters.x
        ? saturate(
            (viewDepth - FogParameters.x)
            / (FogParameters.y - FogParameters.x))
        : 1.0f;
    return float4(
        lerp(result, FogColorModel.rgb * outputAlpha, fogFactor),
        outputAlpha);
}
)";

    constexpr char ParticleShaderSource[] = R"(
cbuffer ParticleConstants : register(b0)
{
    row_major float4x4 ViewProjection;
};

Texture2D ParticleTexture : register(t0);
SamplerState ParticleSampler : register(s0);

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

PixelInput ParticleVertexShader(VertexInput input)
{
    PixelInput output;
    output.position = mul(float4(input.position, 1.0f), ViewProjection);
    output.color = input.color;
    output.textureCoordinate = input.textureCoordinate;
    return output;
}

float4 ParticlePixelShader(PixelInput input) : SV_Target
{
    return ParticleTexture.Sample(ParticleSampler, input.textureCoordinate)
        * input.color;
}

// D3D11のDirectXTK BasicEffect（頂点色とtexture、霧なし）と同じく、COLOR0、
// TEXCOORD0、SV_Positionの順で出力します。ParticleSystemのcustom pixel
// shaderはこの並びで受け取ります。
struct CustomPixelInput
{
    float4 color : COLOR0;
    float2 textureCoordinate : TEXCOORD0;
    float4 position : SV_Position;
};

CustomPixelInput CustomParticleVertexShader(VertexInput input)
{
    CustomPixelInput output;
    output.color = input.color;
    output.textureCoordinate = input.textureCoordinate;
    output.position = mul(float4(input.position, 1.0f), ViewProjection);
    return output;
}
)";

    void ThrowIfFailed(const HRESULT result, const char* operation)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(
                std::string(operation) + " failed with HRESULT "
                + std::to_string(static_cast<unsigned long>(result)));
        }
    }

    [[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob> CompileShader(
        const char* source,
        std::size_t sourceSize,
        const char* sourceName,
        const char* entryPoint,
        const char* target)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        const HRESULT result = D3DCompile(
            source,
            sourceSize,
            sourceName,
            nullptr,
            nullptr,
            entryPoint,
            target,
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0,
            bytecode.GetAddressOf(),
            errors.GetAddressOf());
        if (FAILED(result))
        {
            std::string message = std::string("D3DCompile(") + entryPoint
                + ") failed";
            if (errors != nullptr && errors->GetBufferSize() != 0)
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

    struct Geometry final
    {
        std::vector<PrimitiveRenderVertex> vertices;
        std::vector<std::uint32_t> indices;
    };

    void AddFace(
        Geometry& geometry,
        const DirectX::XMFLOAT3& normal,
        const std::array<DirectX::XMFLOAT3, 4>& positions)
    {
        const auto first = static_cast<std::uint32_t>(geometry.vertices.size());
        constexpr std::array<DirectX::XMFLOAT2, 4> ultravioletCoordinates{ {
            { 0.0f, 1.0f }, { 0.0f, 0.0f },
            { 1.0f, 1.0f }, { 1.0f, 0.0f }
        } };
        for (std::size_t index{}; index < positions.size(); ++index)
        {
            geometry.vertices.push_back({
                positions[index], normal, ultravioletCoordinates[index] });
        }
        geometry.indices.insert(
            geometry.indices.end(),
            { first, first + 1u, first + 2u,
                first + 2u, first + 1u, first + 3u });
    }

    // D3D11のGeometricPrimitiveと同じく、外から見て画面上で時計回りの
    // 三角形へそろえます。CullCounterClockwiseで外側の面が残る向きです。
    void ReverseWinding(Geometry& geometry, const std::size_t firstIndex = 0u)
    {
        for (std::size_t index = firstIndex;
             index + 2u < geometry.indices.size();
             index += 3u)
        {
            std::swap(geometry.indices[index + 1u], geometry.indices[index + 2u]);
        }
    }

    [[nodiscard]] Geometry CreateBox(
        const DirectX::XMFLOAT3& half,
        const bool includeTop)
    {
        Geometry result;
        const float x = half.x;
        const float y = half.y;
        const float z = half.z;
        AddFace(result, { 0, 0, -1 }, { {
            { -x, -y, -z }, { -x, y, -z }, { x, -y, -z }, { x, y, -z } } });
        AddFace(result, { 0, 0, 1 }, { {
            { x, -y, z }, { x, y, z }, { -x, -y, z }, { -x, y, z } } });
        AddFace(result, { -1, 0, 0 }, { {
            { -x, -y, z }, { -x, y, z }, { -x, -y, -z }, { -x, y, -z } } });
        AddFace(result, { 1, 0, 0 }, { {
            { x, -y, -z }, { x, y, -z }, { x, -y, z }, { x, y, z } } });
        if (includeTop)
        {
            AddFace(result, { 0, 1, 0 }, { {
                { -x, y, -z }, { -x, y, z }, { x, y, -z }, { x, y, z } } });
        }
        AddFace(result, { 0, -1, 0 }, { {
            { -x, -y, z }, { -x, -y, -z }, { x, -y, z }, { x, -y, -z } } });
        ReverseWinding(result);
        return result;
    }

    [[nodiscard]] Geometry CreateCube()
    {
        return CreateBox({ 0.5f, 0.5f, 0.5f }, true);
    }

    [[nodiscard]] Geometry CreatePlane()
    {
        // D3D11のCreateBox({ 1, 0.05, 1 })と同じ薄い箱にして、裏から見ても
        // 消えないようにします。上面のUVは従来の板と同じです。
        constexpr float halfThickness = 0.025f;
        auto result = CreateBox({ 0.5f, halfThickness, 0.5f }, false);
        AddFace(result, { 0, 1, 0 }, { {
            { -0.5f, halfThickness, 0.5f }, { -0.5f, halfThickness, -0.5f },
            { 0.5f, halfThickness, 0.5f }, { 0.5f, halfThickness, -0.5f } } });
        return result;
    }

    [[nodiscard]] Geometry CreateSphere()
    {
        Geometry result;
        constexpr std::uint32_t slices = 24;
        constexpr std::uint32_t stacks = 16;
        for (std::uint32_t stack{}; stack <= stacks; ++stack)
        {
            const float v = static_cast<float>(stack) / stacks;
            const float latitude = v * std::numbers::pi_v<float>;
            const float y = std::cos(latitude) * 0.5f;
            const float radius = std::sin(latitude) * 0.5f;
            for (std::uint32_t slice{}; slice <= slices; ++slice)
            {
                const float u = static_cast<float>(slice) / slices;
                const float longitude = u * std::numbers::pi_v<float> * 2.0f;
                const DirectX::XMFLOAT3 position{
                    std::sin(longitude) * radius,
                    y,
                    std::cos(longitude) * radius };
                result.vertices.push_back({
                    position,
                    { position.x * 2.0f, position.y * 2.0f, position.z * 2.0f },
                    { u, v } });
            }
        }
        for (std::uint32_t stack{}; stack < stacks; ++stack)
        {
            for (std::uint32_t slice{}; slice < slices; ++slice)
            {
                const auto first = stack * (slices + 1u) + slice;
                const auto next = first + slices + 1u;
                result.indices.insert(result.indices.end(), {
                    first, next, first + 1u,
                    first + 1u, next, next + 1u });
            }
        }
        ReverseWinding(result);
        return result;
    }

    [[nodiscard]] Geometry CreateCylinder()
    {
        Geometry result;
        constexpr std::uint32_t slices = 24;
        for (std::uint32_t slice{}; slice <= slices; ++slice)
        {
            const float u = static_cast<float>(slice) / slices;
            const float angle = u * std::numbers::pi_v<float> * 2.0f;
            const float x = std::sin(angle) * 0.5f;
            const float z = std::cos(angle) * 0.5f;
            const DirectX::XMFLOAT3 normal{ x * 2.0f, 0.0f, z * 2.0f };
            result.vertices.push_back({ { x, -0.5f, z }, normal, { u, 1 } });
            result.vertices.push_back({ { x, 0.5f, z }, normal, { u, 0 } });
        }
        for (std::uint32_t slice{}; slice < slices; ++slice)
        {
            const auto first = slice * 2u;
            result.indices.insert(result.indices.end(), {
                first, first + 1u, first + 2u,
                first + 2u, first + 1u, first + 3u });
        }
        const auto bottomCenter = static_cast<std::uint32_t>(result.vertices.size());
        result.vertices.push_back({ { 0, -0.5f, 0 }, { 0, -1, 0 }, { 0.5f, 0.5f } });
        const auto topCenter = static_cast<std::uint32_t>(result.vertices.size());
        result.vertices.push_back({ { 0, 0.5f, 0 }, { 0, 1, 0 }, { 0.5f, 0.5f } });
        const auto capIndex = result.indices.size();
        for (std::uint32_t slice{}; slice < slices; ++slice)
        {
            const auto side = slice * 2u;
            const auto next = (slice + 1u) * 2u;
            result.indices.insert(result.indices.end(), {
                bottomCenter, next, side,
                topCenter, side + 1u, next + 1u });
        }
        // 側面は既に外向きです。上下の蓋だけを外向きにそろえます。
        ReverseWinding(result, capIndex);
        return result;
    }

    [[nodiscard]] D3D12_BLEND_DESC MakeBlendDescription(bool enabled) noexcept
    {
        D3D12_RENDER_TARGET_BLEND_DESC target{};
        target.BlendEnable = enabled;
        target.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        target.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        target.BlendOp = D3D12_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D12_BLEND_ONE;
        target.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        D3D12_BLEND_DESC result{};
        for (auto& renderTarget : result.RenderTarget)
        {
            renderTarget = target;
        }
        return result;
    }

    [[nodiscard]] D3D12_BLEND_DESC MakeParticleBlendDescription(
        bool additive) noexcept
    {
        D3D12_RENDER_TARGET_BLEND_DESC target{};
        target.BlendEnable = TRUE;
        target.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        target.DestBlend = additive
            ? D3D12_BLEND_ONE
            : D3D12_BLEND_INV_SRC_ALPHA;
        target.BlendOp = D3D12_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D12_BLEND_ONE;
        target.DestBlendAlpha = additive
            ? D3D12_BLEND_ONE
            : D3D12_BLEND_INV_SRC_ALPHA;
        target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        D3D12_BLEND_DESC result{};
        for (auto& renderTarget : result.RenderTarget)
        {
            renderTarget = target;
        }
        return result;
    }

    // wireframeはDirectXTKのCommonStates::Wireframeと同じく、カリングせず
    // 辺だけを描きます。それ以外はCommonStatesのCullCounterClockwise／
    // CullClockwiseと同じく、時計回りを表面として扱います。
    [[nodiscard]] D3D12_RASTERIZER_DESC MakeRasterizerDescription(
        const bool wireframe = false,
        const LamaPon::ShaderCullMode cull =
            LamaPon::ShaderCullMode::None) noexcept
    {
        D3D12_RASTERIZER_DESC result{};
        result.FillMode = wireframe
            ? D3D12_FILL_MODE_WIREFRAME
            : D3D12_FILL_MODE_SOLID;
        result.CullMode = wireframe || cull == LamaPon::ShaderCullMode::None
            ? D3D12_CULL_MODE_NONE
            : cull == LamaPon::ShaderCullMode::Front
                ? D3D12_CULL_MODE_FRONT
                : D3D12_CULL_MODE_BACK;
        result.FrontCounterClockwise = FALSE;
        // DirectXTKのCommonStatesと同じく、辺は四角形の線で描きます。
        result.MultisampleEnable = wireframe ? TRUE : FALSE;
        result.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        result.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        result.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        result.DepthClipEnable = TRUE;
        return result;
    }

    [[nodiscard]] D3D12_DEPTH_STENCIL_DESC MakeDepthDescription(
        bool depthTest,
        bool depthWrite) noexcept
    {
        D3D12_DEPTH_STENCIL_DESC result{};
        result.DepthEnable = depthTest;
        result.DepthWriteMask = depthWrite
            ? D3D12_DEPTH_WRITE_MASK_ALL
            : D3D12_DEPTH_WRITE_MASK_ZERO;
        result.DepthFunc = depthTest
            ? D3D12_COMPARISON_FUNC_LESS_EQUAL
            : D3D12_COMPARISON_FUNC_ALWAYS;
        result.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        result.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        return result;
    }

    class D3D12RenderServices final
        : public LamaPon::GraphicsRenderServices
        , public LamaPon::Detail::D3D12MaterialShaderServices
    {
    public:
        explicit D3D12RenderServices(LamaPon::D3D12Backend& backend)
            : m_backend(&backend)
            , m_cube(CreateCube())
            , m_sphere(CreateSphere())
            , m_cylinder(CreateCylinder())
            , m_plane(CreatePlane())
        {
            if (!backend.IsInitialized())
            {
                throw std::invalid_argument(
                    "The DirectX 12 render service requires an initialized backend.");
            }
            m_vertexShader = CompileShader(
                PrimitiveShaderSource,
                sizeof(PrimitiveShaderSource) - 1u,
                "LamaPonD3D12Primitive",
                "PrimitiveVertexShader",
                "vs_5_0");
            m_pixelShader = CompileShader(
                PrimitiveShaderSource,
                sizeof(PrimitiveShaderSource) - 1u,
                "LamaPonD3D12Primitive",
                "PrimitivePixelShader",
                "ps_5_0");
            m_instancedVertexShader = CompileShader(
                PrimitiveShaderSource,
                sizeof(PrimitiveShaderSource) - 1u,
                "LamaPonD3D12Primitive",
                "PrimitiveInstancedVertexShader",
                "vs_5_0");
            m_particleVertexShader = CompileShader(
                ParticleShaderSource,
                sizeof(ParticleShaderSource) - 1u,
                "LamaPonD3D12Particle",
                "ParticleVertexShader",
                "vs_5_0");
            m_particlePixelShader = CompileShader(
                ParticleShaderSource,
                sizeof(ParticleShaderSource) - 1u,
                "LamaPonD3D12Particle",
                "ParticlePixelShader",
                "ps_5_0");
            m_customParticleVertexShader = CompileShader(
                ParticleShaderSource,
                sizeof(ParticleShaderSource) - 1u,
                "LamaPonD3D12Particle",
                "CustomParticleVertexShader",
                "vs_5_0");

            // t0〜t5はMaterial、t6〜t8は影、t9はSSAO、t10とt11はSSR、
            // t12とt13はIBL、t14〜t16はForward+のクラスタライト、t17〜t19は
            // ベイクした間接光、t20とt21は混ぜる2個目のリフレクションプローブ
            // です。
            std::array<D3D12_DESCRIPTOR_RANGE, 22> textureRanges{};
            for (UINT index{}; index < textureRanges.size(); ++index)
            {
                textureRanges[index].RangeType =
                    D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                textureRanges[index].NumDescriptors = 1;
                textureRanges[index].BaseShaderRegister = index;
            }
            std::array<D3D12_ROOT_PARAMETER, 23> parameters{};
            parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            parameters[0].Descriptor.ShaderRegister = 0;
            parameters[0].Descriptor.RegisterSpace = 0;
            parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            for (std::size_t index{}; index < textureRanges.size(); ++index)
            {
                auto& parameter = parameters[index + 1u];
                parameter.ParameterType =
                    D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                parameter.DescriptorTable.NumDescriptorRanges = 1;
                parameter.DescriptorTable.pDescriptorRanges =
                    &textureRanges[index];
                parameter.ShaderVisibility =
                    D3D12_SHADER_VISIBILITY_PIXEL;
            }
            std::array<D3D12_STATIC_SAMPLER_DESC, 2> samplers{};
            auto& materialSampler = samplers[0];
            materialSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            materialSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            materialSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            materialSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            materialSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            materialSampler.MaxLOD = D3D12_FLOAT32_MAX;
            materialSampler.ShaderRegister = 0;
            materialSampler.ShaderVisibility =
                D3D12_SHADER_VISIBILITY_PIXEL;
            auto& shadowSampler = samplers[1];
            shadowSampler.Filter =
                D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
            shadowSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            shadowSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            shadowSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            shadowSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            shadowSampler.BorderColor =
                D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
            shadowSampler.MaxLOD = 0.0f;
            shadowSampler.ShaderRegister = 1;
            shadowSampler.ShaderVisibility =
                D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_ROOT_SIGNATURE_DESC description{};
            description.NumParameters = static_cast<UINT>(parameters.size());
            description.pParameters = parameters.data();
            description.NumStaticSamplers =
                static_cast<UINT>(samplers.size());
            description.pStaticSamplers = samplers.data();
            description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
            Microsoft::WRL::ComPtr<ID3DBlob> serialized;
            Microsoft::WRL::ComPtr<ID3DBlob> errors;
            ThrowIfFailed(
                D3D12SerializeRootSignature(
                    &description, D3D_ROOT_SIGNATURE_VERSION_1,
                    serialized.GetAddressOf(), errors.GetAddressOf()),
                "D3D12SerializeRootSignature(primitive)");
            ThrowIfFailed(
                backend.Device()->CreateRootSignature(
                    0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                    IID_PPV_ARGS(m_rootSignature.ReleaseAndGetAddressOf())),
                "ID3D12Device::CreateRootSignature(primitive)");

            // ParticleSystemのcustom pixel shader用です。D3D11と同じく、頂点
            // シェーダーのb0（ViewProjection）とピクセルシェーダーのb0／b1
            // （カスタム値／Light2D）を別の枠にし、t0／t1とs0を渡します。
            {
                std::array<D3D12_DESCRIPTOR_RANGE, 2> particleRanges{};
                for (UINT index{}; index < particleRanges.size(); ++index)
                {
                    particleRanges[index].RangeType =
                        D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                    particleRanges[index].NumDescriptors = 1;
                    particleRanges[index].BaseShaderRegister = index;
                }
                std::array<D3D12_ROOT_PARAMETER, 5> particleParameters{};
                particleParameters[0].ParameterType =
                    D3D12_ROOT_PARAMETER_TYPE_CBV;
                particleParameters[0].Descriptor.ShaderRegister = 0;
                particleParameters[0].ShaderVisibility =
                    D3D12_SHADER_VISIBILITY_VERTEX;
                for (UINT index{}; index < 2u; ++index)
                {
                    auto& parameter = particleParameters[index + 1u];
                    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                    parameter.Descriptor.ShaderRegister = index;
                    parameter.ShaderVisibility =
                        D3D12_SHADER_VISIBILITY_PIXEL;
                }
                for (UINT index{}; index < particleRanges.size(); ++index)
                {
                    auto& parameter = particleParameters[index + 3u];
                    parameter.ParameterType =
                        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                    parameter.DescriptorTable.NumDescriptorRanges = 1;
                    parameter.DescriptorTable.pDescriptorRanges =
                        &particleRanges[index];
                    parameter.ShaderVisibility =
                        D3D12_SHADER_VISIBILITY_PIXEL;
                }
                // DirectXTKのCommonStates::LinearWrapと同じです。
                D3D12_STATIC_SAMPLER_DESC particleSampler{};
                particleSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                particleSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                particleSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                particleSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                particleSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
                particleSampler.MaxLOD = D3D12_FLOAT32_MAX;
                particleSampler.ShaderRegister = 0;
                particleSampler.ShaderVisibility =
                    D3D12_SHADER_VISIBILITY_PIXEL;
                D3D12_ROOT_SIGNATURE_DESC particleDescription{};
                particleDescription.NumParameters =
                    static_cast<UINT>(particleParameters.size());
                particleDescription.pParameters = particleParameters.data();
                particleDescription.NumStaticSamplers = 1;
                particleDescription.pStaticSamplers = &particleSampler;
                particleDescription.Flags =
                    D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
                Microsoft::WRL::ComPtr<ID3DBlob> particleSignature;
                Microsoft::WRL::ComPtr<ID3DBlob> particleErrors;
                ThrowIfFailed(
                    D3D12SerializeRootSignature(
                        &particleDescription,
                        D3D_ROOT_SIGNATURE_VERSION_1,
                        particleSignature.GetAddressOf(),
                        particleErrors.GetAddressOf()),
                    "D3D12SerializeRootSignature(custom particle)");
                ThrowIfFailed(
                    backend.Device()->CreateRootSignature(
                        0,
                        particleSignature->GetBufferPointer(),
                        particleSignature->GetBufferSize(),
                        IID_PPV_ARGS(m_customParticleRootSignature
                            .ReleaseAndGetAddressOf())),
                    "ID3D12Device::CreateRootSignature(custom particle)");
            }
            m_materialShaders = std::make_unique<
                LamaPon::Detail::D3D12MaterialShaderRenderer>(backend);
        }

        [[nodiscard]] LamaPon::Detail::MaterialShaderDrawResult
            DrawMaterialShader(
                LamaPon::AssetManager& assets,
                const LamaPon::Detail::MaterialShaderSource& shader,
                const LamaPon::Detail::MaterialShaderSource& placeholder,
                const bool prepass,
                const LamaPon::PrimitiveDrawRequest& request,
                const LamaPon::Detail::MaterialShaderDrawRequest& material,
                const LamaPon::LightingState& lighting) override
        {
            std::span<const PrimitiveRenderVertex> vertices;
            std::span<const std::uint32_t> indices;
            // glTF／FBXのスキニング経路は、要求が頂点列を直接持ちます。
            if (material.skinned == nullptr
                && !ResolveGeometry(request, vertices, indices))
            {
                return {};
            }
            return m_materialShaders->Draw(
                assets,
                shader,
                placeholder,
                prepass,
                request,
                vertices,
                indices,
                material,
                lighting);
        }

        void InvalidateMaterialShader(
            const std::filesystem::path& shaderPath) noexcept override
        {
            m_materialShaders->Invalidate(shaderPath);
        }

        [[nodiscard]] bool TryGetMaterialShaderRenderState(
            const std::filesystem::path& cacheKey,
            LamaPon::ShaderRenderState& state) const noexcept override
        {
            return m_materialShaders->TryGetRenderState(cacheKey, state);
        }

        [[nodiscard]] LamaPon::Detail::MaterialShaderPasses
            PrepareMaterialShaderPasses(
                LamaPon::AssetManager& assets,
                const LamaPon::Detail::MaterialShaderSource& shader) override
        {
            return m_materialShaders->PreparePasses(assets, shader);
        }

        [[nodiscard]] LamaPon::Detail::MaterialShaderDrawResult
            DrawCustomParticles(
                LamaPon::AssetManager& assets,
                const LamaPon::Detail::MaterialShaderSource& shader,
                const LamaPon::Detail::MaterialShaderSource& placeholder,
                const LamaPon::ParticleDrawRequest& request,
                const std::array<DirectX::XMFLOAT4, 8>& parameters) override
        {
            LamaPon::Detail::MaterialShaderDrawResult result;
            auto* active = &PrepareCustomParticleShader(assets, shader);
            result.generation = active->generation;
            result.error = active->error;
            // 既定のparticleと同じく、半透明particleはshadow casterにしません。
            if (m_backend->IsShadowPassActive() || request.vertices.empty())
            {
                result.drawn = true;
                return result;
            }
            RequireCompleteParticleQuads(request);

            const auto usePlaceholder = [&]()
            {
                auto& fallback =
                    PrepareCustomParticleShader(assets, placeholder);
                if (fallback.pixelShader == nullptr)
                {
                    return false;
                }
                active = &fallback;
                result.placeholder = true;
                return true;
            };
            // D3D11のApplyCustomPixelShaderと同じく、説明の付いた失敗だけを
            // マゼンタの代替表示で描きます。
            if (active->pixelShader == nullptr
                && (active->error.empty() || !usePlaceholder()))
            {
                return result;
            }
            ID3D12PipelineState* pipeline{};
            try
            {
                pipeline = CustomParticlePipelineState(
                    *active,
                    request.additive);
            }
            catch (const std::exception& exception)
            {
                if (result.placeholder)
                {
                    throw;
                }
                // 頂点出力との並びが合わないなど、pipelineを作れないShaderも
                // compile失敗と同じく説明を出して代替表示で描きます。
                active->error = shader.describeFailure
                    ? shader.describeFailure(exception.what())
                    : std::string(exception.what());
                active->pixelShader.Reset();
                for (auto& state : active->pipelineStates)
                {
                    state.Reset();
                }
                result.error = active->error;
                if (!usePlaceholder())
                {
                    return result;
                }
                pipeline = CustomParticlePipelineState(
                    *active,
                    request.additive);
            }

            // D3D11と同じく、t0はparticle texture、t1は補助textureで、
            // 未設定の枠は白です。
            const auto resolve = [this, &request](
                const LamaPon::GraphicsViewHandle& view)
            {
                return m_backend->TryResolveShaderResource(
                    view ? view : request.fallbackTexture);
            };
            const auto texture = resolve(request.texture);
            const auto auxiliaryTexture = resolve(request.auxiliaryTexture);
            auto* const descriptorHeap =
                m_backend->ShaderResourceDescriptorHeap();
            if (!texture || !auxiliaryTexture || descriptorHeap == nullptr)
            {
                return result;
            }

            auto* const commandList = m_backend->BeginFrameCommands();
            const auto geometry = UploadParticleGeometry(request);
            const auto viewProjection = UploadParticleViewProjection(request);
            const auto parameterUpload = m_backend->AllocateFrameUpload(
                sizeof(parameters),
                D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            std::memcpy(
                parameterUpload.data,
                parameters.data(),
                sizeof(parameters));
            // D3D11のApplyCustomPixelShaderはLight2Dを設定しないため、灯の
            // 無い一覧を渡します。
            const LamaPon::Sprite2DLighting lighting{};
            const auto lightingUpload = m_backend->AllocateFrameUpload(
                sizeof(lighting),
                D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            std::memcpy(lightingUpload.data, &lighting, sizeof(lighting));

            ID3D12DescriptorHeap* heaps[]{ descriptorHeap };
            commandList->SetGraphicsRootSignature(
                m_customParticleRootSignature.Get());
            commandList->SetPipelineState(pipeline);
            commandList->SetDescriptorHeaps(1, heaps);
            commandList->SetGraphicsRootConstantBufferView(
                0,
                viewProjection.gpuAddress);
            commandList->SetGraphicsRootConstantBufferView(
                1,
                parameterUpload.gpuAddress);
            commandList->SetGraphicsRootConstantBufferView(
                2,
                lightingUpload.gpuAddress);
            commandList->SetGraphicsRootDescriptorTable(
                3,
                texture->descriptor);
            commandList->SetGraphicsRootDescriptorTable(
                4,
                auxiliaryTexture->descriptor);
            commandList->IASetPrimitiveTopology(
                D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commandList->IASetVertexBuffers(0, 1, &geometry.vertexView);
            commandList->IASetIndexBuffer(&geometry.indexView);
            const auto& viewport = m_backend->ActiveViewport();
            const auto& scissor = m_backend->ActiveScissorRectangle();
            commandList->RSSetViewports(1, &viewport);
            commandList->RSSetScissorRects(1, &scissor);
            commandList->DrawIndexedInstanced(
                geometry.indexCount,
                1,
                0,
                0,
                0);
            result.drawn = true;
            return result;
        }

        void InvalidateCustomPixelShader(
            const std::filesystem::path& shaderPath) noexcept override
        {
            try
            {
                const auto found = m_customParticleShaders.find(shaderPath);
                if (found != m_customParticleShaders.end())
                {
                    found->second.forceReload = true;
                }
            }
            catch (...)
            {
            }
        }

        [[nodiscard]] bool DrawParticles(
            const LamaPon::ParticleDrawRequest& request) override
        {
            // 半透明particleはshadow casterにしません。通常描画入口を
            // 呼ぶとShadowMapのDSVからprimary outputへ戻るため、ここで
            // 成功扱いにしてshadow passを維持します。
            if (m_backend->IsShadowPassActive())
            {
                return true;
            }
            if (request.vertices.empty())
            {
                return true;
            }
            RequireCompleteParticleQuads(request);
            const auto& texture = request.texture
                ? request.texture
                : request.fallbackTexture;
            const auto binding = m_backend->TryResolveShaderResource(texture);
            auto* descriptorHeap = m_backend->ShaderResourceDescriptorHeap();
            if (!binding || descriptorHeap == nullptr)
            {
                return false;
            }

            auto* commandList = m_backend->BeginFrameCommands();
            const auto geometry = UploadParticleGeometry(request);
            const auto& vertexView = geometry.vertexView;
            const auto& indexView = geometry.indexView;
            const auto indexCount = geometry.indexCount;
            const auto constantUpload = UploadParticleViewProjection(request);
            ID3D12DescriptorHeap* heaps[]{ descriptorHeap };
            commandList->SetGraphicsRootSignature(m_rootSignature.Get());
            commandList->SetPipelineState(ParticlePipelineState(request.additive));
            commandList->SetDescriptorHeaps(1, heaps);
            commandList->SetGraphicsRootConstantBufferView(
                0,
                constantUpload.gpuAddress);
            commandList->SetGraphicsRootDescriptorTable(1, binding->descriptor);
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commandList->IASetVertexBuffers(0, 1, &vertexView);
            commandList->IASetIndexBuffer(&indexView);
            const auto& viewport = m_backend->ActiveViewport();
            const auto& scissor = m_backend->ActiveScissorRectangle();
            commandList->RSSetViewports(1, &viewport);
            commandList->RSSetScissorRects(1, &scissor);
            commandList->DrawIndexedInstanced(
                static_cast<UINT>(indexCount),
                1,
                0,
                0,
                0);
            return true;
        }

        [[nodiscard]] bool DrawPrimitive(
            const LamaPon::PrimitiveDrawRequest& request) override
        {
            const Geometry* geometry{};
            switch (request.shape)
            {
            case LamaPon::PrimitiveRenderShape::Cube: geometry = &m_cube; break;
            case LamaPon::PrimitiveRenderShape::Sphere: geometry = &m_sphere; break;
            case LamaPon::PrimitiveRenderShape::Cylinder: geometry = &m_cylinder; break;
            case LamaPon::PrimitiveRenderShape::Plane: geometry = &m_plane; break;
            case LamaPon::PrimitiveRenderShape::Procedural: break;
            default: return false;
            }
            const auto vertices = geometry != nullptr
                ? std::span<const PrimitiveRenderVertex>(geometry->vertices)
                : request.vertices;
            const auto indices = geometry != nullptr
                ? std::span<const std::uint32_t>(geometry->indices)
                : request.indices;
            if (vertices.empty() || indices.empty() || indices.size() % 3u != 0u
                || vertices.size() > std::numeric_limits<UINT>::max()
                || indices.size() > std::numeric_limits<UINT>::max())
            {
                return false;
            }
            // インスタンス描画は、D3D11と同じく色を書く通常のパスだけで行います。
            const bool instanced = !request.instances.empty();
            if (instanced
                && (request.depthOnly
                    || request.instances.size_bytes()
                        > std::numeric_limits<UINT>::max()))
            {
                return false;
            }
            if (std::ranges::any_of(indices, [vertices](const std::uint32_t index)
                { return index >= vertices.size(); }))
            {
                return false;
            }
            const std::array textures{
                request.albedo,
                request.normalTexture,
                request.roughnessTexture,
                request.metallicTexture,
                request.occlusionTexture,
                request.emissiveTexture
            };
            std::array<LamaPon::D3D12Backend::ShaderResourceBinding, 6>
                bindings{};
            LamaPon::D3D12Backend::ShaderResourceBinding shadowBinding{};
            LamaPon::D3D12Backend::ShaderResourceBinding
                spotShadowBinding{};
            LamaPon::D3D12Backend::ShaderResourceBinding
                pointShadowBinding{};
            LamaPon::D3D12Backend::ShaderResourceBinding
                screenAmbientOcclusionBinding{};
            LamaPon::D3D12Backend::ShaderResourceBinding
                screenReflectionColorBinding{};
            LamaPon::D3D12Backend::ShaderResourceBinding
                screenReflectionDepthBinding{};
            bool directionalShadowActive{};
            bool screenAmbientOcclusionActive{};
            bool screenReflectionActive{};
            bool spotShadowTextureCurrent{};
            bool pointShadowTextureCurrent{};
            bool environmentActive{};
            bool prefilteredEnvironmentActive{};
            D3D12_GPU_DESCRIPTOR_HANDLE environmentDescriptor{};
            D3D12_GPU_DESCRIPTOR_HANDLE irradianceDescriptor{};
            bool clusteredActive{};
            D3D12_GPU_DESCRIPTOR_HANDLE clusterLightsDescriptor{};
            D3D12_GPU_DESCRIPTOR_HANDLE clusterIndicesDescriptor{};
            D3D12_GPU_DESCRIPTOR_HANDLE clusterCountsDescriptor{};
            bool bakedGiActive{};
            D3D12_GPU_DESCRIPTOR_HANDLE bakedGiRedDescriptor{};
            D3D12_GPU_DESCRIPTOR_HANDLE bakedGiGreenDescriptor{};
            D3D12_GPU_DESCRIPTOR_HANDLE bakedGiBlueDescriptor{};
            LamaPon::Detail::D3D12ReflectionProbeBindings probe;
            D3D12_GPU_DESCRIPTOR_HANDLE secondaryEnvironmentDescriptor{};
            D3D12_GPU_DESCRIPTOR_HANDLE secondaryIrradianceDescriptor{};
            ID3D12DescriptorHeap* descriptorHeap{};
            if (!request.depthOnly)
            {
                for (std::size_t index{}; index < textures.size(); ++index)
                {
                    const auto& texture = textures[index]
                        ? textures[index]
                        : request.fallbackTexture;
                    const auto binding =
                        m_backend->TryResolveShaderResource(texture);
                    if (!binding)
                    {
                        return false;
                    }
                    bindings[index] = *binding;
                }
                descriptorHeap =
                    m_backend->ShaderResourceDescriptorHeap();
                if (descriptorHeap == nullptr)
                {
                    return false;
                }
                const auto resolvedShadow =
                    m_backend->TryResolveShaderResource(
                        request.directionalShadow.texture);
                if (resolvedShadow)
                {
                    shadowBinding = *resolvedShadow;
                    directionalShadowActive =
                        request.directionalShadow.enabled
                        && request.directionalShadow.lightIndex
                            < request.directionalLightCount
                        && request.directionalShadow.cascadeCount != 0u;
                }
                else
                {
                    const auto fallbackShadow =
                        m_backend->TryResolveShaderResource(
                            request.fallbackTexture);
                    if (!fallbackShadow)
                    {
                        return false;
                    }
                    shadowBinding = *fallbackShadow;
                }
                const auto resolvedSpotShadow =
                    m_backend->TryResolveShaderResource(
                        request.spotShadowTexture);
                if (resolvedSpotShadow)
                {
                    spotShadowBinding = *resolvedSpotShadow;
                    spotShadowTextureCurrent = true;
                }
                else
                {
                    const auto fallbackSpotShadow =
                        m_backend->TryResolveShaderResource(
                            request.fallbackTexture);
                    if (!fallbackSpotShadow)
                    {
                        return false;
                    }
                    spotShadowBinding = *fallbackSpotShadow;
                }
                const auto resolvedPointShadow =
                    m_backend->TryResolveShaderResource(
                        request.pointShadow.texture);
                if (resolvedPointShadow)
                {
                    pointShadowBinding = *resolvedPointShadow;
                    pointShadowTextureCurrent = true;
                }
                else
                {
                    const auto fallbackPointShadow =
                        m_backend->TryResolveShaderResource(
                            request.fallbackTexture);
                    if (!fallbackPointShadow)
                    {
                        return false;
                    }
                    pointShadowBinding = *fallbackPointShadow;
                }
                // 深度プリパスが遮蔽を求めたframeだけSSAOを読みます。
                // 別世代や未解決のviewは白へ置き換え、shaderも読みません。
                const auto resolvedScreenAmbientOcclusion =
                    m_backend->TryResolveShaderResource(
                        request.screenAmbientOcclusion.texture);
                if (resolvedScreenAmbientOcclusion)
                {
                    screenAmbientOcclusionBinding =
                        *resolvedScreenAmbientOcclusion;
                    screenAmbientOcclusionActive =
                        request.screenAmbientOcclusion.enabled;
                }
                else
                {
                    const auto fallbackScreenAmbientOcclusion =
                        m_backend->TryResolveShaderResource(
                            request.fallbackTexture);
                    if (!fallbackScreenAmbientOcclusion)
                    {
                        return false;
                    }
                    screenAmbientOcclusionBinding =
                        *fallbackScreenAmbientOcclusion;
                }
                // SSRは前フレームのカラーとHi-Z深度ピラミッドを両方解決
                // できたframeだけ有効にし、それ以外は白で埋めます。
                const auto resolvedReflectionColor =
                    m_backend->TryResolveShaderResource(
                        request.screenSpaceReflection.texture);
                const auto resolvedReflectionDepth =
                    m_backend->TryResolveShaderResource(
                        request.screenSpaceReflection.depth);
                screenReflectionActive =
                    request.screenSpaceReflection.enabled
                    && resolvedReflectionColor
                    && resolvedReflectionDepth;
                if (screenReflectionActive)
                {
                    screenReflectionColorBinding = *resolvedReflectionColor;
                    screenReflectionDepthBinding = *resolvedReflectionDepth;
                }
                else
                {
                    const auto fallbackReflection =
                        m_backend->TryResolveShaderResource(
                            request.fallbackTexture);
                    if (!fallbackReflection)
                    {
                        return false;
                    }
                    screenReflectionColorBinding = *fallbackReflection;
                    screenReflectionDepthBinding = *fallbackReflection;
                }
                // D3D11のLitEffectと同じく、cubemapを読めるときだけIBLを使い、
                // 事前畳み込みの2本が揃えばそちらを読みます。使わない枠は
                // null TextureCubeです。
                const auto resolvedEnvironment =
                    m_backend->TryResolveShaderResource(
                        request.environment.texture);
                const auto resolvedSpecular =
                    m_backend->TryResolveShaderResource(
                        request.environment.specular);
                const auto resolvedIrradiance =
                    m_backend->TryResolveShaderResource(
                        request.environment.irradiance);
                const auto isCube = [](const auto& binding)
                {
                    return binding.has_value()
                        && binding->dimension
                            == D3D12_SRV_DIMENSION_TEXTURECUBE;
                };
                environmentActive = request.environment.enabled
                    && isCube(resolvedEnvironment);
                prefilteredEnvironmentActive = environmentActive
                    && isCube(resolvedSpecular)
                    && isCube(resolvedIrradiance);
                const auto nullCube =
                    m_backend->NullShaderResourceDescriptor(
                        D3D12_SRV_DIMENSION_TEXTURECUBE);
                environmentDescriptor = environmentActive
                    ? (prefilteredEnvironmentActive
                        ? resolvedSpecular->descriptor
                        : resolvedEnvironment->descriptor)
                    : nullCube;
                irradianceDescriptor = prefilteredEnvironmentActive
                    ? resolvedIrradiance->descriptor
                    : nullCube;
                // D3D11のTrySetLitEffectReflectionProbeと同じく、範囲に入った
                // プローブを解決できたときだけSkyのIBLを差し替えます。
                probe = LamaPon::Detail::ResolveD3D12ReflectionProbe(
                    *m_backend,
                    request.reflectionProbe);
                if (probe.active)
                {
                    environmentActive = true;
                    prefilteredEnvironmentActive = true;
                    environmentDescriptor = probe.specular;
                    irradianceDescriptor = probe.irradiance;
                }
                secondaryEnvironmentDescriptor = probe.blended
                    ? probe.secondarySpecular
                    : nullCube;
                secondaryIrradianceDescriptor = probe.blended
                    ? probe.secondaryIrradiance
                    : nullCube;
                // D3D11のLitEffectと同じく、3本のviewを解決できたframeだけ
                // Forward+を使います。使わない枠はnull bufferです。
                const auto isBuffer = [](const auto& binding)
                {
                    return binding.has_value()
                        && binding->dimension == D3D12_SRV_DIMENSION_BUFFER;
                };
                const auto resolvedClusterLights =
                    m_backend->TryResolveShaderResource(
                        request.clustered.lights);
                const auto resolvedClusterIndices =
                    m_backend->TryResolveShaderResource(
                        request.clustered.lightIndices);
                const auto resolvedClusterCounts =
                    m_backend->TryResolveShaderResource(
                        request.clustered.clusterCounts);
                clusteredActive = request.clustered.enabled
                    && isBuffer(resolvedClusterLights)
                    && isBuffer(resolvedClusterIndices)
                    && isBuffer(resolvedClusterCounts);
                const auto nullBuffer =
                    m_backend->NullShaderResourceDescriptor(
                        D3D12_SRV_DIMENSION_BUFFER);
                clusterLightsDescriptor = clusteredActive
                    ? resolvedClusterLights->descriptor
                    : nullBuffer;
                clusterIndicesDescriptor = clusteredActive
                    ? resolvedClusterIndices->descriptor
                    : nullBuffer;
                clusterCountsDescriptor = clusteredActive
                    ? resolvedClusterCounts->descriptor
                    : nullBuffer;
                // D3D11のLitEffectと同じく、RGB別の3本のTexture3Dを解決
                // できたframeだけベイクした間接光を読みます。使わない枠は
                // null Texture3Dです。
                const auto isVolume = [](const auto& binding)
                {
                    return binding.has_value()
                        && binding->dimension
                            == D3D12_SRV_DIMENSION_TEXTURE3D;
                };
                const auto& bakedGi = request.bakedGlobalIllumination;
                const auto resolvedBakedGiRed =
                    m_backend->TryResolveShaderResource(
                        bakedGi.redCoefficients);
                const auto resolvedBakedGiGreen =
                    m_backend->TryResolveShaderResource(
                        bakedGi.greenCoefficients);
                const auto resolvedBakedGiBlue =
                    m_backend->TryResolveShaderResource(
                        bakedGi.blueCoefficients);
                bakedGiActive = bakedGi.enabled
                    && isVolume(resolvedBakedGiRed)
                    && isVolume(resolvedBakedGiGreen)
                    && isVolume(resolvedBakedGiBlue);
                const auto nullVolume =
                    m_backend->NullShaderResourceDescriptor(
                        D3D12_SRV_DIMENSION_TEXTURE3D);
                bakedGiRedDescriptor = bakedGiActive
                    ? resolvedBakedGiRed->descriptor
                    : nullVolume;
                bakedGiGreenDescriptor = bakedGiActive
                    ? resolvedBakedGiGreen->descriptor
                    : nullVolume;
                bakedGiBlueDescriptor = bakedGiActive
                    ? resolvedBakedGiBlue->descriptor
                    : nullVolume;
            }
            else if (!m_backend->IsDepthOnlyPassActive())
            {
                return false;
            }

            auto* commandList = request.depthOnly
                ? m_backend->CurrentFrameCommands()
                : m_backend->BeginFrameCommands();
            const auto vertexBytes = static_cast<std::uint64_t>(vertices.size_bytes());
            const auto indexBytes = static_cast<std::uint64_t>(indices.size_bytes());
            if (vertexBytes > std::numeric_limits<UINT>::max()
                || indexBytes > std::numeric_limits<UINT>::max())
            {
                throw std::length_error("The DirectX 12 primitive is too large.");
            }
            const auto vertexUpload = m_backend->AllocateFrameUpload(
                vertexBytes, alignof(PrimitiveRenderVertex));
            const auto indexUpload = m_backend->AllocateFrameUpload(
                indexBytes, alignof(std::uint32_t));
            std::memcpy(vertexUpload.data, vertices.data(), vertices.size_bytes());
            std::memcpy(indexUpload.data, indices.data(), indices.size_bytes());

            struct Constants final
            {
                DirectX::XMFLOAT4X4 world;
                DirectX::XMFLOAT4 baseColor;
                DirectX::XMFLOAT4 cameraPosition;
                DirectX::XMFLOAT4 materialProperties;
                DirectX::XMFLOAT4 emissiveFactor;
                DirectX::XMFLOAT4 ambientColorIntensity;
                std::array<DirectX::XMFLOAT4, 4>
                    directionalDirectionIntensity{};
                std::array<DirectX::XMFLOAT4, 4> directionalColors{};
                std::array<std::uint32_t, 4> lightCounts{};
                std::array<DirectX::XMFLOAT4, 16> pointPositionRange{};
                std::array<DirectX::XMFLOAT4, 16> pointColorIntensity{};
                std::array<DirectX::XMFLOAT4, 8> spotPositionRange{};
                std::array<DirectX::XMFLOAT4, 8> spotDirectionInnerCosine{};
                std::array<DirectX::XMFLOAT4, 8> spotColorIntensity{};
                std::array<DirectX::XMFLOAT4, 8> spotOuterCosine{};
                std::array<DirectX::XMFLOAT4X4, 4>
                    shadowViewProjections{};
                DirectX::XMFLOAT4 shadowCascadeSplits{};
                DirectX::XMFLOAT4 shadowParameters{};
                DirectX::XMFLOAT4 cameraForwardShadowTexel{};
                std::array<DirectX::XMFLOAT4X4, 4>
                    spotShadowViewProjections{};
                std::array<DirectX::XMFLOAT4, 4>
                    spotShadowParameters{};
                DirectX::XMFLOAT4 pointShadowParameters{};
                DirectX::XMFLOAT4 localShadowTexelSizes{};
                DirectX::XMFLOAT4 screenAmbientOcclusionParameters{};
                DirectX::XMFLOAT4X4 viewProjection{};
                DirectX::XMFLOAT4 screenReflectionParameters{};
                DirectX::XMFLOAT4 screenReflectionScreen{};
                DirectX::XMFLOAT4 screenReflectionQuality{};
                DirectX::XMFLOAT4X4 screenReflectionPreviousViewProjection{};
                DirectX::XMFLOAT4 environmentParameters{};
                DirectX::XMFLOAT4 fogColorModel{};
                DirectX::XMFLOAT4 fogParameters{};
                DirectX::XMFLOAT4X4 worldInverseTranspose{};
                DirectX::XMFLOAT4 clusteredParameters{};
                DirectX::XMFLOAT4 clusteredDepthParameters{};
                DirectX::XMFLOAT4 clusteredScreenParameters{};
                DirectX::XMFLOAT4 bakedGiVolumeMinimum{};
                DirectX::XMFLOAT4 bakedGiInverseSize{};
                DirectX::XMFLOAT4 bakedGiResolution{};
                DirectX::XMFLOAT4 reflectionBoxCenter{};
                DirectX::XMFLOAT4 reflectionBoxParameters{};
                DirectX::XMFLOAT4 reflectionSecondaryBoxCenter{};
                DirectX::XMFLOAT4 reflectionSecondaryBoxParameters{};
                DirectX::XMFLOAT4 reflectionBlendParameters{};
            } constants{};
            // HLSLのPrimitiveConstantsと同じ並び・大きさであることを保証します。
            static_assert(sizeof(constants) == 2448u);
            const auto view = DirectX::XMLoadFloat4x4(&request.view);
            const auto projection = DirectX::XMLoadFloat4x4(&request.projection);
            constants.world = request.world;
            // LitEffect::SetMatricesと同じ逆転置行列です。
            DirectX::XMVECTOR worldDeterminant{};
            DirectX::XMStoreFloat4x4(
                &constants.worldInverseTranspose,
                DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(
                    &worldDeterminant,
                    DirectX::XMLoadFloat4x4(&request.world))));
            constants.baseColor = request.baseColor;
            const auto inverseView = DirectX::XMMatrixInverse(nullptr, view);
            DirectX::XMStoreFloat4(
                &constants.cameraPosition,
                inverseView.r[3]);
            DirectX::XMStoreFloat4(
                &constants.cameraForwardShadowTexel,
                DirectX::XMVector3Normalize(
                    DirectX::XMVectorNegate(inverseView.r[2])));
            constants.materialProperties = {
                std::clamp(request.roughness, 0.02f, 1.0f),
                std::clamp(request.metallic, 0.0f, 1.0f),
                std::clamp(request.occlusionStrength, 0.0f, 1.0f),
                request.normalTexture ? 1.0f : 0.0f };
            constants.emissiveFactor = {
                std::max(request.emissiveFactor.x, 0.0f),
                std::max(request.emissiveFactor.y, 0.0f),
                std::max(request.emissiveFactor.z, 0.0f),
                std::clamp(request.normalStrength, 0.0f, 2.0f) };
            // D3D11のLitEffectと同じく、負の環境光強度は0へ丸めます。
            constants.ambientColorIntensity = {
                request.ambientColor.x,
                request.ambientColor.y,
                request.ambientColor.z,
                std::max(request.ambientIntensity, 0.0f) };
            constants.lightCounts[0] = static_cast<std::uint32_t>(
                std::min(
                    request.directionalLightCount,
                    request.directionalLights.size()));
            for (std::size_t index{};
                index < constants.lightCounts[0];
                ++index)
            {
                const auto& light = request.directionalLights[index];
                constants.directionalDirectionIntensity[index] = {
                    light.direction.x,
                    light.direction.y,
                    light.direction.z,
                    light.intensity };
                // D3D11のLitEffectと同じく、wへ太陽の角半径を載せます。
                constants.directionalColors[index] = {
                    light.color.x,
                    light.color.y,
                    light.color.z,
                    light.angularRadius };
            }
            constants.lightCounts[1] = static_cast<std::uint32_t>(
                std::min(
                    request.pointLightCount,
                    request.pointLights.size()));
            for (std::size_t index{};
                index < constants.lightCounts[1];
                ++index)
            {
                const auto& light = request.pointLights[index];
                constants.pointPositionRange[index] = {
                    light.position.x,
                    light.position.y,
                    light.position.z,
                    light.range };
                constants.pointColorIntensity[index] = {
                    light.color.x,
                    light.color.y,
                    light.color.z,
                    light.intensity };
            }
            constants.lightCounts[2] = static_cast<std::uint32_t>(
                std::min(
                    request.spotLightCount,
                    request.spotLights.size()));
            for (std::size_t index{};
                index < constants.lightCounts[2];
                ++index)
            {
                const auto& light = request.spotLights[index];
                constants.spotPositionRange[index] = {
                    light.position.x,
                    light.position.y,
                    light.position.z,
                    light.range };
                constants.spotDirectionInnerCosine[index] = {
                    light.direction.x,
                    light.direction.y,
                    light.direction.z,
                    light.innerConeCosine };
                constants.spotColorIntensity[index] = {
                    light.color.x,
                    light.color.y,
                    light.color.z,
                    light.intensity };
                constants.spotOuterCosine[index] = {
                    light.outerConeCosine,
                    0.0f,
                    0.0f,
                    0.0f };
            }
            const auto& shadow = request.directionalShadow;
            constants.lightCounts[3] = directionalShadowActive
                ? static_cast<std::uint32_t>(std::min<std::size_t>(
                    shadow.cascadeCount,
                    constants.shadowViewProjections.size()))
                : 0u;
            constants.shadowViewProjections =
                shadow.lightViewProjections;
            constants.shadowCascadeSplits = {
                shadow.cascadeSplits[0],
                shadow.cascadeSplits[1],
                shadow.cascadeSplits[2],
                shadow.cascadeSplits[3] };
            constants.shadowParameters = {
                directionalShadowActive
                    ? static_cast<float>(shadow.lightIndex + 1u)
                    : 0.0f,
                shadow.bias,
                shadow.normalBias,
                shadow.strength };
            constants.cameraForwardShadowTexel.w =
                std::max(shadow.inverseResolution, 0.0f);
            for (std::size_t slot{};
                slot < request.spotShadows.size();
                ++slot)
            {
                const auto& spotShadow = request.spotShadows[slot];
                if (!spotShadowTextureCurrent
                    || !spotShadow.enabled
                    || spotShadow.lightIndex < 0
                    || static_cast<std::size_t>(spotShadow.lightIndex)
                        >= constants.lightCounts[2])
                {
                    continue;
                }
                constants.spotShadowViewProjections[slot] =
                    spotShadow.lightViewProjection;
                constants.spotShadowParameters[slot] = {
                    spotShadow.bias,
                    spotShadow.normalBias,
                    spotShadow.strength,
                    1.0f };
                constants.spotOuterCosine[
                    static_cast<std::size_t>(spotShadow.lightIndex)].y =
                    static_cast<float>(slot + 1u);
            }
            constants.localShadowTexelSizes.x =
                std::max(request.localShadowInverseResolution, 0.0f);
            const auto& pointShadow = request.pointShadow;
            const bool pointShadowActive = pointShadowTextureCurrent
                && pointShadow.enabled
                && pointShadow.lightIndex >= 0
                && static_cast<std::size_t>(pointShadow.lightIndex)
                    < constants.lightCounts[1];
            constants.pointShadowParameters = {
                pointShadowActive
                    ? static_cast<float>(pointShadow.lightIndex + 1)
                    : 0.0f,
                pointShadow.bias,
                pointShadow.strength,
                0.0f };
            constants.localShadowTexelSizes.y =
                std::max(request.localShadowInverseResolution, 0.0f);
            constants.screenAmbientOcclusionParameters = {
                screenAmbientOcclusionActive
                    ? std::max(
                        request.screenAmbientOcclusion.inverseWidth,
                        0.0f)
                    : 0.0f,
                screenAmbientOcclusionActive
                    ? std::max(
                        request.screenAmbientOcclusion.inverseHeight,
                        0.0f)
                    : 0.0f,
                screenAmbientOcclusionActive ? 1.0f : 0.0f,
                0.0f };
            DirectX::XMStoreFloat4x4(
                &constants.viewProjection,
                DirectX::XMMatrixMultiply(view, projection));
            // D3D11のLitEffectと同じ範囲へ丸めます。
            const auto& reflection = request.screenSpaceReflection;
            constants.screenReflectionParameters = {
                std::clamp(reflection.intensity, 0.0f, 1.0f),
                screenReflectionActive ? 1.0f : 0.0f,
                std::max(reflection.maximumDistance, 0.01f),
                static_cast<float>(std::clamp<std::uint32_t>(
                    reflection.stepCount,
                    1u,
                    128u)) };
            constants.screenReflectionScreen = {
                reflection.inverseWidth,
                reflection.inverseHeight,
                0.0f,
                0.0f };
            constants.screenReflectionQuality = {
                std::max(reflection.thickness, 0.001f),
                std::clamp(reflection.roughnessCutoff, 0.0f, 1.0f),
                static_cast<float>(reflection.depthPyramidMaximumMip),
                0.0f };
            constants.screenReflectionPreviousViewProjection =
                reflection.previousViewProjection;
            // LitEffect::SetLightingD3D11と同じ値です。
            constants.environmentParameters = {
                environmentActive
                    ? std::max(request.environment.intensity, 0.0f)
                    : 0.0f,
                environmentActive ? 1.0f : 0.0f,
                prefilteredEnvironmentActive
                    ? request.environment.specularMaximumMip
                    : 0.0f,
                0.0f };
            // LitEffect::SetLightingとDirectXTKのIEffectFogへ渡す値と同じです。
            constants.fogColorModel = {
                request.fog.color.x,
                request.fog.color.y,
                request.fog.color.z,
                request.fog.model == LamaPon::PrimitiveFogModel::DirectXTK
                    ? 1.0f
                    : 0.0f };
            constants.fogParameters = {
                request.fog.startDistance,
                request.fog.endDistance,
                request.fog.density,
                request.fog.enabled ? 1.0f : 0.0f };
            // リフレクションプローブは、LitEffect::SetEnvironmentOverrideD3D11
            // と同じ値でSkyのIBLを差し替えます。
            if (probe.active)
            {
                constants.environmentParameters = probe.environmentParameters;
                constants.reflectionBoxCenter = probe.boxCenter;
                constants.reflectionBoxParameters = probe.boxParameters;
                constants.reflectionSecondaryBoxCenter =
                    probe.secondaryBoxCenter;
                constants.reflectionSecondaryBoxParameters =
                    probe.secondaryBoxParameters;
                constants.reflectionBlendParameters = probe.blendParameters;
            }
            // LitEffect::SetLightingD3D11と同じForward+の値です。
            const auto& clustered = request.clustered;
            constants.clusteredParameters = {
                static_cast<float>(LamaPon::ClusteredLights::GridWidth),
                static_cast<float>(LamaPon::ClusteredLights::GridHeight),
                static_cast<float>(LamaPon::ClusteredLights::GridDepth),
                clusteredActive ? 1.0f : 0.0f };
            constants.clusteredDepthParameters = {
                clustered.nearPlane,
                clustered.farPlane,
                std::log(std::max(
                    clustered.farPlane
                        / std::max(clustered.nearPlane, 0.0001f),
                    1.0001f)),
                static_cast<float>(
                    LamaPon::ClusteredLights::MaximumLightsPerCluster) };
            constants.clusteredScreenParameters = {
                clustered.inverseWidth,
                clustered.inverseHeight,
                static_cast<float>(clustered.lightCount),
                0.0f };
            // LitEffect::SetLightingD3D11と同じベイクした間接光の値です。
            const auto& bakedGi = request.bakedGlobalIllumination;
            constants.bakedGiVolumeMinimum = {
                bakedGi.volumeMinimum.x,
                bakedGi.volumeMinimum.y,
                bakedGi.volumeMinimum.z,
                bakedGiActive ? 1.0f : 0.0f };
            constants.bakedGiInverseSize = {
                1.0f / std::max(bakedGi.volumeSize.x, 0.0001f),
                1.0f / std::max(bakedGi.volumeSize.y, 0.0001f),
                1.0f / std::max(bakedGi.volumeSize.z, 0.0001f),
                std::max(bakedGi.intensity, 0.0f) };
            constants.bakedGiResolution = {
                std::max(bakedGi.resolution.x, 1.0f),
                std::max(bakedGi.resolution.y, 1.0f),
                std::max(bakedGi.resolution.z, 1.0f),
                0.0f };
            const auto constantUpload = m_backend->AllocateFrameUpload(
                sizeof(constants),
                D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            std::memcpy(
                constantUpload.data,
                &constants,
                sizeof(constants));

            const D3D12_VERTEX_BUFFER_VIEW vertexView{
                vertexUpload.gpuAddress,
                static_cast<UINT>(vertexBytes),
                static_cast<UINT>(sizeof(PrimitiveRenderVertex)) };
            const D3D12_INDEX_BUFFER_VIEW indexView{
                indexUpload.gpuAddress,
                static_cast<UINT>(indexBytes),
                DXGI_FORMAT_R32_UINT };
            D3D12_VERTEX_BUFFER_VIEW instanceView{};
            if (instanced)
            {
                const auto instanceUpload = m_backend->AllocateFrameUpload(
                    static_cast<std::uint64_t>(request.instances.size_bytes()),
                    alignof(LamaPon::PrimitiveInstanceData));
                std::memcpy(
                    instanceUpload.data,
                    request.instances.data(),
                    request.instances.size_bytes());
                instanceView = {
                    instanceUpload.gpuAddress,
                    static_cast<UINT>(request.instances.size_bytes()),
                    static_cast<UINT>(sizeof(LamaPon::PrimitiveInstanceData)) };
            }
            auto* pipeline = request.depthOnly
                ? DepthOnlyPipelineState(request.wireframe, request.cull)
                : PipelineState(
                    request.alphaBlend,
                    request.depthTest,
                    request.depthWrite,
                    request.wireframe,
                    instanced,
                    request.cull);
            commandList->SetGraphicsRootSignature(m_rootSignature.Get());
            commandList->SetPipelineState(pipeline);
            commandList->SetGraphicsRootConstantBufferView(
                0,
                constantUpload.gpuAddress);
            if (!request.depthOnly)
            {
                ID3D12DescriptorHeap* heaps[]{ descriptorHeap };
                commandList->SetDescriptorHeaps(1, heaps);
                for (std::size_t index{}; index < bindings.size(); ++index)
                {
                    commandList->SetGraphicsRootDescriptorTable(
                        static_cast<UINT>(index + 1u),
                        bindings[index].descriptor);
                }
                commandList->SetGraphicsRootDescriptorTable(
                    7,
                    shadowBinding.descriptor);
                commandList->SetGraphicsRootDescriptorTable(
                    8,
                    spotShadowBinding.descriptor);
                commandList->SetGraphicsRootDescriptorTable(
                    9,
                    pointShadowBinding.descriptor);
                commandList->SetGraphicsRootDescriptorTable(
                    10,
                    screenAmbientOcclusionBinding.descriptor);
                commandList->SetGraphicsRootDescriptorTable(
                    11,
                    screenReflectionColorBinding.descriptor);
                commandList->SetGraphicsRootDescriptorTable(
                    12,
                    screenReflectionDepthBinding.descriptor);
                commandList->SetGraphicsRootDescriptorTable(
                    13,
                    environmentDescriptor);
                commandList->SetGraphicsRootDescriptorTable(
                    14,
                    irradianceDescriptor);
                commandList->SetGraphicsRootDescriptorTable(
                    15,
                    clusterLightsDescriptor);
                commandList->SetGraphicsRootDescriptorTable(
                    16,
                    clusterIndicesDescriptor);
                commandList->SetGraphicsRootDescriptorTable(
                    17,
                    clusterCountsDescriptor);
                commandList->SetGraphicsRootDescriptorTable(
                    18,
                    bakedGiRedDescriptor);
                commandList->SetGraphicsRootDescriptorTable(
                    19,
                    bakedGiGreenDescriptor);
                commandList->SetGraphicsRootDescriptorTable(
                    20,
                    bakedGiBlueDescriptor);
                commandList->SetGraphicsRootDescriptorTable(
                    21,
                    secondaryEnvironmentDescriptor);
                commandList->SetGraphicsRootDescriptorTable(
                    22,
                    secondaryIrradianceDescriptor);
            }
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            if (instanced)
            {
                const std::array<D3D12_VERTEX_BUFFER_VIEW, 2> vertexViews{
                    vertexView,
                    instanceView };
                commandList->IASetVertexBuffers(
                    0,
                    static_cast<UINT>(vertexViews.size()),
                    vertexViews.data());
            }
            else
            {
                commandList->IASetVertexBuffers(0, 1, &vertexView);
            }
            commandList->IASetIndexBuffer(&indexView);
            if (!request.depthOnly)
            {
                const auto& viewport = m_backend->ActiveViewport();
                const auto& scissor =
                    m_backend->ActiveScissorRectangle();
                commandList->RSSetViewports(1, &viewport);
                commandList->RSSetScissorRects(1, &scissor);
            }
            commandList->DrawIndexedInstanced(
                static_cast<UINT>(indices.size()),
                instanced
                    ? static_cast<UINT>(request.instances.size())
                    : 1u,
                0,
                0,
                0);
            return true;
        }

    private:
        // DrawPrimitiveと同じく、組み込み形状かProcedural Meshの頂点を選び、
        // 添字が範囲内の三角形列であることを確かめます。
        [[nodiscard]] bool ResolveGeometry(
            const LamaPon::PrimitiveDrawRequest& request,
            std::span<const PrimitiveRenderVertex>& vertices,
            std::span<const std::uint32_t>& indices) const
        {
            const Geometry* geometry{};
            switch (request.shape)
            {
            case LamaPon::PrimitiveRenderShape::Cube: geometry = &m_cube; break;
            case LamaPon::PrimitiveRenderShape::Sphere: geometry = &m_sphere; break;
            case LamaPon::PrimitiveRenderShape::Cylinder: geometry = &m_cylinder; break;
            case LamaPon::PrimitiveRenderShape::Plane: geometry = &m_plane; break;
            case LamaPon::PrimitiveRenderShape::Procedural: break;
            default: return false;
            }
            vertices = geometry != nullptr
                ? std::span<const PrimitiveRenderVertex>(geometry->vertices)
                : request.vertices;
            indices = geometry != nullptr
                ? std::span<const std::uint32_t>(geometry->indices)
                : request.indices;
            return !vertices.empty()
                && !indices.empty()
                && indices.size() % 3u == 0u
                && vertices.size() <= std::numeric_limits<UINT>::max()
                && indices.size() <= std::numeric_limits<UINT>::max()
                && std::ranges::none_of(
                    indices,
                    [vertices](const std::uint32_t index)
                    {
                        return index >= vertices.size();
                    });
        }

        // D3D11のApplyCustomPixelShaderのSpriteShaderEntryと同じ保存監視の
        // 状態を持つ、ParticleSystemのcustom pixel shaderです。
        struct CustomParticleShaderEntry final
        {
            Microsoft::WRL::ComPtr<ID3DBlob> pixelShader;
            // 出力format（RGBA8／RGBA16F）と、通常／加算の合成ごとです。
            std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4>
                pipelineStates;
            std::uint64_t generation{};
            std::string error;
            std::chrono::steady_clock::time_point nextCheck{};
            std::filesystem::file_time_type writeTime{};
            bool observed{};
            bool forceReload{};
            bool sourceExists{};
        };

        struct ParticleGeometry final
        {
            D3D12_VERTEX_BUFFER_VIEW vertexView{};
            D3D12_INDEX_BUFFER_VIEW indexView{};
            UINT indexCount{};
        };

        static void RequireCompleteParticleQuads(
            const LamaPon::ParticleDrawRequest& request)
        {
            constexpr std::size_t maximumParticleCount = 4096u;
            if (request.vertices.size() % 4u != 0u
                || request.vertices.size() > maximumParticleCount * 4u)
            {
                throw std::invalid_argument(
                    "Particle draw requests require complete quads within "
                    "the service capacity.");
            }
        }

        // D3D11のPrimitiveBatch::DrawQuadと同じく、4頂点のquadを2枚の
        // 三角形へ分けてframe uploadへ積みます。
        [[nodiscard]] ParticleGeometry UploadParticleGeometry(
            const LamaPon::ParticleDrawRequest& request)
        {
            const auto quadCount = request.vertices.size() / 4u;
            const auto indexCount = quadCount * 6u;
            const auto vertexBytes = static_cast<std::uint64_t>(
                request.vertices.size_bytes());
            const auto indexBytes = static_cast<std::uint64_t>(
                indexCount * sizeof(std::uint32_t));
            const auto vertexUpload = m_backend->AllocateFrameUpload(
                vertexBytes,
                alignof(LamaPon::ParticleRenderVertex));
            const auto indexUpload = m_backend->AllocateFrameUpload(
                indexBytes,
                alignof(std::uint32_t));
            std::memcpy(
                vertexUpload.data,
                request.vertices.data(),
                request.vertices.size_bytes());
            auto* indices = reinterpret_cast<std::uint32_t*>(indexUpload.data);
            for (std::size_t quad{}; quad < quadCount; ++quad)
            {
                const auto first = static_cast<std::uint32_t>(quad * 4u);
                const auto offset = quad * 6u;
                indices[offset] = first;
                indices[offset + 1u] = first + 1u;
                indices[offset + 2u] = first + 2u;
                indices[offset + 3u] = first;
                indices[offset + 4u] = first + 2u;
                indices[offset + 5u] = first + 3u;
            }
            ParticleGeometry geometry;
            geometry.vertexView = {
                vertexUpload.gpuAddress,
                static_cast<UINT>(vertexBytes),
                static_cast<UINT>(sizeof(LamaPon::ParticleRenderVertex)) };
            geometry.indexView = {
                indexUpload.gpuAddress,
                static_cast<UINT>(indexBytes),
                DXGI_FORMAT_R32_UINT };
            geometry.indexCount = static_cast<UINT>(indexCount);
            return geometry;
        }

        [[nodiscard]] LamaPon::D3D12Backend::FrameUploadAllocation
            UploadParticleViewProjection(
                const LamaPon::ParticleDrawRequest& request)
        {
            DirectX::XMFLOAT4X4 viewProjection{};
            DirectX::XMStoreFloat4x4(
                &viewProjection,
                DirectX::XMMatrixMultiply(
                    DirectX::XMLoadFloat4x4(&request.view),
                    DirectX::XMLoadFloat4x4(&request.projection)));
            const auto upload = m_backend->AllocateFrameUpload(
                sizeof(viewProjection),
                D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            std::memcpy(upload.data, &viewProjection, sizeof(viewProjection));
            return upload;
        }

        // D3D11のApplyCustomPixelShaderと同じく、保存を250ミリ秒ごとに
        // 確かめて作り直し、失敗したShaderは残しません。
        [[nodiscard]] CustomParticleShaderEntry& PrepareCustomParticleShader(
            LamaPon::AssetManager& assets,
            const LamaPon::Detail::MaterialShaderSource& source)
        {
            auto& entry = m_customParticleShaders[source.cacheKey];
            const auto now = std::chrono::steady_clock::now();
            if (entry.observed && !entry.forceReload && now < entry.nextCheck)
            {
                return entry;
            }
            entry.nextCheck = now + std::chrono::milliseconds(250);
            const bool archived = assets.IsArchived();
            std::error_code fileError;
            const bool sourceExists = assets.FileExists(source.path);
            const auto writeTime = (sourceExists && !archived)
                ? std::filesystem::last_write_time(source.path, fileError)
                : std::filesystem::file_time_type{};
            const bool changed = !entry.observed
                || entry.forceReload
                || entry.sourceExists != sourceExists
                || (sourceExists
                    && !archived
                    && entry.writeTime != writeTime);
            if (!changed)
            {
                return entry;
            }
            entry.observed = true;
            entry.forceReload = false;
            entry.sourceExists = sourceExists;
            entry.writeTime = writeTime;
            for (auto& pipeline : entry.pipelineStates)
            {
                pipeline.Reset();
            }
            if (!sourceExists)
            {
                entry.error = "Custom pixel shader file was not found: "
                    + LamaPon::PathToUtf8(source.path);
                entry.pixelShader.Reset();
                return entry;
            }
            try
            {
                entry.pixelShader = LamaPon::CompileShaderCached(
                    assets,
                    source.path,
                    "PSMain",
                    "ps_5_0");
                entry.generation = m_nextCustomParticleGeneration++;
                entry.error.clear();
            }
            catch (const std::exception& exception)
            {
                entry.error = source.describeFailure
                    ? source.describeFailure(exception.what())
                    : std::string(exception.what());
                entry.pixelShader.Reset();
            }
            return entry;
        }

        [[nodiscard]] ID3D12PipelineState* CustomParticlePipelineState(
            CustomParticleShaderEntry& entry,
            const bool additive)
        {
            const auto colorFormat = m_backend->ActiveColorFormat();
            const std::size_t formatIndex = colorFormat
                    == LamaPon::D3D12Backend::PrimaryColorFormat
                ? 0u
                : colorFormat == DXGI_FORMAT_R16G16B16A16_FLOAT
                    ? 1u
                    : throw std::invalid_argument(
                        "The active DirectX 12 particle target format is "
                        "unsupported.");
            auto& pipeline = entry.pipelineStates[
                formatIndex * 2u + (additive ? 1u : 0u)];
            if (pipeline != nullptr)
            {
                return pipeline.Get();
            }
            static const std::array<D3D12_INPUT_ELEMENT_DESC, 3> inputs{ {
                { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
                    offsetof(LamaPon::ParticleRenderVertex, position),
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
                    offsetof(LamaPon::ParticleRenderVertex, color),
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
                    offsetof(LamaPon::ParticleRenderVertex, textureCoordinate),
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
            } };
            D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
            description.pRootSignature = m_customParticleRootSignature.Get();
            description.VS = {
                m_customParticleVertexShader->GetBufferPointer(),
                m_customParticleVertexShader->GetBufferSize() };
            description.PS = {
                entry.pixelShader->GetBufferPointer(),
                entry.pixelShader->GetBufferSize() };
            // D3D11と同じく通常／加算の合成、深度は読むだけ、カリングなしです。
            description.BlendState = MakeParticleBlendDescription(additive);
            description.SampleMask = std::numeric_limits<UINT>::max();
            description.RasterizerState = MakeRasterizerDescription();
            description.DepthStencilState = MakeDepthDescription(true, false);
            description.InputLayout = {
                inputs.data(),
                static_cast<UINT>(inputs.size()) };
            description.PrimitiveTopologyType =
                D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            description.NumRenderTargets = 1;
            description.RTVFormats[0] = colorFormat;
            description.DSVFormat = m_backend->ActiveDepthFormat();
            description.SampleDesc.Count = 1;
            ThrowIfFailed(
                m_backend->Device()->CreateGraphicsPipelineState(
                    &description,
                    IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf())),
                "ID3D12Device::CreateGraphicsPipelineState(custom particle)");
            return pipeline.Get();
        }

        [[nodiscard]] ID3D12PipelineState* ParticlePipelineState(
            bool additive)
        {
            const auto colorFormat = m_backend->ActiveColorFormat();
            const std::size_t formatIndex = colorFormat
                    == LamaPon::D3D12Backend::PrimaryColorFormat
                ? 0u
                : colorFormat == DXGI_FORMAT_R16G16B16A16_FLOAT
                    ? 1u
                    : throw std::invalid_argument(
                        "The active DirectX 12 particle target format is "
                        "unsupported.");
            auto& pipeline = m_particlePipelineStates[
                formatIndex * 2u + (additive ? 1u : 0u)];
            if (pipeline != nullptr)
            {
                return pipeline.Get();
            }
            static const std::array<D3D12_INPUT_ELEMENT_DESC, 3> inputs{ {
                { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
                    offsetof(LamaPon::ParticleRenderVertex, position),
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
                    offsetof(LamaPon::ParticleRenderVertex, color),
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
                    offsetof(LamaPon::ParticleRenderVertex, textureCoordinate),
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
            } };
            D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
            description.pRootSignature = m_rootSignature.Get();
            description.VS = {
                m_particleVertexShader->GetBufferPointer(),
                m_particleVertexShader->GetBufferSize() };
            description.PS = {
                m_particlePixelShader->GetBufferPointer(),
                m_particlePixelShader->GetBufferSize() };
            description.BlendState = MakeParticleBlendDescription(additive);
            description.SampleMask = std::numeric_limits<UINT>::max();
            description.RasterizerState = MakeRasterizerDescription();
            description.DepthStencilState = MakeDepthDescription(true, false);
            description.InputLayout = {
                inputs.data(),
                static_cast<UINT>(inputs.size()) };
            description.PrimitiveTopologyType =
                D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            description.NumRenderTargets = 1;
            description.RTVFormats[0] = colorFormat;
            description.DSVFormat = m_backend->ActiveDepthFormat();
            description.SampleDesc.Count = 1;
            ThrowIfFailed(
                m_backend->Device()->CreateGraphicsPipelineState(
                    &description,
                    IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf())),
                "ID3D12Device::CreateGraphicsPipelineState(particle)");
            return pipeline.Get();
        }

        // ワイヤーフレームはカリングしないので、カリングの指定ごとに
        // pipelineを分けません。
        [[nodiscard]] static std::size_t CullIndex(
            const bool wireframe,
            const LamaPon::ShaderCullMode cull) noexcept
        {
            return wireframe
                ? static_cast<std::size_t>(LamaPon::ShaderCullMode::None)
                : static_cast<std::size_t>(cull);
        }

        [[nodiscard]] ID3D12PipelineState* PipelineState(
            bool alphaBlend,
            bool depthTest,
            bool depthWrite,
            bool wireframe,
            bool instanced,
            LamaPon::ShaderCullMode cull)
        {
            const auto colorFormat = m_backend->ActiveColorFormat();
            const std::size_t formatIndex = colorFormat
                    == LamaPon::D3D12Backend::PrimaryColorFormat
                ? 0u
                : colorFormat == DXGI_FORMAT_R16G16B16A16_FLOAT
                    ? 1u
                    : throw std::invalid_argument(
                        "The active DirectX 12 primitive target format is "
                        "unsupported.");
            const std::size_t index = formatIndex * 36u
                + CullIndex(wireframe, cull) * 12u
                + (instanced ? 6u : 0u)
                + (wireframe ? 3u : 0u)
                + (!depthTest ? 2u : (alphaBlend ? 1u : 0u));
            auto& pipeline = m_pipelineStates[index];
            if (pipeline != nullptr)
            {
                return pipeline.Get();
            }
            static const std::array<D3D12_INPUT_ELEMENT_DESC, 3> inputs{ {
                { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
                    offsetof(PrimitiveRenderVertex, position),
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
                    offsetof(PrimitiveRenderVertex, normal),
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
                    offsetof(PrimitiveRenderVertex, textureCoordinate),
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
            } };
            D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
            description.pRootSignature = m_rootSignature.Get();
            // D3D11のLamaPonLit.hlslのVSInstancedMainと同じ入力で、slot 1に
            // world行列4行と色を80 bytesずつ並べます。
            static const std::array<D3D12_INPUT_ELEMENT_DESC, 8>
                instancedInputs{ {
                inputs[0],
                inputs[1],
                inputs[2],
                { "INSTANCE_TRANSFORM", 0, DXGI_FORMAT_R32G32B32A32_FLOAT,
                    1, 0u, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
                { "INSTANCE_TRANSFORM", 1, DXGI_FORMAT_R32G32B32A32_FLOAT,
                    1, 16u, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
                { "INSTANCE_TRANSFORM", 2, DXGI_FORMAT_R32G32B32A32_FLOAT,
                    1, 32u, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
                { "INSTANCE_TRANSFORM", 3, DXGI_FORMAT_R32G32B32A32_FLOAT,
                    1, 48u, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
                { "INSTANCE_COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT,
                    1, 64u, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 }
            } };
            auto* const vertexShader = instanced
                ? m_instancedVertexShader.Get()
                : m_vertexShader.Get();
            description.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
            description.PS = { m_pixelShader->GetBufferPointer(), m_pixelShader->GetBufferSize() };
            description.BlendState = MakeBlendDescription(alphaBlend || !depthTest);
            description.SampleMask = std::numeric_limits<UINT>::max();
            description.RasterizerState = MakeRasterizerDescription(wireframe, cull);
            description.DepthStencilState = MakeDepthDescription(depthTest, depthWrite);
            description.InputLayout = instanced
                ? D3D12_INPUT_LAYOUT_DESC{
                    instancedInputs.data(),
                    static_cast<UINT>(instancedInputs.size()) }
                : D3D12_INPUT_LAYOUT_DESC{
                    inputs.data(),
                    static_cast<UINT>(inputs.size()) };
            description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            description.NumRenderTargets = 1;
            description.RTVFormats[0] = colorFormat;
            description.DSVFormat = m_backend->ActiveDepthFormat();
            description.SampleDesc.Count = 1;
            ThrowIfFailed(
                m_backend->Device()->CreateGraphicsPipelineState(
                    &description, IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf())),
                "ID3D12Device::CreateGraphicsPipelineState(primitive)");
            return pipeline.Get();
        }

        [[nodiscard]] ID3D12PipelineState* DepthOnlyPipelineState(
            bool wireframe,
            LamaPon::ShaderCullMode cull)
        {
            const auto depthFormat = m_backend->ActiveDepthFormat();
            const std::size_t formatIndex = depthFormat
                    == LamaPon::D3D12Backend::PrimaryDepthFormat
                ? 0u
                : depthFormat == LamaPon::D3D12Backend::ShadowDepthFormat
                    ? 1u
                    : throw std::invalid_argument(
                        "The active DirectX 12 depth target format is "
                        "unsupported.");
            auto& pipeline = m_depthOnlyPipelineStates[
                formatIndex * 6u
                + CullIndex(wireframe, cull) * 2u
                + (wireframe ? 1u : 0u)];
            if (pipeline != nullptr)
            {
                return pipeline.Get();
            }
            static const std::array<D3D12_INPUT_ELEMENT_DESC, 3> inputs{ {
                { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
                    offsetof(PrimitiveRenderVertex, position),
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
                    offsetof(PrimitiveRenderVertex, normal),
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
                    offsetof(PrimitiveRenderVertex, textureCoordinate),
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
            } };
            D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
            description.pRootSignature = m_rootSignature.Get();
            description.VS = {
                m_vertexShader->GetBufferPointer(),
                m_vertexShader->GetBufferSize() };
            description.BlendState = MakeBlendDescription(false);
            description.SampleMask = std::numeric_limits<UINT>::max();
            // D3D11のShadow mapもrasterizerのbiasを使わず、裏面をカリングして
            // shader側のbiasだけで自己遮蔽を抑えます。
            description.RasterizerState = MakeRasterizerDescription(wireframe, cull);
            description.DepthStencilState =
                MakeDepthDescription(true, true);
            description.InputLayout = {
                inputs.data(),
                static_cast<UINT>(inputs.size()) };
            description.PrimitiveTopologyType =
                D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            description.NumRenderTargets = 0;
            description.DSVFormat = depthFormat;
            description.SampleDesc.Count = 1;
            ThrowIfFailed(
                m_backend->Device()->CreateGraphicsPipelineState(
                    &description,
                    IID_PPV_ARGS(
                        pipeline.ReleaseAndGetAddressOf())),
                "ID3D12Device::CreateGraphicsPipelineState(depth only)");
            return pipeline.Get();
        }

        LamaPon::D3D12Backend* m_backend{};
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
        Microsoft::WRL::ComPtr<ID3DBlob> m_vertexShader;
        Microsoft::WRL::ComPtr<ID3DBlob> m_pixelShader;
        Microsoft::WRL::ComPtr<ID3DBlob> m_instancedVertexShader;
        Microsoft::WRL::ComPtr<ID3DBlob> m_particleVertexShader;
        Microsoft::WRL::ComPtr<ID3DBlob> m_particlePixelShader;
        std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4>
            m_particlePipelineStates;
        Microsoft::WRL::ComPtr<ID3DBlob> m_customParticleVertexShader;
        Microsoft::WRL::ComPtr<ID3D12RootSignature>
            m_customParticleRootSignature;
        std::unordered_map<std::filesystem::path, CustomParticleShaderEntry>
            m_customParticleShaders;
        std::uint64_t m_nextCustomParticleGeneration{ 1 };
        // 色の2形式×カリング3種×instanced×wireframe×合成3種です。
        std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 72>
            m_pipelineStates;
        // 深度の2形式×カリング3種×wireframeです。
        std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 12>
            m_depthOnlyPipelineStates;
        Geometry m_cube;
        Geometry m_sphere;
        Geometry m_cylinder;
        Geometry m_plane;
        std::unique_ptr<LamaPon::Detail::D3D12MaterialShaderRenderer>
            m_materialShaders;
    };
}

namespace LamaPon
{
    std::unique_ptr<GraphicsRenderServices>
        CreateD3D12GraphicsRenderServices(D3D12Backend& backend)
    {
        return std::make_unique<D3D12RenderServices>(backend);
    }
}
