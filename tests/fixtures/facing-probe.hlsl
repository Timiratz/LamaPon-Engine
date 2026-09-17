// Material custom shaderが描画状態を宣言しないとき、表面と裏面のどちらが
// 描かれるかを検査するためのテスト専用Shaderです。ゲームでは使いません。
// 表面は法線の向きを色にし、裏面は暗い赤で返します。

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
};

PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    const float4 worldPosition = mul(float4(input.Position, 1.0f), World);
    output.Position = mul(worldPosition, ViewProjection);
    output.WorldPosition = worldPosition.xyz;
    output.WorldNormal =
        mul(float4(input.Normal, 0.0f), WorldInverseTranspose).xyz;
    return output;
}

float4 PSMain(PixelInput input) : SV_Target
{
    const float3 normal = normalize(input.WorldNormal);
    const bool front = dot(
        normal,
        normalize(CameraPosition.xyz - input.WorldPosition)) > 0.0f;
    return front
        ? float4(normal * 0.5f + 0.5f, 1.0f)
        : float4(0.3f, 0.02f, 0.02f, 1.0f);
}
