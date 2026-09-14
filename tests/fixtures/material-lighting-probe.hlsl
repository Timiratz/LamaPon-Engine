// 3D Materialのcustom shaderが、LitEffectと同じ定数とtexture枠を
// 受け取ることを検査するためのテスト専用Shaderです。ゲームでは使いません。
//
// b1の環境光・方向光・点光源、b3のCustomVectors、t0のアルベド、
// t7のcustom texture、s0（CustomParameters[7].wで点サンプリング）を
// 1つの色へまとめ、MaterialColor.aで半透明に重ねます。

/* LAMAPON_RENDER_STATE
{ "blend": "alpha", "cull": "none" }
*/

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

cbuffer LightingBuffer : register(b1)
{
    float4 Ambient;
    uint4 LightCounts;
    DirectionalLight DirectionalLights[4];
    PointLight PointLights[16];
};

cbuffer CustomVectorBuffer : register(b3)
{
    float4 CustomVectors[64];
};

Texture2D AlbedoTexture : register(t0);
Texture2D MaskTexture : register(t7);
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
    const float4 albedo = AlbedoTexture.Sample(
        MaterialSampler,
        input.TexCoord);
    const float mask = MaskTexture.Sample(
        MaterialSampler,
        input.TexCoord * 3.0f).r;

    float3 light = Ambient.rgb;
    [loop]
    for (uint index = 0u; index < LightCounts.x; ++index)
    {
        const float3 direction =
            -normalize(DirectionalLights[index].DirectionIntensity.xyz);
        light += DirectionalLights[index].Color.rgb
            * DirectionalLights[index].DirectionIntensity.w
            * saturate(dot(normal, direction));
    }
    [loop]
    for (uint pointIndex = 0u; pointIndex < LightCounts.y; ++pointIndex)
    {
        const float3 offset =
            PointLights[pointIndex].PositionRange.xyz - input.WorldPosition;
        const float distance = length(offset);
        const float attenuation = saturate(
            1.0f - distance / max(PointLights[pointIndex].PositionRange.w, 0.001f));
        light += PointLights[pointIndex].ColorIntensity.rgb
            * PointLights[pointIndex].ColorIntensity.w
            * attenuation
            * saturate(dot(normal, offset / max(distance, 0.001f)));
    }

    const float3 color = albedo.rgb
        * MaterialColor.rgb
        * light
        * lerp(0.5f, 1.0f, mask)
        + CustomVectors[0].rgb;
    return float4(color, MaterialColor.a);
}
