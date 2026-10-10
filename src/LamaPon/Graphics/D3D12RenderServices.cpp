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

    // 標準照明とインスタンス描画のHLSL
    constexpr char PrimitiveShaderSource[] =
        // MSVCの単一リテラルの長さ制限を避け、同じHLSLを連結します。
        R"(
// C++の2448バイト定数と同じb0配置
cbuffer PrimitiveConstants : register(b0)
{
    // ワールド変換行列
    row_major float4x4 World;
    // マテリアルのRGBA色
    float4 BaseColor;
    // カメラのワールド位置
    float4 CameraPosition;
    // 粗さ・金属度・AO強度・法線有無
    float4 MaterialProperties;
    // 発光RGBと法線強度
    float4 EmissiveFactor;
    // 環境光RGBと強度
    float4 AmbientColorIntensity;
    // 平行光の方向XYZと強度
    float4 DirectionalDirectionIntensity[4];
    // 平行光RGBと太陽の角半径
    float4 DirectionalColors[4];
    // 平行・点・スポット・影の数
    uint4 LightCounts;
    // 点光源の位置XYZと範囲
    float4 PointPositionRange[16];
    // 点光源のRGBと強度
    float4 PointColorIntensity[16];
    // スポット光の位置XYZと範囲
    float4 SpotPositionRange[8];
    // スポット方向と内側角のcos
    float4 SpotDirectionInnerCosine[8];
    // スポット光のRGBと強度
    float4 SpotColorIntensity[8];
    // 外側角のcosと影枠番号
    float4 SpotOuterCosine[8];
    // カスケード別の影描画行列
    row_major float4x4 ShadowViewProjections[4];
    // カスケード別の終端距離
    float4 ShadowCascadeSplits;
    // 平行光番号・バイアスと強度
    float4 ShadowParameters;
    // カメラ前方XYZと影画素幅
    float4 CameraForwardShadowTexel;
    // スポット影の描画行列
    row_major float4x4 SpotShadowViewProjections[4];
    // 影のバイアス・強度・有無
    float4 SpotShadowParameters[4];
    // 点光源番号・バイアス・強度
    float4 PointShadowParameters;
    // スポット影と点影の画素幅
    float4 LocalShadowTexelSizes;
    // 画面寸法の逆数とSSAO有無
    float4 ScreenAmbientOcclusionParameters;
    // ビューと射影の合成行列
    row_major float4x4 ViewProjection;
    // SSR強度・有無・距離・歩数
    float4 ScreenReflectionParameters;
    // 画面寸法の逆数
    float4 ScreenReflectionScreen;
    // SSR厚み・粗さ上限・最大段
    float4 ScreenReflectionQuality;
    // SSR履歴を描いた合成行列
    row_major float4x4 ScreenReflectionPreviousViewProjection;
    // IBL強度・有無・最大ミップ
    float4 EnvironmentParameters;
    // 霧RGBと互換霧方式の指定
    float4 FogColorModel;
    // 霧の開始・終端・密度・有無
    float4 FogParameters;
    // 法線用の逆転置行列
    row_major float4x4 WorldInverseTranspose;
    // クラスタXYZの分割数と有無
    float4 ClusteredParameters;
    // 近遠距離・対数比・ライト上限
    float4 ClusteredDepthParameters;
    // 画面寸法の逆数とライト数
    float4 ClusteredScreenParameters;
    // GI領域の最小座標と有無
    float4 BakedGiVolumeMinimum;
    // GI領域寸法の逆数と強度
    float4 BakedGiInverseSize;
    // GI格子のXYZ解像度
    float4 BakedGiResolution;
    // 反射補正箱の中心
    float4 ReflectionBoxCenter;
    // 反射補正箱の半寸法と有無
    float4 ReflectionBoxParameters;
    // 第2反射補正箱の中心
    float4 ReflectionSecondaryBoxCenter;
    // 第2反射補正箱の半寸法と有無
    float4 ReflectionSecondaryBoxParameters;
    // 第2プローブの混合率と最終段
    float4 ReflectionBlendParameters;
};

// t0の基本色画像
Texture2D AlbedoTexture : register(t0);
// t1の法線画像
Texture2D NormalTexture : register(t1);
// t2の粗さ画像
Texture2D RoughnessTexture : register(t2);
// t3の金属度画像
Texture2D MetallicTexture : register(t3);
// t4の素材AO画像
Texture2D OcclusionTexture : register(t4);
// t5の発光画像
Texture2D EmissiveTexture : register(t5);
// t6の平行光の影配列
Texture2DArray<float> DirectionalShadowTexture : register(t6);
// t7のスポット影配列
Texture2DArray<float> SpotShadowTexture : register(t7);
// t8の点光源の影キューブ
TextureCube<float> PointShadowTexture : register(t8);
// t9の画面空間AO画像
Texture2D ScreenAmbientOcclusionTexture : register(t9);
// t10の前フレームHDR色
Texture2D ScreenReflectionColorTexture : register(t10);
// t11の最小距離ピラミッド
Texture2D ScreenReflectionDepthTexture : register(t11);
// t12の反射用キューブ
TextureCube EnvironmentMap : register(t12);
// t13の放射照度キューブ
TextureCube IrradianceMap : register(t13);
// t14～t16はカリング側が出力するライト一覧・索引・個数です。
struct ClusterLight
{
    // ライトの位置XYZと範囲
    float4 PositionRange;
    // ライトのRGBと強度
    float4 ColorIntensity;
    // ライト方向と内側角のcos
    float4 DirectionInnerCosine;
    // 外側角のcos・種類・影参照
    float4 ExtraParameters;
};
// t14の構造化ライト一覧
StructuredBuffer<ClusterLight> ClusterLights : register(t14);
// t15のクラスタ別ライト番号
StructuredBuffer<uint> ClusterLightIndexList : register(t15);
// t16のクラスタ別ライト数
StructuredBuffer<uint> ClusterLightCounts : register(t16);
// t17の赤成分GI係数体積
Texture3D BakedGiRedTexture : register(t17);
// t18の緑成分GI係数体積
Texture3D BakedGiGreenTexture : register(t18);
// t19の青成分GI係数体積
Texture3D BakedGiBlueTexture : register(t19);
// t20の第2反射キューブ
TextureCube SecondaryEnvironmentMap : register(t20);
// t21の第2放射照度キューブ
TextureCube SecondaryIrradianceMap : register(t21);
// s0の線形繰り返しサンプラー
SamplerState AlbedoSampler : register(s0);
// s1の影深度比較サンプラー
SamplerComparisonState ShadowSampler : register(s1);

// GI領域の係数を評価し、境界5%で通常環境光と混合します(worldPosition: ワールド位置, normal: 単位ワールド法線)。
float3 EvaluateBakedAmbient(float3 worldPosition, float3 normal)
{
    // 強度を適用した通常環境光
    const float3 ambient = AmbientColorIntensity.rgb * AmbientColorIntensity.w;
    if (BakedGiVolumeMinimum.w < 0.5f)
    {
        return ambient;
    }
    // GI領域内の正規化座標
    const float3 volumeUvw =
        (worldPosition - BakedGiVolumeMinimum.xyz) * BakedGiInverseSize.xyz;
    // GI格子のXYZ解像度
    const float3 resolution = BakedGiResolution.xyz;
    // 格子点をテクセル中心へ移す座標
    const float3 texelUvw =
        (volumeUvw * (resolution - 1.0f) + 0.5f) / resolution;
    // XYZ法線と定数項のL1基底
    const float4 basis = float4(normal, 1.0f);
    // RGB別のベイク間接光
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
    // 境界5%の帯に対する内側距離
    const float3 edge = (0.5f - abs(volumeUvw - 0.5f)) / 0.05f;
    // ベイク間接光の混合率
    const float weight = saturate(min(min(edge.x, edge.y), edge.z));
    return lerp(ambient, gi, weight);
}

// 反射レイと箱の交点をプローブ中心からの方向へ変換します(reflection: 反射方向, worldPosition: ワールド位置, boxCenter: 箱の中心, boxExtents: 箱の半寸法)。
float3 ApplyBoxProjection(
    float3 reflection,
    float3 worldPosition,
    float3 boxCenter,
    float3 boxExtents)
{
    // 箱の正側平面までのレイ比率
    const float3 firstPlane =
        (boxCenter + boxExtents - worldPosition) / reflection;
    // 箱の負側平面までのレイ比率
    const float3 secondPlane =
        (boxCenter - boxExtents - worldPosition) / reflection;
    // 各軸で遠い側の交点比率
)"
        R"(    const float3 furthest = max(firstPlane, secondPlane);
    // 箱の出口までのレイ比率
    const float distance = min(min(furthest.x, furthest.y), furthest.z);
    // 箱との交点のワールド位置
    const float3 intersection = worldPosition + reflection * distance;
    return intersection - boxCenter;
}

// 箱補正した方向と粗さのミップで反射を読みます(probeMap: 反射キューブ, normal: 単位ワールド法線, viewDirection: 視点への単位方向, worldPosition: ワールド位置, roughness: 粗さ, maximumMip: 最終ミップ番号, boxCenter: 箱の中心, boxParameters: 半寸法XYZと有無)。
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
    // 箱補正する反射の単位方向
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
    // オブジェクト空間の頂点位置
    float3 position : POSITION;
    // ローカル空間の頂点法線
    float3 normal : NORMAL;
    // 画像を参照するUV座標
    float2 textureCoordinate : TEXCOORD;
};

struct PixelInput
{
    // 頂点のクリップ座標
    float4 position : SV_Position;
    // ワールド座標の頂点位置
    float3 worldPosition : TEXCOORD1;
    // ワールド空間の単位法線
    float3 normal : NORMAL;
    // 画像を参照するUV座標
    float2 textureCoordinate : TEXCOORD;
    // 基本色またはインスタンス色
    float4 tint : COLOR0;
};

// 基本頂点を射影し、逆転置法線と素材色を渡します(input: 位置・法線・UV)。
PixelInput PrimitiveVertexShader(VertexInput input)
{
    // 画素段へ渡す頂点出力
    PixelInput output;
    // ワールド座標の頂点位置
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
    // インスタンス内の頂点位置
    float3 position : POSITION;
    // ローカル空間の頂点法線
    float3 normal : NORMAL;
    // 画像を参照するUV座標
    float2 textureCoordinate : TEXCOORD;
    // インスタンス行列の第1行
    float4 instanceWorld0 : INSTANCE_TRANSFORM0;
    // インスタンス行列の第2行
    float4 instanceWorld1 : INSTANCE_TRANSFORM1;
    // インスタンス行列の第3行
    float4 instanceWorld2 : INSTANCE_TRANSFORM2;
    // インスタンス行列の第4行
    float4 instanceWorld3 : INSTANCE_TRANSFORM3;
    // インスタンスのRGBA色
    float4 instanceColor : INSTANCE_COLOR0;
};

// インスタンス行列で位置・法線を変換して色を渡します(input: 基本頂点と行列・色)。
PixelInput PrimitiveInstancedVertexShader(InstancedVertexInput input)
{
    // インスタンスのワールド行列
    const float4x4 world = float4x4(
        input.instanceWorld0,
        input.instanceWorld1,
        input.instanceWorld2,
        input.instanceWorld3);
    // 画素段へ渡す頂点出力
    PixelInput output;
    // ワールド座標の頂点位置
    const float4 worldPosition = mul(float4(input.position, 1.0f), world);
    output.position = mul(worldPosition, ViewProjection);
    output.worldPosition = worldPosition.xyz;
    output.normal = normalize(mul(float4(input.normal, 0.0f), world).xyz);
    output.textureCoordinate = input.textureCoordinate;
    output.tint = input.instanceColor;
    return output;
}

// 平行光の指定カスケードを3×3の比較で読みます(worldPosition: ワールド位置, normal: 単位ワールド法線, cascadeIndex: カスケード番号)。
float SampleDirectionalShadowCascade(
    float3 worldPosition,
    float3 normal,
    uint cascadeIndex)
{
    // 法線方向へずらした影照合位置
    const float3 biasedPosition =
        worldPosition + normal * ShadowParameters.z;
    // ライトのクリップ座標
    const float4 lightPosition = mul(
        float4(biasedPosition, 1.0f),
        ShadowViewProjections[cascadeIndex]);
    // 除算後のライトNDC座標
    const float3 projected = lightPosition.xyz
        / max(abs(lightPosition.w), 0.00001f);
    // 影画像のUV座標
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

    // 影の可視率の合計
    float visibility = 0.0f;
    // 影比較の縦方向オフセット
    [unroll]
    for (int tapY = -1; tapY <= 1; ++tapY)
    {
        // 影比較の横方向オフセット
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

// 平行光のカスケードを選び、境界10%で次段の影と混ぜます(worldPosition: ワールド位置, normal: 単位ワールド法線, lightIndex: 平行光の番号)。
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
    // カメラ前方へのビュー距離
    const float cameraDistance = dot(
        worldPosition - CameraPosition.xyz,
        CameraForwardShadowTexel.xyz);
    // 距離に対応するカスケード番号
    uint cascadeIndex = 0u;
    // 調べるカスケードの番号
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

    // 比較で得た影の可視率
    float visibility = SampleDirectionalShadowCascade(
        worldPosition,
        normal,
        cascadeIndex);
    if (cascadeIndex + 1u < LightCounts.w)
    {
        // 前段の終端距離
        const float previousSplit = cascadeIndex == 0u
            ? 0.0f
            : ShadowCascadeSplits[cascadeIndex - 1u];
        // 選択段が持つ距離範囲
        const float cascadeRange =
            ShadowCascadeSplits[cascadeIndex] - previousSplit;
        // 次段との混合を始める距離
        const float blendStart = ShadowCascadeSplits[cascadeIndex]
            - cascadeRange * 0.1f;
        // 次段の影へ移す混合率
        const float blend = saturate(
            (cameraDistance - blendStart)
            / max(cascadeRange * 0.1f, 0.0001f));
        if (blend > 0.0f)
        {
            visibility = lerp(
                visibility,
                SampleDirectionalShadowCascade(
)"
        R"(                    worldPosition,
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

// スポット影を3×3の比較で読み、強度を適用します(worldPosition: ワールド位置, normal: 単位ワールド法線, slot: 影の配列面番号)。
float EvaluateSpotShadow(
    float3 worldPosition,
    float3 normal,
    uint slot)
{
    // 指定面のバイアス・強度・有無
    const float4 parameters = SpotShadowParameters[slot];
    if (parameters.w < 0.5f)
    {
        return 1.0f;
    }
    // 法線方向へずらした影照合位置
    const float3 biasedPosition =
        worldPosition + normal * parameters.y;
    // ライトのクリップ座標
    const float4 lightPosition = mul(
        float4(biasedPosition, 1.0f),
        SpotShadowViewProjections[slot]);
    if (lightPosition.w <= 0.0001f)
    {
        return 1.0f;
    }
    // 除算後のライトNDC座標
    const float3 projected = lightPosition.xyz / lightPosition.w;
    // 影画像のUV座標
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

    // 影の可視率の合計
    float visibility = 0.0f;
    // 影比較の縦方向オフセット
    [unroll]
    for (int tapY = -1; tapY <= 1; ++tapY)
    {
        // 影比較の横方向オフセット
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

// 点光源のキューブ影を5点の比較で読みます(worldPosition: ワールド位置, lightIndex: 点光源の番号, lightPosition: 光源の位置, range: 光の影響範囲)。
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
    // 光源から照合位置へのベクトル
    const float3 fromLight = worldPosition - lightPosition;
    // 光源ベクトル各成分の絶対値
    const float3 absoluteVector = abs(fromLight);
    // キューブ深度に使う最大軸距離
    const float majorAxis = max(
        absoluteVector.x,
        max(absoluteVector.y, absoluteVector.z));
    // 点光源の影射影の近距離
    const float nearPlane = 0.1f;
    // 点光源の影射影の遠距離
    const float farPlane = max(range, nearPlane + 0.01f);
    // 光源射影の比較用深度
    const float depth = farPlane / (farPlane - nearPlane)
        - farPlane * nearPlane
            / ((farPlane - nearPlane) * max(majorAxis, nearPlane));
    // 光源から画素への単位方向
    const float3 direction = normalize(fromLight);
    // 接線基底の生成に使う補助軸
    const float3 axis = abs(direction.y) > 0.9f
        ? float3(1.0f, 0.0f, 0.0f)
        : float3(0.0f, 1.0f, 0.0f);
    // 影比較方向をずらす接線
    const float3 tangent = normalize(cross(axis, direction));
    // 影比較方向をずらす従接線
    const float3 bitangent = cross(direction, tangent);
    // キューブ比較の方向オフセット
    const float pointTexel = LocalShadowTexelSizes.y * 2.0f;
    // バイアスを引いた照合深度
    const float compareDepth = depth - PointShadowParameters.y;
    // 5点で求める影の可視率
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

// Hi-Zの最下段を点で読み、輪郭をまたぐ距離補間を避けます(uv: 画面のUV座標)。
float ScreenReflectionSceneDistance(float2 uv)
{
    // 画面の幅と高さ
    const float2 screenSize =
        1.0f / max(ScreenReflectionScreen.xy, 1e-6f);
    // 画面内の最後の画素座標
    const int2 lastPixel = max(int2(screenSize) - 1, int2(0, 0));
    // 参照する整数画素座標
    const int2 pixel = clamp(
        int2(saturate(uv) * screenSize),
        int2(0, 0),
        lastPixel);
    return ScreenReflectionDepthTexture.Load(int3(pixel, 0)).r;
}

// Hi-Zを探索し、前フレームの反射RGBと信頼度を返します(worldPosition: ワールド位置, reflection: 単位反射方向, viewDirection: 視点への単位方向, roughness: 粗さ)。
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
    // SSRを打ち切る粗さ上限
    const float roughnessCutoff = max(ScreenReflectionQuality.y, 0.0001f);
    if (roughness >= roughnessCutoff)
    {
        return 0.0f.xxxx;
    }

    // 反射レイの最大長
    const float maximumDistance = max(ScreenReflectionParameters.z, 0.01f);
    // 反射の交差を許す面の厚み
    const float thickness = max(ScreenReflectionQuality.x, 0.001f);
    // Hi-Z探索の反復上限
    const int maximumSteps = clamp(
        (int)ScreenReflectionParameters.w,
        4,
        128);

    // 反射レイの始点
    const float3 rayStart = worldPosition;
    // 反射レイの終点
    float3 rayEnd = worldPosition + reflection * maximumDistance;
    // 始点のクリップ座標
    float4 clipStart = mul(float4(rayStart, 1.0f), ViewProjection);
    // 終点のクリップ座標
    float4 clipEnd = mul(float4(rayEnd, 1.0f), ViewProjection);

    // 射影を保つ最小ビュー距離
    const float nearW = 0.05f;
    if (clipStart.w <= nearW)
    {
        return 0.0f.xxxx;
    }
    if (clipEnd.w <= nearW)
    {
        // 近距離面でレイを切る比率
        const float clipRatio =
            (nearW - clipStart.w) / (clipEnd.w - clipStart.w);
        rayEnd = lerp(rayStart, rayEnd, saturate(clipRatio));
        clipEnd = mul(float4(rayEnd, 1.0f), ViewProjection);
    }

    // レイ始点の画面UV
    const float2 startUv = float2(
        clipStart.x / clipStart.w * 0.5f + 0.5f,
        0.5f - clipStart.y / clipStart.w * 0.5f);
    // レイ終点の画面UV
    const float2 endUv = float2(
        clipEnd.x / clipEnd.w * 0.5f + 0.5f,
        0.5f - clipEnd.y / clipEnd.w * 0.5f);
    // 始点から終点へのUV差
    const float2 deltaUv = endUv - startUv;

    // 画面内で進めるレイ比率
    float limitAlpha = 1.0f;
    // 画面の横軸または縦軸
    [unroll]
    for (int axis = 0; axis < 2; ++axis)
    {
        // 対象軸のUV進行量
)"
        R"(        const float direction = axis == 0 ? deltaUv.x : deltaUv.y;
        // 対象軸の始点UV
        const float origin = axis == 0 ? startUv.x : startUv.y;
        if (abs(direction) > 1e-6f)
        {
            // 区画または画面の出口比率
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

    // 画面の幅と高さ
    const float2 screenSize =
        1.0f / max(ScreenReflectionScreen.xy, 1e-6f);
    // 画面内レイの画素移動量
    const float2 pixelDelta = deltaUv * limitAlpha * screenSize;
    // 画素移動量の大きい側
    const float pixelLength = max(
        max(abs(pixelDelta.x), abs(pixelDelta.y)),
        1.0f);
    // 透視補間に使う始点距離の逆数
    const float inverseStartW = 1.0f / clipStart.w;
    // 透視補間に使う終点距離の逆数
    const float inverseEndW = 1.0f / clipEnd.w;
    // Hi-Zの最大ミップ番号
    const int maximumLevel = max((int)ScreenReflectionQuality.z, 0);

    // 半画素先から進めるレイ比率
    float alpha = 0.5f * limitAlpha / pixelLength;
    // 同じ境界への再訪を防ぐ前進量
    const float alphaBias = limitAlpha * 1e-5f;
    // 探索中のHi-Zミップ番号
    int level = 0;

    // Hi-Z探索の反復番号
    [loop]
    for (int iteration = 0; iteration < maximumSteps; ++iteration)
    {
        if (alpha >= limitAlpha)
        {
            break;
        }
        // 探索位置の画面UV
        const float2 uv = startUv + deltaUv * alpha;
        // 対象ミップの幅と高さ
        const float2 levelSize = max(
            floor(screenSize / exp2((float)level)),
            1.0f);
        // 探索位置が属する区画
        const float2 cell = floor(clamp(uv, 0.0f, 1.0f) * levelSize);
        // 進行側の区画境界を選ぶ値
        const float2 towardEdge = float2(
            deltaUv.x >= 0.0f ? 1.0f : 0.0f,
            deltaUv.y >= 0.0f ? 1.0f : 0.0f);
        // 進行側の区画境界UV
        const float2 boundaryUv = (cell + towardEdge) / levelSize;
        // 各軸の区画境界までの比率
        float2 boundaryAlpha = float2(1e9f, 1e9f);
        if (abs(deltaUv.x) > 1e-8f)
        {
            boundaryAlpha.x = (boundaryUv.x - startUv.x) / deltaUv.x;
        }
        if (abs(deltaUv.y) > 1e-8f)
        {
            boundaryAlpha.y = (boundaryUv.y - startUv.y) / deltaUv.y;
        }
        // 区画または画面の出口比率
        const float exitAlpha = max(
            min(boundaryAlpha.x, boundaryAlpha.y),
            alpha + alphaBias);
        // 画面内に収めた区画出口比率
        const float clampedExitAlpha = min(exitAlpha, limitAlpha);

        // 区画入口のレイのビュー距離
        const float entryDistance = 1.0f / max(
            lerp(inverseStartW, inverseEndW, alpha),
            1e-6f);
        // 区画出口のレイのビュー距離
        const float exitDistance = 1.0f / max(
            lerp(inverseStartW, inverseEndW, clampedExitAlpha),
            1e-6f);
        // 区画内のレイの近い側の距離
        const float rayNear = min(entryDistance, exitDistance);
        // 区画内のレイの遠い側の距離
        const float rayFar = max(entryDistance, exitDistance);
        // 区画内で最も手前の面の距離
        const float sceneDistance = ScreenReflectionDepthTexture.Load(
            int3(int2(min(cell, levelSize - 1.0f)), level)).r;

        if (rayFar <= sceneDistance)
        {
            // 区間全体が最前面より手前なら出口まで進み、2%以上離れた場合だけ粗い段へ移ります。
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
            // 交差を詰める区間の始点比率
            float nearAlpha = alpha;
            // 交差を詰める区間の終点比率
            float farAlpha = clampedExitAlpha;
            // 交差区間の二分探索番号
            [unroll]
            for (int refine = 0; refine < 4; ++refine)
            {
                // 交差候補区間の中央比率
                const float middleAlpha = (nearAlpha + farAlpha) * 0.5f;
                // 区間中央のレイのビュー距離
                const float middleDistance = 1.0f / max(
                    lerp(inverseStartW, inverseEndW, middleAlpha),
                    1e-6f);
                // 区間中央で読んだ面の距離
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
            // 二分探索後の交差比率
            const float hitAlpha = (nearAlpha + farAlpha) * 0.5f;
            // 交差位置の画面UV
            const float2 hitUv = startUv + deltaUv * hitAlpha;
            // 透視補間後のワールド比率
            const float worldRatio = hitAlpha * clipStart.w
                / max(lerp(clipEnd.w, clipStart.w, hitAlpha), 1e-6f);
            // 交差位置のワールド座標
            const float3 hitPosition = lerp(
                rayStart,
                rayEnd,
                saturate(worldRatio));

            // 交差位置の前フレーム射影
            const float4 previousClip = mul(
                float4(hitPosition, 1.0f),
                ScreenReflectionPreviousViewProjection);
            if (previousClip.w <= 0.0001f)
            {
                return 0.0f.xxxx;
            }
            // 交差位置の前フレームUV
            const float2 previousUv = float2(
                previousClip.x / previousClip.w * 0.5f + 0.5f,
                0.5f - previousClip.y / previousClip.w * 0.5f);
            if (previousUv.x < 0.0f || previousUv.x > 1.0f
                || previousUv.y < 0.0f || previousUv.y > 1.0f)
            {
                return 0.0f.xxxx;
            }

            // 現在画面の縁までのUV距離
            const float2 currentEdge = min(hitUv, 1.0f - hitUv);
            // 履歴画面の縁までのUV距離
            const float2 previousEdge = min(previousUv, 1.0f - previousUv);
            // 両画面で近い縁までの距離
            const float edgeDistance = min(
                min(currentEdge.x, currentEdge.y),
                min(previousEdge.x, previousEdge.y));
            // 画面の縁に近い反射の減衰率
            const float edgeFade = saturate(edgeDistance / 0.08f);
            // レイ始点から交差までの距離
            const float travelled = saturate(worldRatio)
                * length(rayEnd - rayStart);
            // 最大距離に対する到達距離
            const float travelledFraction = saturate(
                travelled / maximumDistance);
            // 最大距離の最後の四分の一の減衰
            const float distanceFade = saturate(
                (1.0f - travelledFraction) / 0.25f);
            // カメラに戻る反射の度合い
            const float towardCamera = saturate(
                dot(reflection, viewDirection));
)"
        R"(            // 裏面へ向かう反射の減衰率
            const float directionFade = saturate(
                (1.0f - towardCamera) / 0.5f);
            // 粗さ上限に近い反射の減衰率
            const float roughnessFade = saturate(
                1.0f - roughness / roughnessCutoff);
            // 前フレームで読んだ反射RGB
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

        // 面の厚みの外を通るため、同じミップで次の区画へ進みます。
        alpha = exitAlpha;
    }
    return 0.0f.xxxx;
}

// PBR計算に使う円周率
static const float LamaPonPi = 3.14159265f;

// 光源円盤内の鏡面反射の代表方向を求めます(toLight: 光源への単位方向, normal: 単位法線, viewDirection: 視点への単位方向, angularRadius: 光源の角半径)。
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
    // 法線で反射した視線方向
    const float3 reflected = reflect(-viewDirection, normal);
    // 光源方向と反射方向の内積
    const float alignment = dot(toLight, reflected);
    // 光源の角半径のcos
    const float diskCosine = cos(angularRadius);
    if (alignment >= diskCosine)
    {
        // 法線で反射した視線方向
        return reflected;
    }
    // 反射方向の光源に垂直な成分
    const float3 sideways = reflected - alignment * toLight;
    // 垂直成分の長さ
    const float sidewaysLength = length(sideways);
    if (sidewaysLength <= 1.0e-5f)
    {
        return toLight;
    }
    return normalize(
        toLight * diskCosine
        + (sideways / sidewaysLength) * sin(angularRadius));
}

// 光源円盤で広がった鏡面反射の明るさを補正します(roughness: 粗さ, angularRadius: 光源の角半径)。
float SourceSpecularEnergy(
    float roughness,
    float angularRadius)
{
    if (angularRadius <= 0.0f)
    {
        return 1.0f;
    }
    // 粗さの二乗で得る分布幅
    const float alpha = max(roughness * roughness, 1.0e-4f);
    // 光源円盤で広げた分布幅
    const float widened =
        saturate(alpha + sin(angularRadius) * 0.5f);
    // 元の分布幅と補正後の比率
    const float ratio = alpha / max(widened, 1.0e-4f);
    return ratio * ratio;
}

// 実光源方向の拡散と代表方向のGGX鏡面光を合成します(normal: 単位法線, toLight: 光源への単位方向, specularToLight: 鏡面用の代表方向, viewDirection: 視点への単位方向, albedo: 基本色RGB, roughness: 粗さ, metallic: 金属度, radiance: 入射光RGB, specularEnergy: 鏡面のエネルギー補正)。
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
    // 法線と光源方向の内積
    const float normalDotLight =
        saturate(dot(normal, toLight));
    if (normalDotLight <= 0.0f)
    {
        return 0.0f.xxx;
    }
    // 視点方向と鏡面光方向の半角
    const float3 halfVector =
        normalize(specularToLight + viewDirection);
    // 法線と視点方向の内積
    const float normalDotView = max(
        dot(normal, viewDirection),
        0.0001f);
    // 法線と半角ベクトルの内積
    const float normalDotHalf =
        saturate(dot(normal, halfVector));
    // 視点方向と半角方向の内積
    const float viewDotHalf =
        saturate(dot(viewDirection, halfVector));

    // 粗さの二乗で得る分布幅
    const float alpha = roughness * roughness;
    // GGX分布幅の二乗
    const float alphaSquared = alpha * alpha;
    // GGX法線分布の分母項
    const float denominator =
        normalDotHalf * normalDotHalf
            * (alphaSquared - 1.0f)
        + 1.0f;
    // GGXの微小面法線分布
    const float distribution =
        alphaSquared
        / max(LamaPonPi * denominator * denominator,
            1.0e-12f);

    // Smith遮蔽近似の係数
    const float k = alpha * 0.5f + 0.0001f;
    // 視点側の微小面遮蔽率
    const float geometryView =
        normalDotView / (normalDotView * (1.0f - k) + k);
    // 光源側の微小面遮蔽率
    const float geometryLight =
        normalDotLight
        / (normalDotLight * (1.0f - k) + k);
    // 両方向の微小面遮蔽率
    const float geometry = geometryView * geometryLight;

    // 垂直入射時の反射率RGB
    const float3 f0 = lerp(0.04f.xxx, albedo, metallic);
    // Schlick近似の反射率RGB
    const float3 fresnel =
        f0
        + (1.0f.xxx - f0)
            * pow(1.0f - viewDotHalf, 5.0f);

    // GGX鏡面の反射RGB
    const float3 specular =
        distribution * geometry * fresnel
        * specularEnergy
        / max(4.0f * normalDotView * normalDotLight,
            0.0001f);
    // 非金属成分の拡散RGB
    const float3 diffuse =
        (1.0f.xxx - fresnel)
        * (1.0f - metallic)
        * albedo
        / LamaPonPi;
    return (diffuse + specular)
        * radiance
        * normalDotLight;
}

// 点光源方向を拡散と鏡面に用いてGGX照明を求めます(normal: 単位法線, toLight: 光源への単位方向, viewDirection: 視点への単位方向, albedo: 基本色RGB, roughness: 粗さ, metallic: 金属度, radiance: 入射光RGB)。
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

// 法線・間接光・直接光・発光・霧を合成してRGBAを返します(input: 射影位置・ワールド位置・法線・UV・色)。
float4 PrimitivePixelShader(PixelInput input) : SV_Target
{
    // 素材法線を適用する単位法線
    float3 normal = normalize(input.normal);
    if (MaterialProperties.w > 0.5f)
    {
        // 法線画像のXYに掛ける強度
        const float strength = max(EmissiveFactor.w, 0.0f);
        // 強度を適用した接線空間XY
        const float2 sampledNormalXY = (NormalTexture.Sample(
            AlbedoSampler,
            input.textureCoordinate).xy * 2.0f - 1.0f) * strength;
        // XYからZを復元した素材法線
        const float3 sampledNormal = float3(
            sampledNormalXY,
            sqrt(saturate(
                1.0f - dot(sampledNormalXY, sampledNormalXY))));
        // ワールド位置の横方向微分
        const float3 positionDx = ddx(input.worldPosition);
        // ワールド位置の縦方向微分
        const float3 positionDy = ddy(input.worldPosition);
        // UVの横方向微分
        const float2 uvDx = ddx(input.textureCoordinate);
        // UVの縦方向微分
        const float2 uvDy = ddy(input.textureCoordinate);
        // UV微分から得た未正規化接線
        const float3 tangentUnscaled =
            cross(positionDy, normal) * uvDx.x
            + cross(normal, positionDx) * uvDy.x;
        // UV微分から得た未正規化従接線
        const float3 bitangentUnscaled =
            cross(positionDy, normal) * uvDx.y
            + cross(normal, positionDx) * uvDy.y;
        // 接線基底の共通長さの逆数
)"
        R"(        const float inverseScale = rsqrt(max(
            max(dot(tangentUnscaled, tangentUnscaled),
                dot(bitangentUnscaled, bitangentUnscaled)),
            0.000001f));
        // 素材法線を回転する接線
        const float3 tangent = tangentUnscaled * inverseScale;
        // 素材法線を回転する従接線
        const float3 bitangent = bitangentUnscaled * inverseScale;
        normal = normalize(
            tangent * sampledNormal.x
            + bitangent * sampledNormal.y
            + normal * sampledNormal.z);
    }
    // 基本色画像のRGBA
    const float4 albedo = AlbedoTexture.Sample(
        AlbedoSampler,
        input.textureCoordinate);
    // 頂点色を乗算した基本色RGB
    const float3 surfaceColor = albedo.rgb * input.tint.rgb;
    // 粗さ画像のG成分
    const float roughnessSample = RoughnessTexture.Sample(
        AlbedoSampler,
        input.textureCoordinate).g;
    // 金属度画像のB成分
    const float metallicSample = MetallicTexture.Sample(
        AlbedoSampler,
        input.textureCoordinate).b;
    // 素材AO画像のR成分
    const float occlusionSample = OcclusionTexture.Sample(
        AlbedoSampler,
        input.textureCoordinate).r;
    // 発光画像のRGB
    const float3 emissiveSample = EmissiveTexture.Sample(
        AlbedoSampler,
        input.textureCoordinate).rgb;
    // 素材と画像を合成した粗さ
    const float roughness = clamp(
        MaterialProperties.x * roughnessSample,
        0.02f,
        1.0f);
    // 素材と画像を合成した金属度
    const float metallic = saturate(
        MaterialProperties.y * metallicSample);
    // 間接光だけに適用する遮蔽率
    float occlusion = lerp(
        1.0f,
        occlusionSample,
        saturate(MaterialProperties.z));
    // SSAOは間接光項だけに掛け、直接光を暗くしません。
    if (ScreenAmbientOcclusionParameters.z >= 0.5f)
    {
        // SSAO画像を読む画面UV
        const float2 screenUv =
            input.position.xy * ScreenAmbientOcclusionParameters.xy;
        occlusion *= ScreenAmbientOcclusionTexture.Sample(
            AlbedoSampler,
            screenUv).r;
    }
    // 素材の垂直入射反射率RGB
    const float3 specularColor = lerp(0.04f.xxx, surfaceColor, metallic);
    // 画素から視点への単位方向
    const float3 viewDirection = normalize(
        CameraPosition.xyz - input.worldPosition);
    // 互換下限を適用した照明用粗さ
    const float environmentRoughness =
        clamp(MaterialProperties.x * roughnessSample, 0.04f, 1.0f);
    // SSRのRGBと信頼度
    const float4 screenReflection = EvaluateScreenSpaceReflection(
        input.worldPosition,
        reflect(-viewDirection, normal),
        viewDirection,
        environmentRoughness);
    // 環境光とGIの合成RGB
    float3 ambient = 0.0f.xxx;
    if (EnvironmentParameters.y < 0.5f)
    {
        // 環境画像がない場合は金属の環境光を弱め、SSRの反射をF0で重み付けします。
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
        // 畳込み済み反射画像の最終段
        const float prefilteredMaximumMip = EnvironmentParameters.z;
        // 反射画像を参照する最終段
        float maximumMip = prefilteredMaximumMip;
        if (maximumMip <= 0.0f)
        {
            // 反射キューブの幅
            uint width;
            // 反射キューブの高さ
            uint height;
            // 反射キューブのミップ数
            uint mipCount;
            EnvironmentMap.GetDimensions(0, width, height, mipCount);
            maximumMip = max((float)mipCount - 1.0f, 0.0f);
        }
        // 法線方向の放射照度RGB
        float3 irradiance = prefilteredMaximumMip > 0.0f
            ? IrradianceMap.SampleLevel(AlbedoSampler, normal, 0.0f).rgb
            : EnvironmentMap.SampleLevel(
                AlbedoSampler,
                normal,
                maximumMip).rgb;
        // 粗さに対応する反射RGB
        float3 prefiltered = SampleProbeSpecular(
            EnvironmentMap,
            normal,
            viewDirection,
            input.worldPosition,
            environmentRoughness,
            maximumMip,
            ReflectionBoxCenter,
            ReflectionBoxParameters);
        // 第2反射プローブの混合率
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
        // 法線と視点方向の内積
        const float normalDotView = max(dot(normal, viewDirection), 0.0001f);
        // Karis BRDF近似の第1係数
        const float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
        // Karis BRDF近似の第2係数
        const float4 c1 = float4(1.0f, 0.0425f, 1.04f, -0.04f);
        // 粗さを適用したBRDF係数
        const float4 r = environmentRoughness * c0 + c1;
        // BRDF近似の反射補助項
        const float a004 =
            min(r.x * r.x, exp2(-9.28f * normalDotView)) * r.x + r.y;
        // 鏡面反射の倍率と加算項
        const float2 brdf = float2(-1.04f, 1.04f) * a004 + r.zw;
        // IBLの非金属拡散RGB
        const float3 diffuse = irradiance * surfaceColor * (1.0f - metallic);
        // IBLの鏡面反射RGB
        const float3 specular =
            prefiltered * (specularColor * brdf.x + brdf.y);
        ambient = (diffuse + specular) * EnvironmentParameters.x
            + surfaceColor
                * EvaluateBakedAmbient(input.worldPosition, normal);
    }
    // 照明と発光を蓄積するRGB
    float3 result = ambient * occlusion;
    // 平行光の番号
    [loop]
    for (uint index = 0; index < min(LightCounts.x, 4u); ++index)
    {
        // 対象ライトの影の可視率
        const float shadow = EvaluateDirectionalShadow(
            input.worldPosition,
            normal,
            index);
        // 画素から光源への単位方向
        const float3 toLight = normalize(
            -DirectionalDirectionIntensity[index].xyz);
        // 太陽光源の角半径
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

)"
        R"(    if (ClusteredParameters.w >= 0.5f)
    {
        // クラスタの横方向分割数
        const uint gridX = (uint)ClusteredParameters.x;
        // クラスタの縦方向分割数
        const uint gridY = (uint)ClusteredParameters.y;
        // クラスタの深度方向分割数
        const uint gridZ = (uint)ClusteredParameters.z;
        // 画素位置の正規化画面座標
        const float2 screenRatio = saturate(
            input.position.xy
            * ClusteredScreenParameters.xy);
        // 画素が属する横クラスタ番号
        const uint clusterX = min(
            (uint)(screenRatio.x * gridX),
            gridX - 1u);
        // 画素が属する縦クラスタ番号
        const uint clusterY = min(
            (uint)(screenRatio.y * gridY),
            gridY - 1u);
        // クラスタ深度分割の近距離
        const float nearPlane = ClusteredDepthParameters.x;
        // カメラ前方へのビュー距離
        const float viewDepth = max(
            dot(
                CameraForwardShadowTexel.xyz,
                input.worldPosition - CameraPosition.xyz),
            nearPlane);
        // 画素が属する深度クラスタ番号
        const uint clusterZ = min(
            (uint)(log(viewDepth / nearPlane)
                / ClusteredDepthParameters.z
                * gridZ),
            gridZ - 1u);
        // XYZを平坦化したクラスタ番号
        const uint cluster =
            clusterZ * gridX * gridY
            + clusterY * gridX
            + clusterX;
        // クラスタ別ライト数の上限
        const uint maximumPerCluster =
            (uint)ClusteredDepthParameters.w;
        // クラスタのライト番号一覧の先頭
        const uint clusterOffset = cluster * maximumPerCluster;
        // クラスタ内で評価するライト数
        const uint clusterLightCount = min(
            ClusterLightCounts[cluster],
            maximumPerCluster);
        // クラスタ内のライト参照番号
        [loop]
        for (uint slot = 0; slot < clusterLightCount; ++slot)
        {
            // クラスタで選んだライト情報
            const ClusterLight light = ClusterLights[
                ClusterLightIndexList[clusterOffset + slot]];
            // 画素から点光源へのベクトル
            const float3 delta =
                light.PositionRange.xyz - input.worldPosition;
            // 画素から光源までの距離
            const float lightDistance = length(delta);
            // 対象光源の影響範囲
            const float range = max(light.PositionRange.w, 0.001f);
            // 範囲末端へ二乗で落とす光強度
            const float distanceAttenuation =
                pow(saturate(1.0f - lightDistance / range), 2.0f);
            // 画素から光源への単位方向
            const float3 toLight =
                delta / max(lightDistance, 0.0001f);
            // 距離とコーンを合成した減衰率
            float attenuation = distanceAttenuation;
            // 対象ライトの影の可視率
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
                // 光源方向と照射方向の内積
                const float cone = dot(
                    normalize(light.DirectionInnerCosine.xyz),
                    -toLight);
                // 内外角に応じたコーン減衰率
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
    // 従来経路の点光源番号
    [loop]
    for (uint pointIndex = 0; pointIndex < min(LightCounts.y, 16u); ++pointIndex)
    {
        // 画素から点光源へのベクトル
        const float3 delta = PointPositionRange[pointIndex].xyz - input.worldPosition;
        // 画素から光源までの距離
        const float lightDistance = length(delta);
        // 対象光源の影響範囲
        const float range = max(PointPositionRange[pointIndex].w, 0.001f);
        // 距離とコーンを合成した減衰率
        const float attenuation = pow(saturate(1.0f - lightDistance / range), 2.0f);
        // 対象ライトの影の可視率
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

    // 従来経路のスポット光番号
    [loop]
    for (uint spotIndex = 0; spotIndex < min(LightCounts.z, 8u); ++spotIndex)
    {
        // スポット光源から画素への差
        const float3 lightToPixel = input.worldPosition - SpotPositionRange[spotIndex].xyz;
        // 画素から光源までの距離
        const float lightDistance = length(lightToPixel);
        // 対象光源の影響範囲
        const float range = max(SpotPositionRange[spotIndex].w, 0.001f);
        // スポット光源から画素への方向
        const float3 rayDirection = lightToPixel / max(lightDistance, 0.0001f);
        // 光源方向と照射方向の内積
        const float cone = dot(normalize(SpotDirectionInnerCosine[spotIndex].xyz), rayDirection);
        // スポット光のコーン減衰率
        const float coneAttenuation = smoothstep(
            SpotOuterCosine[spotIndex].x,
            SpotDirectionInnerCosine[spotIndex].w,
            cone);
        // 範囲末端へ二乗で落とす光強度
        const float distanceAttenuation = pow(saturate(1.0f - lightDistance / range), 2.0f);
        // スポット影の面番号に1を足す値
        const uint shadowSlot =
            (uint)SpotOuterCosine[spotIndex].y;
        // 対象ライトの影の可視率
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
)"
        R"(    }
    }
    result += emissiveSample * EmissiveFactor.rgb;
    // 画像と頂点色を掛けた透過度
    const float outputAlpha = albedo.a * input.tint.a;
    if (FogParameters.w < 0.5f)
    {
        return float4(result, outputAlpha);
    }
    if (FogColorModel.w < 0.5f)
    {
        // 負の値を除いた照明RGB
        const float3 litColor = max(result, 0.0f);
        // 画素から視点までの距離
        const float distanceToCamera =
            length(input.worldPosition - CameraPosition.xyz);
        // 開始距離から終端までの霧率
        const float rangeFog = smoothstep(
            FogParameters.x,
            max(FogParameters.y, FogParameters.x + 0.001f),
            distanceToCamera);
        // 距離と密度による指数霧率
        const float exponentialFog = 1.0f
            - exp(
                -max(FogParameters.z, 0.0f)
                * max(distanceToCamera - FogParameters.x, 0.0f));
        // 範囲霧と指数霧の濃い方
        const float fogAmount = saturate(max(rangeFog, exponentialFog));
        return float4(
            lerp(litColor, FogColorModel.rgb, fogAmount),
            outputAlpha);
    }
    // カメラ前方へのビュー距離
    const float viewDepth = dot(
        input.worldPosition - CameraPosition.xyz,
        CameraForwardShadowTexel.xyz);
    // 互換方式の線形ビュー深度霧率
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

    // 標準粒子とカスタムPS互換VSのHLSL
    constexpr char ParticleShaderSource[] = R"(
// 粒子VSのb0射影行列定数
cbuffer ParticleConstants : register(b0)
{
    // ビューと射影の合成行列
    row_major float4x4 ViewProjection;
};

// t0の粒子画像
Texture2D ParticleTexture : register(t0);
// s0の線形繰り返しサンプラー
SamplerState ParticleSampler : register(s0);

struct VertexInput
{
    // 粒子のワールド位置
    float3 position : POSITION;
    // 頂点のRGBA色
    float4 color : COLOR;
    // 粒子画像のUV座標
    float2 textureCoordinate : TEXCOORD;
};

struct PixelInput
{
    // 頂点のクリップ座標
    float4 position : SV_Position;
    // 頂点のRGBA色
    float4 color : COLOR;
    // 粒子画像のUV座標
    float2 textureCoordinate : TEXCOORD;
};

// 粒子頂点を射影して色とUVを渡します(input: ワールド位置・RGBA色・UV)。
PixelInput ParticleVertexShader(VertexInput input)
{
    // 画素段へ渡す粒子頂点出力
    PixelInput output;
    output.position = mul(float4(input.position, 1.0f), ViewProjection);
    output.color = input.color;
    output.textureCoordinate = input.textureCoordinate;
    return output;
}

// 粒子画像に頂点色を掛けたRGBAを返します(input: 射影位置・RGBA色・UV)。
float4 ParticlePixelShader(PixelInput input) : SV_Target
{
    return ParticleTexture.Sample(ParticleSampler, input.textureCoordinate)
        * input.color;
}

// カスタム粒子PSとの互換のためCOLOR0・TEXCOORD0・SV_Positionの順を維持します。
struct CustomPixelInput
{
    // 頂点のRGBA色
    float4 color : COLOR0;
    // 粒子画像のUV座標
    float2 textureCoordinate : TEXCOORD0;
    // 頂点のクリップ座標
    float4 position : SV_Position;
};

// カスタムPSの互換出力順で粒子頂点を射影します(input: ワールド位置・RGBA色・UV)。
CustomPixelInput CustomParticleVertexShader(VertexInput input)
{
    // 画素段へ渡す粒子頂点出力
    CustomPixelInput output;
    output.color = input.color;
    output.textureCoordinate = input.textureCoordinate;
    output.position = mul(float4(input.position, 1.0f), ViewProjection);
    return output;
}
)";

    // 失敗HRESULTを操作名付きの例外に変えます(result: 呼出し結果, operation: 操作名)。
    void ThrowIfFailed(const HRESULT result, const char* operation)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(
                std::string(operation) + " failed with HRESULT "
                + std::to_string(static_cast<unsigned long>(result)));
        }
    }

    // 埋め込みHLSLを最適化付きでコンパイルし、失敗診断を例外にします(source: HLSL文字列, sourceSize: 文字列のバイト数, sourceName: 診断用のソース名, entryPoint: 入口関数名, target: シェーダー形式)。
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob> CompileShader(
        const char* source,
        std::size_t sourceSize,
        const char* sourceName,
        const char* entryPoint,
        const char* target)
    {
        // コンパイル済みシェーダー
        Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
        // コンパイラーの診断文字列
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        // 生成する形状または描画状態
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
            // 失敗時に例外へ渡す診断
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
        // 位置・法線・UVの頂点列
        std::vector<PrimitiveRenderVertex> vertices;
        // 三角形を構成する頂点索引
        std::vector<std::uint32_t> indices;
    };

    // 四角面の頂点4個と三角形2個を追加します(geometry: 追記先の形状, normal: 面の法線, positions: UV順の四隅)。
    void AddFace(
        Geometry& geometry,
        const DirectX::XMFLOAT3& normal,
        const std::array<DirectX::XMFLOAT3, 4>& positions)
    {
        // 追加する三角形群の先頭頂点番号
        const auto first = static_cast<std::uint32_t>(geometry.vertices.size());
        // 四隅に対応するUV座標
        constexpr std::array<DirectX::XMFLOAT2, 4> ultravioletCoordinates{ {
            { 0.0f, 1.0f }, { 0.0f, 0.0f },
            { 1.0f, 1.0f }, { 1.0f, 0.0f }
        } };
        // 面の頂点または三角形の索引位置
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

    // 指定位置以降の三角形の第2・第3頂点を交換します(geometry: 巻き順を反転する形状, firstIndex: 開始する索引位置)。
    void ReverseWinding(Geometry& geometry, const std::size_t firstIndex = 0u)
    {
        // 面の頂点または三角形の索引位置
        for (std::size_t index = firstIndex;
             index + 2u < geometry.indices.size();
             index += 3u)
        {
            std::swap(geometry.indices[index + 1u], geometry.indices[index + 2u]);
        }
    }

    // 時計回りを表面とする箱を生成します(half: 各軸の半寸法, includeTop: 上面を付ける指定)。
    [[nodiscard]] Geometry CreateBox(
        const DirectX::XMFLOAT3& half,
        const bool includeTop)
    {
        // 生成する形状または描画状態
        Geometry result;
        // 形状のX座標または半幅
        const float x = half.x;
        // 形状のY座標または半高さ
        const float y = half.y;
        // 形状のZ座標または半奥行
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

    // 各辺が1の立方体を生成します。
    [[nodiscard]] Geometry CreateCube()
    {
        return CreateBox({ 0.5f, 0.5f, 0.5f }, true);
    }

    // 互換上面UVを持つ厚さ0.05の箱型平面を生成します。
    [[nodiscard]] Geometry CreatePlane()
    {
        // 平面箱の半分の厚さ
        constexpr float halfThickness = 0.025f;
        // 生成する形状または描画状態
        auto result = CreateBox({ 0.5f, halfThickness, 0.5f }, false);
        AddFace(result, { 0, 1, 0 }, { {
            { -0.5f, halfThickness, 0.5f }, { -0.5f, halfThickness, -0.5f },
            { 0.5f, halfThickness, 0.5f }, { 0.5f, halfThickness, -0.5f } } });
        return result;
    }

    // 直径1の球を経度24・緯度16分割で生成します。
    [[nodiscard]] Geometry CreateSphere()
    {
        // 生成する形状または描画状態
        Geometry result;
        // 周方向の分割数
        constexpr std::uint32_t slices = 24;
        // 球の緯度方向の分割数
        constexpr std::uint32_t stacks = 16;
        // 球の緯度分割番号
        for (std::uint32_t stack{}; stack <= stacks; ++stack)
        {
            // 球の緯度を正規化したUV
            const float v = static_cast<float>(stack) / stacks;
            // 球の緯度角
            const float latitude = v * std::numbers::pi_v<float>;
            // 形状のY座標または半高さ
            const float y = std::cos(latitude) * 0.5f;
            // 球の緯度輪の半径
            const float radius = std::sin(latitude) * 0.5f;
            // 周方向の分割番号
            for (std::uint32_t slice{}; slice <= slices; ++slice)
            {
                // 周方向を正規化したUV
                const float u = static_cast<float>(slice) / slices;
                // 球の経度角
                const float longitude = u * std::numbers::pi_v<float> * 2.0f;
                // 球面上の頂点位置
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
        // 球の緯度分割番号
        for (std::uint32_t stack{}; stack < stacks; ++stack)
        {
            // 周方向の分割番号
            for (std::uint32_t slice{}; slice < slices; ++slice)
            {
                // 追加する三角形群の先頭頂点番号
                const auto first = stack * (slices + 1u) + slice;
                // 次の緯度輪または次の側面頂点
                const auto next = first + slices + 1u;
                result.indices.insert(result.indices.end(), {
                    first, next, first + 1u,
                    first + 1u, next, next + 1u });
            }
        }
        ReverseWinding(result);
        return result;
    }

    // 高さと直径が1の円柱を周方向24分割で生成します。
    [[nodiscard]] Geometry CreateCylinder()
    {
        // 生成する形状または描画状態
        Geometry result;
        // 周方向の分割数
        constexpr std::uint32_t slices = 24;
        // 周方向の分割番号
        for (std::uint32_t slice{}; slice <= slices; ++slice)
        {
            // 周方向を正規化したUV
            const float u = static_cast<float>(slice) / slices;
            // 円柱の周方向の角度
            const float angle = u * std::numbers::pi_v<float> * 2.0f;
            // 形状のX座標または半幅
            const float x = std::sin(angle) * 0.5f;
            // 形状のZ座標または半奥行
            const float z = std::cos(angle) * 0.5f;
            // 円柱側面の単位法線
            const DirectX::XMFLOAT3 normal{ x * 2.0f, 0.0f, z * 2.0f };
            result.vertices.push_back({ { x, -0.5f, z }, normal, { u, 1 } });
            result.vertices.push_back({ { x, 0.5f, z }, normal, { u, 0 } });
        }
        // 周方向の分割番号
        for (std::uint32_t slice{}; slice < slices; ++slice)
        {
            // 追加する三角形群の先頭頂点番号
            const auto first = slice * 2u;
            result.indices.insert(result.indices.end(), {
                first, first + 1u, first + 2u,
                first + 2u, first + 1u, first + 3u });
        }
        // 円柱の底面中心の頂点番号
        const auto bottomCenter = static_cast<std::uint32_t>(result.vertices.size());
        result.vertices.push_back({ { 0, -0.5f, 0 }, { 0, -1, 0 }, { 0.5f, 0.5f } });
        // 円柱の上面中心の頂点番号
        const auto topCenter = static_cast<std::uint32_t>(result.vertices.size());
        result.vertices.push_back({ { 0, 0.5f, 0 }, { 0, 1, 0 }, { 0.5f, 0.5f } });
        // 円柱の蓋の索引列の開始位置
        const auto capIndex = result.indices.size();
        // 周方向の分割番号
        for (std::uint32_t slice{}; slice < slices; ++slice)
        {
            // 円柱側面の下側頂点番号
            const auto side = slice * 2u;
            // 次の緯度輪または次の側面頂点
            const auto next = (slice + 1u) * 2u;
            result.indices.insert(result.indices.end(), {
                bottomCenter, next, side,
                topCenter, side + 1u, next + 1u });
        }
        // 側面は外向きのため、上下の蓋だけの巻き順を反転します。
        ReverseWinding(result, capIndex);
        return result;
    }

    // 通常のアルファ合成状態を全描画先へ設定します(enabled: 合成を有効にする指定)。
    [[nodiscard]] D3D12_BLEND_DESC MakeBlendDescription(bool enabled) noexcept
    {
        // 描画先1枚の合成状態
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
        // 生成する形状または描画状態
        D3D12_BLEND_DESC result{};
        // 合成状態を設定する描画先
        for (auto& renderTarget : result.RenderTarget)
        {
            renderTarget = target;
        }
        return result;
    }

    // 粒子用の透過合成または加算合成を作ります(additive: 加算合成の指定)。
    [[nodiscard]] D3D12_BLEND_DESC MakeParticleBlendDescription(
        bool additive) noexcept
    {
        // 描画先1枚の合成状態
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
        // 生成する形状または描画状態
        D3D12_BLEND_DESC result{};
        // 合成状態を設定する描画先
        for (auto& renderTarget : result.RenderTarget)
        {
            renderTarget = target;
        }
        return result;
    }

    // 時計回りを表面とし、ワイヤー時は両面の辺を描きます(wireframe: 辺だけの描画指定, cull: 除去する面)。
    [[nodiscard]] D3D12_RASTERIZER_DESC MakeRasterizerDescription(
        const bool wireframe = false,
        const LamaPon::ShaderCullMode cull =
            LamaPon::ShaderCullMode::None) noexcept
    {
        // 生成する形状または描画状態
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

    // 深度の比較・書込み状態を作ります(depthTest: 以下比較を使う指定, depthWrite: 深度を更新する指定)。
    [[nodiscard]] D3D12_DEPTH_STENCIL_DESC MakeDepthDescription(
        bool depthTest,
        bool depthWrite) noexcept
    {
        // 生成する形状または描画状態
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
        // 初期化済みバックエンドの標準・粒子・素材描画資源を作ります(backend: 本サービスより長く生存する描画基盤)。
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

            // t0～t21を個別に渡すSRV範囲
            std::array<D3D12_DESCRIPTOR_RANGE, 22> textureRanges{};
            // ルート引数またはSRV枠の番号
            for (UINT index{}; index < textureRanges.size(); ++index)
            {
                textureRanges[index].RangeType =
                    D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                textureRanges[index].NumDescriptors = 1;
                textureRanges[index].BaseShaderRegister = index;
            }
            // b0と22個の画像のルート引数
            std::array<D3D12_ROOT_PARAMETER, 23> parameters{};
            parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            parameters[0].Descriptor.ShaderRegister = 0;
            parameters[0].Descriptor.RegisterSpace = 0;
            parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            // ルート引数またはSRV枠の番号
            for (std::size_t index{}; index < textureRanges.size(); ++index)
            {
                // 構築中のルート引数
                auto& parameter = parameters[index + 1u];
                parameter.ParameterType =
                    D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                parameter.DescriptorTable.NumDescriptorRanges = 1;
                parameter.DescriptorTable.pDescriptorRanges =
                    &textureRanges[index];
                parameter.ShaderVisibility =
                    D3D12_SHADER_VISIBILITY_PIXEL;
            }
            // 素材用と影比較用のサンプラー
            std::array<D3D12_STATIC_SAMPLER_DESC, 2> samplers{};
            // s0の線形繰り返しサンプラー
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
            // s1の白境界・以下比較サンプラー
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
            // 標準描画のルート署名の定義
            D3D12_ROOT_SIGNATURE_DESC description{};
            description.NumParameters = static_cast<UINT>(parameters.size());
            description.pParameters = parameters.data();
            description.NumStaticSamplers =
                static_cast<UINT>(samplers.size());
            description.pStaticSamplers = samplers.data();
            description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
            // 標準描画の直列化ルート署名
            Microsoft::WRL::ComPtr<ID3DBlob> serialized;
            // 標準署名の直列化診断
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

            // 頂点のb0とPSのb0・b1を別枠で渡し、D3D11の粒子PSとの互換を保ちます。
            {
                // 粒子PSのt0・t1のSRV範囲
                std::array<D3D12_DESCRIPTOR_RANGE, 2> particleRanges{};
                // ルート引数またはSRV枠の番号
                for (UINT index{}; index < particleRanges.size(); ++index)
                {
                    particleRanges[index].RangeType =
                        D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                    particleRanges[index].NumDescriptors = 1;
                    particleRanges[index].BaseShaderRegister = index;
                }
                // VSとPSを分離した粒子引数
                std::array<D3D12_ROOT_PARAMETER, 5> particleParameters{};
                particleParameters[0].ParameterType =
                    D3D12_ROOT_PARAMETER_TYPE_CBV;
                particleParameters[0].Descriptor.ShaderRegister = 0;
                particleParameters[0].ShaderVisibility =
                    D3D12_SHADER_VISIBILITY_VERTEX;
                // ルート引数またはSRV枠の番号
                for (UINT index{}; index < 2u; ++index)
                {
                    // 構築中のルート引数
                    auto& parameter = particleParameters[index + 1u];
                    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                    parameter.Descriptor.ShaderRegister = index;
                    parameter.ShaderVisibility =
                        D3D12_SHADER_VISIBILITY_PIXEL;
                }
                // ルート引数またはSRV枠の番号
                for (UINT index{}; index < particleRanges.size(); ++index)
                {
                    // 構築中のルート引数
                    auto& parameter = particleParameters[index + 3u];
                    parameter.ParameterType =
                        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                    parameter.DescriptorTable.NumDescriptorRanges = 1;
                    parameter.DescriptorTable.pDescriptorRanges =
                        &particleRanges[index];
                    parameter.ShaderVisibility =
                        D3D12_SHADER_VISIBILITY_PIXEL;
                }
                // 粒子PSのs0線形サンプラー
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
                // カスタム粒子のルート署名定義
                D3D12_ROOT_SIGNATURE_DESC particleDescription{};
                particleDescription.NumParameters =
                    static_cast<UINT>(particleParameters.size());
                particleDescription.pParameters = particleParameters.data();
                particleDescription.NumStaticSamplers = 1;
                particleDescription.pStaticSamplers = &particleSampler;
                particleDescription.Flags =
                    D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
                // 粒子用の直列化ルート署名
                Microsoft::WRL::ComPtr<ID3DBlob> particleSignature;
                // 粒子署名の直列化診断
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

        // 形状を解決して素材シェーダーの描画を委譲します(assets: 資産の読込元, shader: 使用する素材HLSL, placeholder: 失敗時の代替HLSL, prepass: 深度事前描画の素材選別指定, request: 形状と描画状態, material: 素材値とスキニング情報, lighting: 照明状態)。
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
            // 素材描画へ渡す借用頂点列
            std::span<const PrimitiveRenderVertex> vertices;
            // 素材描画へ渡す借用三角形索引
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

        // 素材シェーダーを次回使用時の再読込み対象にします(shaderPath: キャッシュ対象のパス)。
        void InvalidateMaterialShader(
            const std::filesystem::path& shaderPath) noexcept override
        {
            m_materialShaders->Invalidate(shaderPath);
        }

        // 素材シェーダーの宣言済み描画状態を取得します(cacheKey: キャッシュ識別パス, state: 取得状態の出力先)。
        [[nodiscard]] bool TryGetMaterialShaderRenderState(
            const std::filesystem::path& cacheKey,
            LamaPon::ShaderRenderState& state) const noexcept override
        {
            return m_materialShaders->TryGetRenderState(cacheKey, state);
        }

        // 素材HLSLを準備して使用できる描画パスを返します(assets: 資産の読込元, shader: 素材HLSLと診断処理)。
        [[nodiscard]] LamaPon::Detail::MaterialShaderPasses
            PrepareMaterialShaderPasses(
                LamaPon::AssetManager& assets,
                const LamaPon::Detail::MaterialShaderSource& shader) override
        {
            return m_materialShaders->PreparePasses(assets, shader);
        }

        // カスタムPSで粒子を描き、失敗時は代替PSを試します(assets: 資産の読込元, shader: 使用する粒子HLSL, placeholder: 失敗時の代替HLSL, request: 完全な四角粒子の描画要求, parameters: PSのb0へ渡す8ベクトル)。
        [[nodiscard]] LamaPon::Detail::MaterialShaderDrawResult
            DrawCustomParticles(
                LamaPon::AssetManager& assets,
                const LamaPon::Detail::MaterialShaderSource& shader,
                const LamaPon::Detail::MaterialShaderSource& placeholder,
                const LamaPon::ParticleDrawRequest& request,
                const std::array<DirectX::XMFLOAT4, 8>& parameters) override
        {
            // 描画結果と世代・失敗情報
            LamaPon::Detail::MaterialShaderDrawResult result;
            // 使用する粒子PSのキャッシュ項目
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

            // 代替PSが使えれば描画先を切り替え、失敗情報を保持します。
            const auto usePlaceholder = [&]()
            {
                // 失敗表示用の粒子PS項目
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
            // D3D11のApplyCustomPixelShaderと同じく、説明の付いた失敗だけをマゼンタの代替表示で描きます。
            if (active->pixelShader == nullptr
                && (active->error.empty() || !usePlaceholder()))
            {
                return result;
            }
            // 使用する粒子描画PSO
            ID3D12PipelineState* pipeline{};
            try
            {
                pipeline = CustomParticlePipelineState(
                    *active,
                    request.additive);
            }
            // exception: 主PSのPSO作成失敗。
            catch (const std::exception& exception)
            {
                if (result.placeholder)
                {
                    throw;
                }
                // 頂点出力との並びが合わないなど、pipelineを作れないShaderもcompile失敗と同じく説明を出して代替表示で描きます。
                active->error = shader.describeFailure
                    ? shader.describeFailure(exception.what())
                    : std::string(exception.what());
                active->pixelShader.Reset();
                // 失敗したPSに依存するPSO
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

            // 指定画像を解決し、未指定時は要求の白画像を使用します(view: 解決する画像ビュー)。
            const auto resolve = [this, &request](
                const LamaPon::GraphicsViewHandle& view)
            {
                return m_backend->TryResolveShaderResource(
                    view ? view : request.fallbackTexture);
            };
            // 粒子の主画像のGPU参照
            const auto texture = resolve(request.texture);
            // 粒子の補助画像のGPU参照
            const auto auxiliaryTexture = resolve(request.auxiliaryTexture);
            // 画像SRVを持つGPUヒープ
            auto* const descriptorHeap =
                m_backend->ShaderResourceDescriptorHeap();
            if (!texture || !auxiliaryTexture || descriptorHeap == nullptr)
            {
                return result;
            }

            // 粒子描画を記録するコマンド列
            auto* const commandList = m_backend->BeginFrameCommands();
            // 転送済み粒子の頂点と索引ビュー
            const auto geometry = UploadParticleGeometry(request);
            // 粒子用のb0射影行列領域
            const auto viewProjection = UploadParticleViewProjection(request);
            // PSのb0へ渡すカスタム値
            const auto parameterUpload = m_backend->AllocateFrameUpload(
                sizeof(parameters),
                D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            std::memcpy(
                parameterUpload.data,
                parameters.data(),
                sizeof(parameters));
            // D3D11互換の無照明b1定数
            const LamaPon::Sprite2DLighting lighting{};
            // PSのb1へ渡す無照明定数
            const auto lightingUpload = m_backend->AllocateFrameUpload(
                sizeof(lighting),
                D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            std::memcpy(lightingUpload.data, &lighting, sizeof(lighting));

            // 描画に設定するSRVヒープ
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
            // 現在の描画先のビューポート
            const auto& viewport = m_backend->ActiveViewport();
            // 現在の描画先のクリップ矩形
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

        // カスタム粒子PSの次回再読込みを予約します(shaderPath: キャッシュ識別パス)。
        void InvalidateCustomPixelShader(
            const std::filesystem::path& shaderPath) noexcept override
        {
            try
            {
                // 無効化する粒子PSの検索結果
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

        // 完全な四角粒子を描き、影パスと空要求は処理済みとして返します(request: 最大4096枚の四角粒子要求)。
        [[nodiscard]] bool DrawParticles(
            const LamaPon::ParticleDrawRequest& request) override
        {
            // 通常描画を始めると出力先が戻るため、粒子は省略して影パスを維持します。
            if (m_backend->IsShadowPassActive())
            {
                return true;
            }
            if (request.vertices.empty())
            {
                return true;
            }
            RequireCompleteParticleQuads(request);
            // 指定画像または未指定時の白画像
            const auto& texture = request.texture
                ? request.texture
                : request.fallbackTexture;
            // 粒子画像のGPU参照
            const auto binding = m_backend->TryResolveShaderResource(texture);
            // 画像SRVを持つGPUヒープ
            auto* descriptorHeap = m_backend->ShaderResourceDescriptorHeap();
            if (!binding || descriptorHeap == nullptr)
            {
                return false;
            }

            // 粒子描画を記録するコマンド列
            auto* commandList = m_backend->BeginFrameCommands();
            // 転送済み粒子の頂点と索引ビュー
            const auto geometry = UploadParticleGeometry(request);
            // 粒子の頂点バッファビュー
            const auto& vertexView = geometry.vertexView;
            // 粒子の索引バッファビュー
            const auto& indexView = geometry.indexView;
            // 四角粒子を構成する索引数
            const auto indexCount = geometry.indexCount;
            // VSのb0射影行列の転送領域
            const auto constantUpload = UploadParticleViewProjection(request);
            // 描画に設定するSRVヒープ
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
            // 現在の描画先のビューポート
            const auto& viewport = m_backend->ActiveViewport();
            // 現在の描画先のクリップ矩形
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

        // 標準照明で形状または深度を描き、無効な要求ではfalseを返します(request: 形状・素材・照明・描画状態)。
        [[nodiscard]] bool DrawPrimitive(
            const LamaPon::PrimitiveDrawRequest& request) override
        {
            // 選択した組込み形状の借用参照
            const Geometry* geometry{};
            switch (request.shape)
            {
            // 選択した組込み形状の借用参照
            case LamaPon::PrimitiveRenderShape::Cube: geometry = &m_cube; break;
            // 選択した組込み形状の借用参照
            case LamaPon::PrimitiveRenderShape::Sphere: geometry = &m_sphere; break;
            // 選択した組込み形状の借用参照
            case LamaPon::PrimitiveRenderShape::Cylinder: geometry = &m_cylinder; break;
            // 選択した組込み形状の借用参照
            case LamaPon::PrimitiveRenderShape::Plane: geometry = &m_plane; break;
            case LamaPon::PrimitiveRenderShape::Procedural: break;
            default: return false;
            }
            // 描画する位置・法線・UVの列
            const auto vertices = geometry != nullptr
                ? std::span<const PrimitiveRenderVertex>(geometry->vertices)
                : request.vertices;
            // 描画する三角形の頂点索引
            const auto indices = geometry != nullptr
                ? std::span<const std::uint32_t>(geometry->indices)
                : request.indices;
            if (vertices.empty() || indices.empty() || indices.size() % 3u != 0u
                || vertices.size() > std::numeric_limits<UINT>::max()
                || indices.size() > std::numeric_limits<UINT>::max())
            {
                return false;
            }
            // インスタンス列を使用する指定
            const bool instanced = !request.instances.empty();
            if (instanced
                && (request.depthOnly
                    || request.instances.size_bytes()
                        > std::numeric_limits<UINT>::max()))
            {
                return false;
            }
            // 頂点列の範囲外を指す索引を検出します(index: 三角形の頂点索引)。
            if (std::ranges::any_of(indices, [vertices](const std::uint32_t index)
                { return index >= vertices.size(); }))
            {
                return false;
            }
            // 基本色から発光までの素材6画像
            const std::array textures{
                request.albedo,
                request.normalTexture,
                request.roughnessTexture,
                request.metallicTexture,
                request.occlusionTexture,
                request.emissiveTexture
            };
            // t0～t5の素材画像のGPU参照
            std::array<LamaPon::D3D12Backend::ShaderResourceBinding, 6>
                bindings{};
            // 平行光の影配列のGPU参照
            LamaPon::D3D12Backend::ShaderResourceBinding shadowBinding{};
            // スポット影画像のGPU参照
            LamaPon::D3D12Backend::ShaderResourceBinding
                spotShadowBinding{};
            // 点光源の影キューブのGPU参照
            LamaPon::D3D12Backend::ShaderResourceBinding
                pointShadowBinding{};
            // SSAO画像のGPU参照
            LamaPon::D3D12Backend::ShaderResourceBinding
                screenAmbientOcclusionBinding{};
            // SSR履歴色のGPU参照
            LamaPon::D3D12Backend::ShaderResourceBinding
                screenReflectionColorBinding{};
            // SSR深度ピラミッドのGPU参照
            LamaPon::D3D12Backend::ShaderResourceBinding
                screenReflectionDepthBinding{};
            // 平行光の影を使用できる状態
            bool directionalShadowActive{};
            // SSAOを使用できる状態
            bool screenAmbientOcclusionActive{};
            // SSRを使用できる状態
            bool screenReflectionActive{};
            // スポット影が現世代で解決済み
            bool spotShadowTextureCurrent{};
            // 点光源の影が現世代で解決済み
            bool pointShadowTextureCurrent{};
            // 環境光キューブを使用する状態
            bool environmentActive{};
            // 畳込み済みIBLを使用する状態
            bool prefilteredEnvironmentActive{};
            // 反射または元環境画像のGPU枠
            D3D12_GPU_DESCRIPTOR_HANDLE environmentDescriptor{};
            // 放射照度画像のGPU枠
            D3D12_GPU_DESCRIPTOR_HANDLE irradianceDescriptor{};
            // Forward+参照が全て有効な状態
            bool clusteredActive{};
            // クラスタライト一覧のGPU枠
            D3D12_GPU_DESCRIPTOR_HANDLE clusterLightsDescriptor{};
            // クラスタライト索引のGPU枠
            D3D12_GPU_DESCRIPTOR_HANDLE clusterIndicesDescriptor{};
            // クラスタライト数のGPU枠
            D3D12_GPU_DESCRIPTOR_HANDLE clusterCountsDescriptor{};
            // RGBのGI体積を使用できる状態
            bool bakedGiActive{};
            // 赤成分GI係数画像のGPU枠
            D3D12_GPU_DESCRIPTOR_HANDLE bakedGiRedDescriptor{};
            // 緑成分GI係数画像のGPU枠
            D3D12_GPU_DESCRIPTOR_HANDLE bakedGiGreenDescriptor{};
            // 青成分GI係数画像のGPU枠
            D3D12_GPU_DESCRIPTOR_HANDLE bakedGiBlueDescriptor{};
            // 解決した反射プローブのGPU参照
            LamaPon::Detail::D3D12ReflectionProbeBindings probe;
            // 第2反射キューブのGPU枠
            D3D12_GPU_DESCRIPTOR_HANDLE secondaryEnvironmentDescriptor{};
            // 第2放射照度キューブのGPU枠
            D3D12_GPU_DESCRIPTOR_HANDLE secondaryIrradianceDescriptor{};
            // 画像SRVを持つGPUヒープ
            ID3D12DescriptorHeap* descriptorHeap{};
            if (!request.depthOnly)
            {
                // 素材参照またはライトの番号
                for (std::size_t index{}; index < textures.size(); ++index)
                {
                    // 指定素材画像または白い代替画像
                    const auto& texture = textures[index]
                        ? textures[index]
                        : request.fallbackTexture;
                    // 素材画像のGPU参照
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
                // 平行光の影画像の解決結果
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
                    // 平行光の影の白い代替参照
                    const auto fallbackShadow =
                        m_backend->TryResolveShaderResource(
                            request.fallbackTexture);
                    if (!fallbackShadow)
                    {
                        return false;
                    }
                    shadowBinding = *fallbackShadow;
                }
                // スポット影画像の解決結果
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
                    // スポット影の白い代替参照
                    const auto fallbackSpotShadow =
                        m_backend->TryResolveShaderResource(
                            request.fallbackTexture);
                    if (!fallbackSpotShadow)
                    {
                        return false;
                    }
                    spotShadowBinding = *fallbackSpotShadow;
                }
                // 点光源の影画像の解決結果
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
                    // 点光源の影の白い代替参照
                    const auto fallbackPointShadow =
                        m_backend->TryResolveShaderResource(
                            request.fallbackTexture);
                    if (!fallbackPointShadow)
                    {
                        return false;
                    }
                    pointShadowBinding = *fallbackPointShadow;
                }
                // SSAO画像の解決結果
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
                    // SSAOの白い代替参照
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
                // SSR履歴色の解決結果
                const auto resolvedReflectionColor =
                    m_backend->TryResolveShaderResource(
                        request.screenSpaceReflection.texture);
                // SSR深度画像の解決結果
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
                    // SSRを使わない白い代替参照
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
                // 元環境キューブの解決結果
                const auto resolvedEnvironment =
                    m_backend->TryResolveShaderResource(
                        request.environment.texture);
                // 畳込み済み反射画像の解決結果
                const auto resolvedSpecular =
                    m_backend->TryResolveShaderResource(
                        request.environment.specular);
                // 放射照度画像の解決結果
                const auto resolvedIrradiance =
                    m_backend->TryResolveShaderResource(
                        request.environment.irradiance);
                // 解決済みのキューブSRVか判定します(binding: 画像参照の解決結果)。
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
                // 未使用キューブ枠のnull参照
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
                // D3D11のTrySetLitEffectReflectionProbeと同じく、範囲に入ったプローブを解決できたときだけSkyのIBLを差し替えます。
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
                // 解決済みの構造化バッファSRVか判定します(binding: バッファ参照の解決結果)。
                const auto isBuffer = [](const auto& binding)
                {
                    return binding.has_value()
                        && binding->dimension == D3D12_SRV_DIMENSION_BUFFER;
                };
                // クラスタライト一覧の解決結果
                const auto resolvedClusterLights =
                    m_backend->TryResolveShaderResource(
                        request.clustered.lights);
                // クラスタライト索引の解決結果
                const auto resolvedClusterIndices =
                    m_backend->TryResolveShaderResource(
                        request.clustered.lightIndices);
                // クラスタライト数の解決結果
                const auto resolvedClusterCounts =
                    m_backend->TryResolveShaderResource(
                        request.clustered.clusterCounts);
                clusteredActive = request.clustered.enabled
                    && isBuffer(resolvedClusterLights)
                    && isBuffer(resolvedClusterIndices)
                    && isBuffer(resolvedClusterCounts);
                // 未使用バッファ枠のnull参照
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
                // 解決済みの体積画像SRVか判定します(binding: 画像参照の解決結果)。
                const auto isVolume = [](const auto& binding)
                {
                    return binding.has_value()
                        && binding->dimension
                            == D3D12_SRV_DIMENSION_TEXTURE3D;
                };
                // 要求されたベイクGIの設定
                const auto& bakedGi = request.bakedGlobalIllumination;
                // 赤成分GI係数画像の解決結果
                const auto resolvedBakedGiRed =
                    m_backend->TryResolveShaderResource(
                        bakedGi.redCoefficients);
                // 緑成分GI係数画像の解決結果
                const auto resolvedBakedGiGreen =
                    m_backend->TryResolveShaderResource(
                        bakedGi.greenCoefficients);
                // 青成分GI係数画像の解決結果
                const auto resolvedBakedGiBlue =
                    m_backend->TryResolveShaderResource(
                        bakedGi.blueCoefficients);
                bakedGiActive = bakedGi.enabled
                    && isVolume(resolvedBakedGiRed)
                    && isVolume(resolvedBakedGiGreen)
                    && isVolume(resolvedBakedGiBlue);
                // 未使用体積画像枠のnull参照
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

            // 描画先を維持するGPUコマンド列
            auto* commandList = request.depthOnly
                ? m_backend->CurrentFrameCommands()
                : m_backend->BeginFrameCommands();
            // 頂点列の転送バイト数
            const auto vertexBytes = static_cast<std::uint64_t>(vertices.size_bytes());
            // 索引列の転送バイト数
            const auto indexBytes = static_cast<std::uint64_t>(indices.size_bytes());
            if (vertexBytes > std::numeric_limits<UINT>::max()
                || indexBytes > std::numeric_limits<UINT>::max())
            {
                throw std::length_error("The DirectX 12 primitive is too large.");
            }
            // フレームが保持する頂点転送領域
            const auto vertexUpload = m_backend->AllocateFrameUpload(
                vertexBytes, alignof(PrimitiveRenderVertex));
            // フレームが保持する索引転送領域
            const auto indexUpload = m_backend->AllocateFrameUpload(
                indexBytes, alignof(std::uint32_t));
            std::memcpy(vertexUpload.data, vertices.data(), vertices.size_bytes());
            std::memcpy(indexUpload.data, indices.data(), indices.size_bytes());

            // constants: HLSLのPrimitiveConstantsと同じ配置のb0定数。
            struct Constants final
            {
                // ワールド変換行列
                DirectX::XMFLOAT4X4 world;
                // マテリアルのRGBA色
                DirectX::XMFLOAT4 baseColor;
                // カメラのワールド位置
                DirectX::XMFLOAT4 cameraPosition;
                // 粗さ・金属度・AO強度・法線有無
                DirectX::XMFLOAT4 materialProperties;
                // 発光RGBと法線強度
                DirectX::XMFLOAT4 emissiveFactor;
                // 環境光RGBと強度
                DirectX::XMFLOAT4 ambientColorIntensity;
                // 平行光の方向XYZと強度
                std::array<DirectX::XMFLOAT4, 4>
                    directionalDirectionIntensity{};
                // 平行光RGBと太陽の角半径
                std::array<DirectX::XMFLOAT4, 4> directionalColors{};
                // 平行・点・スポット・影の数
                std::array<std::uint32_t, 4> lightCounts{};
                // 点光源の位置XYZと範囲
                std::array<DirectX::XMFLOAT4, 16> pointPositionRange{};
                // 点光源のRGBと強度
                std::array<DirectX::XMFLOAT4, 16> pointColorIntensity{};
                // スポット光の位置XYZと範囲
                std::array<DirectX::XMFLOAT4, 8> spotPositionRange{};
                // スポット方向と内側角のcos
                std::array<DirectX::XMFLOAT4, 8> spotDirectionInnerCosine{};
                // スポット光のRGBと強度
                std::array<DirectX::XMFLOAT4, 8> spotColorIntensity{};
                // 外側角のcosと影枠番号
                std::array<DirectX::XMFLOAT4, 8> spotOuterCosine{};
                // カスケード別の影描画行列
                std::array<DirectX::XMFLOAT4X4, 4>
                    shadowViewProjections{};
                // カスケード別の終端距離
                DirectX::XMFLOAT4 shadowCascadeSplits{};
                // 平行光番号・バイアスと強度
                DirectX::XMFLOAT4 shadowParameters{};
                // カメラ前方XYZと影画素幅
                DirectX::XMFLOAT4 cameraForwardShadowTexel{};
                // スポット影の描画行列
                std::array<DirectX::XMFLOAT4X4, 4>
                    spotShadowViewProjections{};
                // 影のバイアス・強度・有無
                std::array<DirectX::XMFLOAT4, 4>
                    spotShadowParameters{};
                // 点光源番号・バイアス・強度
                DirectX::XMFLOAT4 pointShadowParameters{};
                // スポット影と点影の画素幅
                DirectX::XMFLOAT4 localShadowTexelSizes{};
                // 画面寸法の逆数とSSAO有無
                DirectX::XMFLOAT4 screenAmbientOcclusionParameters{};
                // ビューと射影の合成行列
                DirectX::XMFLOAT4X4 viewProjection{};
                // SSR強度・有無・距離・歩数
                DirectX::XMFLOAT4 screenReflectionParameters{};
                // 画面寸法の逆数
                DirectX::XMFLOAT4 screenReflectionScreen{};
                // SSR厚み・粗さ上限・最大段
                DirectX::XMFLOAT4 screenReflectionQuality{};
                // SSR履歴を描いた合成行列
                DirectX::XMFLOAT4X4 screenReflectionPreviousViewProjection{};
                // IBL強度・有無・最大ミップ
                DirectX::XMFLOAT4 environmentParameters{};
                // 霧RGBと互換霧方式の指定
                DirectX::XMFLOAT4 fogColorModel{};
                // 霧の開始・終端・密度・有無
                DirectX::XMFLOAT4 fogParameters{};
                // 法線用の逆転置行列
                DirectX::XMFLOAT4X4 worldInverseTranspose{};
                // クラスタXYZの分割数と有無
                DirectX::XMFLOAT4 clusteredParameters{};
                // 近遠距離・対数比・ライト上限
                DirectX::XMFLOAT4 clusteredDepthParameters{};
                // 画面寸法の逆数とライト数
                DirectX::XMFLOAT4 clusteredScreenParameters{};
                // GI領域の最小座標と有無
                DirectX::XMFLOAT4 bakedGiVolumeMinimum{};
                // GI領域寸法の逆数と強度
                DirectX::XMFLOAT4 bakedGiInverseSize{};
                // GI格子のXYZ解像度
                DirectX::XMFLOAT4 bakedGiResolution{};
                // 反射補正箱の中心
                DirectX::XMFLOAT4 reflectionBoxCenter{};
                // 反射補正箱の半寸法と有無
                DirectX::XMFLOAT4 reflectionBoxParameters{};
                // 第2反射補正箱の中心
                DirectX::XMFLOAT4 reflectionSecondaryBoxCenter{};
                // 第2反射補正箱の半寸法と有無
                DirectX::XMFLOAT4 reflectionSecondaryBoxParameters{};
                // 第2プローブの混合率と最終段
                DirectX::XMFLOAT4 reflectionBlendParameters{};
            } constants{};
            // HLSLのPrimitiveConstantsと同じ並び・大きさであることを保証します。
            static_assert(sizeof(constants) == 2448u);
            // 要求されたビュー行列
            const auto view = DirectX::XMLoadFloat4x4(&request.view);
            // 要求された射影行列
            const auto projection = DirectX::XMLoadFloat4x4(&request.projection);
            constants.world = request.world;
            // ワールド逆行列計算の行列式
            DirectX::XMVECTOR worldDeterminant{};
            DirectX::XMStoreFloat4x4(
                &constants.worldInverseTranspose,
                DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(
                    &worldDeterminant,
                    DirectX::XMLoadFloat4x4(&request.world))));
            constants.baseColor = request.baseColor;
            // カメラ位置と前方を得る逆ビュー
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
            // 素材参照またはライトの番号
            for (std::size_t index{};
                index < constants.lightCounts[0];
                ++index)
            {
                // 定数へ写す対象ライト
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
            // 素材参照またはライトの番号
            for (std::size_t index{};
                index < constants.lightCounts[1];
                ++index)
            {
                // 定数へ写す対象ライト
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
            // 素材参照またはライトの番号
            for (std::size_t index{};
                index < constants.lightCounts[2];
                ++index)
            {
                // 定数へ写す対象ライト
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
            // 平行光の影の設定
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
            // スポット影配列の面番号
            for (std::size_t slot{};
                slot < request.spotShadows.size();
                ++slot)
            {
                // 対象面のスポット影の設定
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
            // 点光源の影の設定
            const auto& pointShadow = request.pointShadow;
            // 点光源の影が有効な状態
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
            // SSRの射影と品質設定
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
            constants.environmentParameters = {
                environmentActive
                    ? std::max(request.environment.intensity, 0.0f)
                    : 0.0f,
                environmentActive ? 1.0f : 0.0f,
                prefilteredEnvironmentActive
                    ? request.environment.specularMaximumMip
                    : 0.0f,
                0.0f };
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
            // リフレクションプローブは、LitEffect::SetEnvironmentOverrideD3D11と同じ値でSkyのIBLを差し替えます。
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
            // Forward+の距離と画面設定
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
            // 要求されたベイクGIの設定
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
            // b0定数の256バイト境界領域
            const auto constantUpload = m_backend->AllocateFrameUpload(
                sizeof(constants),
                D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            std::memcpy(
                constantUpload.data,
                &constants,
                sizeof(constants));

            // 形状の頂点バッファビュー
            const D3D12_VERTEX_BUFFER_VIEW vertexView{
                vertexUpload.gpuAddress,
                static_cast<UINT>(vertexBytes),
                static_cast<UINT>(sizeof(PrimitiveRenderVertex)) };
            // 形状の索引バッファビュー
            const D3D12_INDEX_BUFFER_VIEW indexView{
                indexUpload.gpuAddress,
                static_cast<UINT>(indexBytes),
                DXGI_FORMAT_R32_UINT };
            // インスタンスバッファビュー
            D3D12_VERTEX_BUFFER_VIEW instanceView{};
            if (instanced)
            {
                // フレームが保持する行列と色の列
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
            // 描画条件から選んだPSO
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
                // 描画に設定するSRVヒープ
                ID3D12DescriptorHeap* heaps[]{ descriptorHeap };
                commandList->SetDescriptorHeaps(1, heaps);
                // 素材参照またはライトの番号
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
                // 形状とインスタンスの頂点ビュー
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
                // 現在の描画先のビューポート
                const auto& viewport = m_backend->ActiveViewport();
                // 現在の描画先のクリップ矩形
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
        // 組込みまたは手続き形状を解決して三角形索引を検査します(request: 形状の選択と入力列, vertices: 借用頂点列の出力先, indices: 借用索引列の出力先)。
        [[nodiscard]] bool ResolveGeometry(
            const LamaPon::PrimitiveDrawRequest& request,
            std::span<const PrimitiveRenderVertex>& vertices,
            std::span<const std::uint32_t>& indices) const
        {
            // 選択した組込み形状の借用参照
            const Geometry* geometry{};
            switch (request.shape)
            {
            // 選択した組込み形状の借用参照
            case LamaPon::PrimitiveRenderShape::Cube: geometry = &m_cube; break;
            // 選択した組込み形状の借用参照
            case LamaPon::PrimitiveRenderShape::Sphere: geometry = &m_sphere; break;
            // 選択した組込み形状の借用参照
            case LamaPon::PrimitiveRenderShape::Cylinder: geometry = &m_cylinder; break;
            // 選択した組込み形状の借用参照
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
            // 頂点・三角形数と範囲外索引を検査します(index: 三角形の頂点索引)。
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

        // 粒子PSの更新監視とPSOを同じキャッシュ項目に保持します。
        struct CustomParticleShaderEntry final
        {
            // 使用するカスタム粒子PS
            Microsoft::WRL::ComPtr<ID3DBlob> pixelShader;
            // 色2形式×合成2種のPSO
            std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4>
                pipelineStates;
            // 成功コンパイル時の世代番号
            std::uint64_t generation{};
            // 最後の読込み・コンパイル診断
            std::string error;
            // 次に変更を調べる時刻
            std::chrono::steady_clock::time_point nextCheck{};
            // 最後に観測したソース更新時刻
            std::filesystem::file_time_type writeTime{};
            // 初回の観測を完了した状態
            bool observed{};
            // 次回に再コンパイルする指定
            bool forceReload{};
            // 最後に観測したソースの有無
            bool sourceExists{};
        };

        struct ParticleGeometry final
        {
            // 転送済み粒子の頂点ビュー
            D3D12_VERTEX_BUFFER_VIEW vertexView{};
            // 転送済み粒子の索引ビュー
            D3D12_INDEX_BUFFER_VIEW indexView{};
            // 四角粒子を構成する索引数
            UINT indexCount{};
        };

        // 粒子が4頂点単位かつ4096枚以内でなければ例外を投げます(request: 検査する粒子描画要求)。
        static void RequireCompleteParticleQuads(
            const LamaPon::ParticleDrawRequest& request)
        {
            // 受理する四角粒子の最大枚数
            constexpr std::size_t maximumParticleCount = 4096u;
            if (request.vertices.size() % 4u != 0u
                || request.vertices.size() > maximumParticleCount * 4u)
            {
                throw std::invalid_argument(
                    "Particle draw requests require complete quads within "
                    "the service capacity.");
            }
        }

        // 検査済みの四角粒子をフレーム資源へ転送します(request: 完全な四角粒子の描画要求)。
        [[nodiscard]] ParticleGeometry UploadParticleGeometry(
            const LamaPon::ParticleDrawRequest& request)
        {
            // 転送する四角粒子の枚数
            const auto quadCount = request.vertices.size() / 4u;
            // 四角粒子を構成する索引数
            const auto indexCount = quadCount * 6u;
            // 粒子頂点列の転送バイト数
            const auto vertexBytes = static_cast<std::uint64_t>(
                request.vertices.size_bytes());
            // 粒子索引列の転送バイト数
            const auto indexBytes = static_cast<std::uint64_t>(
                indexCount * sizeof(std::uint32_t));
            // フレームが保持する粒子頂点領域
            const auto vertexUpload = m_backend->AllocateFrameUpload(
                vertexBytes,
                alignof(LamaPon::ParticleRenderVertex));
            // フレームが保持する粒子索引領域
            const auto indexUpload = m_backend->AllocateFrameUpload(
                indexBytes,
                alignof(std::uint32_t));
            std::memcpy(
                vertexUpload.data,
                request.vertices.data(),
                request.vertices.size_bytes());
            // 転送領域へ書き込む索引列
            auto* indices = reinterpret_cast<std::uint32_t*>(indexUpload.data);
            // 索引を生成する四角粒子番号
            for (std::size_t quad{}; quad < quadCount; ++quad)
            {
                // 四角粒子の先頭頂点番号
                const auto first = static_cast<std::uint32_t>(quad * 4u);
                // 四角粒子の索引列の開始位置
                const auto offset = quad * 6u;
                indices[offset] = first;
                indices[offset + 1u] = first + 1u;
                indices[offset + 2u] = first + 2u;
                indices[offset + 3u] = first;
                indices[offset + 4u] = first + 2u;
                indices[offset + 5u] = first + 3u;
            }
            // 転送済み粒子の描画ビュー
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

        // 粒子の合成射影行列を256バイト境界の定数領域へ転送します(request: ビューと射影を持つ描画要求)。
        [[nodiscard]] LamaPon::D3D12Backend::FrameUploadAllocation
            UploadParticleViewProjection(
                const LamaPon::ParticleDrawRequest& request)
        {
            // 粒子のビュー・射影合成行列
            DirectX::XMFLOAT4X4 viewProjection{};
            DirectX::XMStoreFloat4x4(
                &viewProjection,
                DirectX::XMMatrixMultiply(
                    DirectX::XMLoadFloat4x4(&request.view),
                    DirectX::XMLoadFloat4x4(&request.projection)));
            // b0定数の256バイト境界領域
            const auto upload = m_backend->AllocateFrameUpload(
                sizeof(viewProjection),
                D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            std::memcpy(upload.data, &viewProjection, sizeof(viewProjection));
            return upload;
        }

        // 粒子PSを250ms間隔で再確認し、変更時は古いPSOを破棄します(assets: 資産の読込元, source: PSのパス・キー・診断処理)。
        [[nodiscard]] CustomParticleShaderEntry& PrepareCustomParticleShader(
            LamaPon::AssetManager& assets,
            const LamaPon::Detail::MaterialShaderSource& source)
        {
            // 対象粒子PSのキャッシュ項目
            auto& entry = m_customParticleShaders[source.cacheKey];
            // 変更確認を始める現在時刻
            const auto now = std::chrono::steady_clock::now();
            if (entry.observed && !entry.forceReload && now < entry.nextCheck)
            {
                return entry;
            }
            entry.nextCheck = now + std::chrono::milliseconds(250);
            // 資産がアーカイブ内にある状態
            const bool archived = assets.IsArchived();
            // 更新時刻取得のエラー
            std::error_code fileError;
            // PSソースが存在する状態
            const bool sourceExists = assets.FileExists(source.path);
            // 今回観測したPS更新時刻
            const auto writeTime = (sourceExists && !archived)
                ? std::filesystem::last_write_time(source.path, fileError)
                : std::filesystem::file_time_type{};
            // PSを再コンパイルする条件
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
            // 変更されたPSに依存するPSO
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
            // exception: PSの読込み・コンパイル失敗。
            catch (const std::exception& exception)
            {
                entry.error = source.describeFailure
                    ? source.describeFailure(exception.what())
                    : std::string(exception.what());
                entry.pixelShader.Reset();
            }
            return entry;
        }

        // 描画先の色形式と合成指定でカスタム粒子PSOを取得します(entry: PSと4種のPSOを持つ項目, additive: 加算合成の指定)。
        [[nodiscard]] ID3D12PipelineState* CustomParticlePipelineState(
            CustomParticleShaderEntry& entry,
            const bool additive)
        {
            // 現在の描画先の色形式
            const auto colorFormat = m_backend->ActiveColorFormat();
            // 描画先形式のPSO配列番号
            const std::size_t formatIndex = colorFormat
                    == LamaPon::D3D12Backend::PrimaryColorFormat
                ? 0u
                : colorFormat == DXGI_FORMAT_R16G16B16A16_FLOAT
                    ? 1u
                    : throw std::invalid_argument(
                        "The active DirectX 12 particle target format is "
                        "unsupported.");
            // 対象描画条件のPSOキャッシュ
            auto& pipeline = entry.pipelineStates[
                formatIndex * 2u + (additive ? 1u : 0u)];
            if (pipeline != nullptr)
            {
                return pipeline.Get();
            }
            // 粒子の位置・色・UVの入力配置
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
            // GPUへ渡すPSO作成情報
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

        // 描画先の色形式と合成指定で標準粒子PSOを取得します(additive: 加算合成の指定)。
        [[nodiscard]] ID3D12PipelineState* ParticlePipelineState(
            bool additive)
        {
            // 現在の描画先の色形式
            const auto colorFormat = m_backend->ActiveColorFormat();
            // 描画先形式のPSO配列番号
            const std::size_t formatIndex = colorFormat
                    == LamaPon::D3D12Backend::PrimaryColorFormat
                ? 0u
                : colorFormat == DXGI_FORMAT_R16G16B16A16_FLOAT
                    ? 1u
                    : throw std::invalid_argument(
                        "The active DirectX 12 particle target format is "
                        "unsupported.");
            // 対象描画条件のPSOキャッシュ
            auto& pipeline = m_particlePipelineStates[
                formatIndex * 2u + (additive ? 1u : 0u)];
            if (pipeline != nullptr)
            {
                return pipeline.Get();
            }
            // 粒子の位置・色・UVの入力配置
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
            // GPUへ渡すPSO作成情報
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

        // ワイヤー時は両面描画の共通番号へ正規化します(wireframe: 辺だけの描画指定, cull: 除去する面)。
        [[nodiscard]] static std::size_t CullIndex(
            const bool wireframe,
            const LamaPon::ShaderCullMode cull) noexcept
        {
            return wireframe
                ? static_cast<std::size_t>(LamaPon::ShaderCullMode::None)
                : static_cast<std::size_t>(cull);
        }

        // 色形式・面除去・頂点形式・合成条件で標準描画PSOを取得します(alphaBlend: 透過合成の指定, depthTest: 深度比較の指定, depthWrite: 深度書込みの指定, wireframe: 辺だけの描画指定, instanced: インスタンス頂点の指定, cull: 除去する面)。
        [[nodiscard]] ID3D12PipelineState* PipelineState(
            bool alphaBlend,
            bool depthTest,
            bool depthWrite,
            bool wireframe,
            bool instanced,
            LamaPon::ShaderCullMode cull)
        {
            // 現在の描画先の色形式
            const auto colorFormat = m_backend->ActiveColorFormat();
            // 描画先形式のPSO配列番号
            const std::size_t formatIndex = colorFormat
                    == LamaPon::D3D12Backend::PrimaryColorFormat
                ? 0u
                : colorFormat == DXGI_FORMAT_R16G16B16A16_FLOAT
                    ? 1u
                    : throw std::invalid_argument(
                        "The active DirectX 12 primitive target format is "
                        "unsupported.");
            // 標準描画PSO配列の参照番号
            const std::size_t index = formatIndex * 36u
                + CullIndex(wireframe, cull) * 12u
                + (instanced ? 6u : 0u)
                + (wireframe ? 3u : 0u)
                + (!depthTest ? 2u : (alphaBlend ? 1u : 0u));
            // 対象描画条件のPSOキャッシュ
            auto& pipeline = m_pipelineStates[index];
            // 深度書込みとDSV形式はキーに含まれず、同じキーの最初の設定が再利用されます。
            if (pipeline != nullptr)
            {
                return pipeline.Get();
            }
            // 形状の位置・法線・UVの入力配置
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
            // GPUへ渡すPSO作成情報
            D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
            description.pRootSignature = m_rootSignature.Get();
            // slot1の80バイト行列・色の入力配置
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
            // 頂点形式に対応する標準VS
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

        // 深度形式・面除去・ワイヤー指定でPSなしの深度PSOを取得します(wireframe: 辺だけの描画指定, cull: 除去する面)。
        [[nodiscard]] ID3D12PipelineState* DepthOnlyPipelineState(
            bool wireframe,
            LamaPon::ShaderCullMode cull)
        {
            // 現在の描画先の深度形式
            const auto depthFormat = m_backend->ActiveDepthFormat();
            // 描画先形式のPSO配列番号
            const std::size_t formatIndex = depthFormat
                    == LamaPon::D3D12Backend::PrimaryDepthFormat
                ? 0u
                : depthFormat == LamaPon::D3D12Backend::ShadowDepthFormat
                    ? 1u
                    : throw std::invalid_argument(
                        "The active DirectX 12 depth target format is "
                        "unsupported.");
            // 対象描画条件のPSOキャッシュ
            auto& pipeline = m_depthOnlyPipelineStates[
                formatIndex * 6u
                + CullIndex(wireframe, cull) * 2u
                + (wireframe ? 1u : 0u)];
            if (pipeline != nullptr)
            {
                return pipeline.Get();
            }
            // 形状の位置・法線・UVの入力配置
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
            // GPUへ渡すPSO作成情報
            D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
            description.pRootSignature = m_rootSignature.Get();
            description.VS = {
                m_vertexShader->GetBufferPointer(),
                m_vertexShader->GetBufferSize() };
            description.BlendState = MakeBlendDescription(false);
            description.SampleMask = std::numeric_limits<UINT>::max();
            // D3D11のShadow mapもrasterizerのbiasを使わず、裏面をカリングしてshader側のbiasだけで自己遮蔽を抑えます。
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

        // サービスより長く生存する描画基盤
        LamaPon::D3D12Backend* m_backend{};
        // b0と22画像を渡す標準ルート署名
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
        // 標準形状の頂点シェーダー
        Microsoft::WRL::ComPtr<ID3DBlob> m_vertexShader;
        // 標準照明の画素シェーダー
        Microsoft::WRL::ComPtr<ID3DBlob> m_pixelShader;
        // インスタンス用の標準VS
        Microsoft::WRL::ComPtr<ID3DBlob> m_instancedVertexShader;
        // 標準粒子の頂点シェーダー
        Microsoft::WRL::ComPtr<ID3DBlob> m_particleVertexShader;
        // 標準粒子の画素シェーダー
        Microsoft::WRL::ComPtr<ID3DBlob> m_particlePixelShader;
        // 色2形式×合成2種の粒子PSO
        std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4>
            m_particlePipelineStates;
        // カスタム粒子PS互換の頂点段
        Microsoft::WRL::ComPtr<ID3DBlob> m_customParticleVertexShader;
        // VSとPSのb0を分離する署名
        Microsoft::WRL::ComPtr<ID3D12RootSignature>
            m_customParticleRootSignature;
        // パス別のカスタム粒子PS
        std::unordered_map<std::filesystem::path, CustomParticleShaderEntry>
            m_customParticleShaders;
        // 次の成功コンパイルの世代番号
        std::uint64_t m_nextCustomParticleGeneration{ 1 };
        // 色・面・頂点・線・合成の72PSO
        std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 72>
            m_pipelineStates;
        // 深度・面・線の12種の深度PSO
        std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 12>
            m_depthOnlyPipelineStates;
        // 単位立方体の頂点と索引
        Geometry m_cube;
        // 単位球の頂点と索引
        Geometry m_sphere;
        // 単位円柱の頂点と索引
        Geometry m_cylinder;
        // 薄い箱型平面の頂点と索引
        Geometry m_plane;
        // 素材シェーダーの描画担当
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
