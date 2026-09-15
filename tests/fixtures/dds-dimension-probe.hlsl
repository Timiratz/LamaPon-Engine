// Material custom shaderの自由枠から、DDSの2D array（t7）、volume（t8）、
// cube array（t9）を読めることを検査するテスト専用Shaderです。
// ゲームでは使いません。

cbuffer ObjectBuffer : register(b0)
{
    row_major float4x4 World;
    row_major float4x4 ViewProjection;
};

Texture2DArray SliceTexture : register(t7);
Texture3D VolumeTexture : register(t8);
TextureCubeArray CubeArrayTexture : register(t9);
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
    float2 TexCoord : TEXCOORD0;
};

PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    output.Position = mul(
        mul(float4(input.Position, 1.0f), World),
        ViewProjection);
    output.TexCoord = input.TexCoord;
    return output;
}

float4 PSMain(PixelInput input) : SV_Target
{
    const float2 uv = input.TexCoord;
    // 左から、2枚目のslice、奥側の層の中心、2個目のcubeの+Y面です。
    const float3 slice =
        SliceTexture.SampleLevel(MaterialSampler, float3(uv, 1.0f), 0.0f).rgb;
    const float3 volume =
        VolumeTexture.SampleLevel(MaterialSampler, float3(uv, 0.75f), 0.0f).rgb;
    const float3 cube = CubeArrayTexture.SampleLevel(
        MaterialSampler,
        float4(0.0f, 1.0f, 0.0f, 1.0f),
        0.0f).rgb;
    const float3 color = uv.x < 1.0f / 3.0f
        ? slice
        : (uv.x < 2.0f / 3.0f ? volume : cube);
    return float4(color, 1.0f);
}
