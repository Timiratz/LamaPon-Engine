// 3D Materialのcustom shaderが、DirectX 11と同じベイクした間接光（t23〜t25の
// RGB別L1球面調和Texture3D）とb1のBakedGi*定数を受け取ることを検査するための
// テスト専用Shaderです。ゲームでは使いません。

cbuffer ObjectBuffer : register(b0)
{
    row_major float4x4 World;
    row_major float4x4 ViewProjection;
    row_major float4x4 WorldInverseTranspose;
    float4 MaterialColor;
    float4 CameraPosition;
    float4 CameraForward;
    float4 MaterialParameters;
    float4 CustomParameters[8];
};

struct DirectionalLight
{
    float4 DirectionIntensity;
    float4 Color;
};

struct PointLight
{
    float4 PositionRange;
    float4 ColorIntensity;
};

struct SpotLight
{
    float4 PositionRange;
    float4 DirectionInnerCosine;
    float4 ColorIntensity;
    float4 OuterCosinePadding;
};

cbuffer LightingBuffer : register(b1)
{
    float4 Ambient;
    uint4 LightCounts;
    DirectionalLight DirectionalLights[4];
    PointLight PointLights[16];
    SpotLight SpotLights[8];
    row_major float4x4 ShadowViewProjections[4];
    float4 ShadowCascadeSplits;
    float4 ShadowParameters;
    float4 FogColor;
    float4 FogParameters;
    float4 EnvironmentParameters;
    row_major float4x4 SpotShadowViewProjections[4];
    float4 SpotShadowParameters[4];
    float4 PointShadowParameters;
    float4 ShadowTexelSizes;
    float4 ScreenAmbientOcclusionParameters;
    float4 ClusteredParameters;
    float4 ClusteredDepthParameters;
    float4 ClusteredScreenParameters;
    float4 ReflectionBoxCenter;
    float4 ReflectionBoxParameters;
    float4 ReflectionSecondaryBoxCenter;
    float4 ReflectionSecondaryBoxParameters;
    float4 ReflectionBlendParameters;
    float4 ScreenReflectionParameters;
    float4 ScreenReflectionScreen;
    float4 ScreenReflectionQuality;
    row_major float4x4 ScreenReflectionPreviousViewProjection;
    // xyz=ボリュームの最小コーナー, w=有効
    float4 BakedGiVolumeMinimum;
    // xyz=1/大きさ, w=強さ
    float4 BakedGiInverseSize;
    // xyz=各軸のプローブ数
    float4 BakedGiResolution;
};

Texture3D BakedGiRedTexture : register(t23);
Texture3D BakedGiGreenTexture : register(t24);
Texture3D BakedGiBlueTexture : register(t25);
SamplerState MaterialSampler : register(s0);

struct VertexInput
{
    float3 Position : SV_Position;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD0;
};

struct PixelInput
{
    float4 Position : SV_Position;
    float3 WorldPosition : TEXCOORD0;
    float3 WorldNormal : TEXCOORD1;
    float2 TexCoord : TEXCOORD2;
};

PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    const float4 worldPosition = mul(float4(input.Position, 1.0f), World);
    output.Position = mul(worldPosition, ViewProjection);
    output.WorldPosition = worldPosition.xyz;
    output.WorldNormal = normalize(
        mul(float4(input.Normal, 0.0f), WorldInverseTranspose).xyz);
    output.TexCoord = input.TexCoord;
    return output;
}

float4 PSMain(PixelInput input) : SV_Target
{
    if (BakedGiVolumeMinimum.w < 0.5f)
    {
        return float4(Ambient.rgb * 0.5f, 1.0f);
    }
    // 縁のフェードは省き、ボリューム内の係数をそのまま法線で評価します。
    const float3 normal = normalize(input.WorldNormal);
    const float3 volumeUvw = saturate(
        (input.WorldPosition - BakedGiVolumeMinimum.xyz)
        * BakedGiInverseSize.xyz);
    const float3 resolution = BakedGiResolution.xyz;
    const float3 texelUvw =
        (volumeUvw * (resolution - 1.0f) + 0.5f) / resolution;
    const float4 basis = float4(normal, 1.0f);
    const float3 gi = float3(
        dot(basis, BakedGiRedTexture.SampleLevel(
            MaterialSampler, texelUvw, 0.0f)),
        dot(basis, BakedGiGreenTexture.SampleLevel(
            MaterialSampler, texelUvw, 0.0f)),
        dot(basis, BakedGiBlueTexture.SampleLevel(
            MaterialSampler, texelUvw, 0.0f)));
    return float4(max(gi, 0.0f) * BakedGiInverseSize.w * 0.8f, 1.0f);
}
