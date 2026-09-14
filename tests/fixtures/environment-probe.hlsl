// 3D Materialのcustom shaderが、DirectX 11と同じ環境マップ（t3の事前畳み込み
// 済みスペキュラ、t6の放射照度）とb1のEnvironmentParametersを受け取ることを
// 検査するためのテスト専用Shaderです。ゲームでは使いません。

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
    // x=強さ, y=有効, z=事前畳み込み済みスペキュラの最終ミップ番号
    float4 EnvironmentParameters;
};

TextureCube EnvironmentMap : register(t3);
TextureCube IrradianceMap : register(t6);
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
    const float3 normal = normalize(input.WorldNormal);
    const float3 viewDirection = normalize(
        CameraPosition.xyz - input.WorldPosition);
    // 反射方向の半分の粗さのミップと、法線方向の放射照度を混ぜます。
    const float3 specular = EnvironmentMap.SampleLevel(
        MaterialSampler,
        reflect(-viewDirection, normal),
        EnvironmentParameters.z * 0.5f).rgb;
    const float3 diffuse = IrradianceMap.SampleLevel(
        MaterialSampler,
        normal,
        0.0f).rgb;
    const float3 color =
        (specular * 0.6f + diffuse * 0.4f) * EnvironmentParameters.x
        + Ambient.rgb * 0.1f;
    return float4(color, 1.0f);
}
