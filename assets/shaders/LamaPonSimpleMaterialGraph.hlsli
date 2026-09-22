// Simple Shader Graphが生成したHLSLから共有するエンジン側の配線です。
// 利用者がregister番号や定数バッファの並びを管理する必要はありません。

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
    float4 MaterialTextureParameters;
    float4 EmissiveParameters;
    float4 TimeParameters;
};

cbuffer BoneBuffer : register(b2)
{
    float4x3 BoneTransforms[72];
};

Texture2D AlbedoTexture : register(t0);
Texture2D NormalTexture : register(t1);
Texture2D CustomTexture0 : register(t7);
Texture2D CustomTexture1 : register(t8);
Texture2D CustomTexture2 : register(t9);
Texture2D CustomTexture3 : register(t10);
SamplerState MaterialSampler : register(s0);

struct VertexInput
{
    float3 Position : SV_Position;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD0;
};

struct SkinnedVertexInput
{
    float3 Position : SV_Position;
    float3 Normal : NORMAL;
    float4 Tangent : TANGENT;
    float4 Color : COLOR;
    float2 TexCoord : TEXCOORD0;
    uint4 BlendIndices : BLENDINDICES0;
    float4 BlendWeights : BLENDWEIGHT0;
};

struct PixelInput
{
    float4 Position : SV_Position;
    float3 WorldPosition : TEXCOORD0;
    float3 WorldNormal : TEXCOORD1;
    float2 TexCoord : TEXCOORD2;
};

struct SkinnedPixelInput
{
    float2 TexCoord : TEXCOORD0;
    float4 WorldPosition : TEXCOORD1;
    float3 WorldNormal : TEXCOORD2;
    float4 Diffuse : COLOR0;
    float4 Position : SV_Position;
};

PixelInput BuildPixelInput(
    float3 position,
    float3 normal,
    float2 texCoord)
{
    PixelInput output;
    const float4 worldPosition = mul(float4(position, 1.0f), World);
    output.Position = mul(worldPosition, ViewProjection);
    output.WorldPosition = worldPosition.xyz;
    output.WorldNormal = normalize(
        mul(float4(normal, 0.0f), WorldInverseTranspose).xyz);
    output.TexCoord = texCoord;
    return output;
}

PixelInput VSMain(VertexInput input)
{
    return BuildPixelInput(input.Position, input.Normal, input.TexCoord);
}

void SkinVertex(
    SkinnedVertexInput input,
    out float3 position,
    out float3 normal)
{
    float4x3 skinning = 0.0f;
    [unroll]
    for (uint index = 0u; index < 4u; ++index)
    {
        const uint bone = min(input.BlendIndices[index], 71u);
        const float weight = input.BlendWeights[index];
        skinning += BoneTransforms[bone] * weight;
    }
    position = mul(float4(input.Position, 1.0f), skinning);
    normal = normalize(mul(input.Normal, (float3x3)skinning));
}

PixelInput VSSkinnedMain(SkinnedVertexInput input)
{
    float3 position;
    float3 normal;
    SkinVertex(input, position, normal);
    return BuildPixelInput(position, normal, input.TexCoord);
}

PixelInput ToPixelInput(SkinnedPixelInput input)
{
    PixelInput pixel;
    pixel.Position = input.Position;
    pixel.WorldPosition = input.WorldPosition.xyz;
    pixel.WorldNormal = input.WorldNormal;
    pixel.TexCoord = input.TexCoord;
    return pixel;
}
