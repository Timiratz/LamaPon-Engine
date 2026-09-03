// ノイズ関数の見本Shaderです。
//
// LamaPonNoise.hlsli の関数を使って、5種類のノイズを切り替えて
// 表示します。Inspectorで種類・大きさ・オクターブ・流れる速さを
// 触れるので、何がどう効くかを目で確かめられます。
//
// ★このファイルの実装は src/LamaPon/Core/Noise.h（C++）と同じ値を
//   返します。C++で地形を作り、同じノイズで色を乗せられます。

#include "LamaPonNoise.hlsli"

/* LAMAPON_RENDER_STATE
{ "blend": "opaque", "cull": "back", "depthWrite": true }
*/

/* LAMAPON_PROPERTIES
[
  { "target": "0.x", "type": "float",
    "name": "種類（0=Value 1=Perlin 2=fBm 3=Worley 4=Curl）",
    "min": 0.0, "max": 4.0, "default": 2.0 },
  { "target": "0.y", "type": "float", "name": "大きさ（スケール）",
    "min": 0.5, "max": 64.0, "default": 8.0 },
  { "target": "0.z", "type": "float", "name": "重ねる回数（fBmのみ）",
    "min": 1.0, "max": 8.0, "default": 5.0 },
  { "target": "0.w", "type": "float", "name": "流れる速さ",
    "min": 0.0, "max": 2.0, "default": 0.0 },
  { "target": "1.x", "type": "float", "name": "コントラスト",
    "min": 0.1, "max": 4.0, "default": 1.0 },
  { "target": "1.y", "type": "float", "name": "明るさ",
    "min": 0.0, "max": 3.0, "default": 1.0 },
  { "target": "2.rgb", "type": "color", "name": "暗い側の色",
    "default": [0.05, 0.12, 0.25] },
  { "target": "3.rgb", "type": "color", "name": "明るい側の色",
    "default": [0.95, 0.9, 0.7] }
]
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
    float4 MaterialTextureParameters;
    float4 EmissiveParameters;
    // x=経過秒（エンジンが入れます。1時間で巻き戻ります）。
    float4 TimeParameters;
};

cbuffer BoneBuffer : register(b2)
{
    float4x3 BoneTransforms[72];
};

Texture2D AlbedoTexture : register(t0);
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
    const float4 worldPosition =
        mul(float4(position, 1.0f), World);
    output.Position = mul(worldPosition, ViewProjection);
    output.WorldPosition = worldPosition.xyz;
    output.WorldNormal = normalize(
        mul(float4(normal, 0.0f), WorldInverseTranspose).xyz);
    output.TexCoord = texCoord;
    return output;
}

PixelInput VSMain(VertexInput input)
{
    return BuildPixelInput(
        input.Position,
        input.Normal,
        input.TexCoord);
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
    return BuildPixelInput(
        position,
        normal,
        input.TexCoord);
}

float4 ShadeNoise(PixelInput input)
{
    const float kind = CustomParameters[0].x;
    const float scale = max(CustomParameters[0].y, 0.001f);
    const int octaves = (int)max(CustomParameters[0].z, 1.0f);
    const float flowSpeed = CustomParameters[0].w;
    // 時間はエンジンが入れるので、「流れる速さ」を上げるだけで
    // 動きます（以前はスクリプトから秒を渡す必要がありました）。
    const float time = TimeParameters.x;

    // UVを大きさ倍して、時間で流します。
    float2 samplePosition =
        input.TexCoord * scale
        + float2(time * flowSpeed, time * flowSpeed * 0.6f);

    float value = 0.0f;
    if (kind < 0.5f)
    {
        value = LamaPonValueNoise2D(samplePosition);
    }
    else if (kind < 1.5f)
    {
        value = LamaPonPerlinNoise2D(samplePosition);
    }
    else if (kind < 2.5f)
    {
        value = LamaPonFractalNoise2D(
            samplePosition, octaves, 2.0f, 0.5f);
    }
    else if (kind < 3.5f)
    {
        value = LamaPonWorleyNoise2D(samplePosition);
    }
    else
    {
        // 渦の向きを見えるようにするため、ベクトルの長さと角度を
        // 明るさへ落とし込みます。
        const float2 curl =
            LamaPonCurlNoise2D(samplePosition, 0.01f);
        value = saturate(length(curl) * 0.5f);
    }

    // コントラスト（0.5を中心に伸縮）。
    const float contrast = max(CustomParameters[1].x, 0.001f);
    value = saturate((value - 0.5f) * contrast + 0.5f);

    const float3 darkColor = CustomParameters[2].rgb;
    const float3 lightColor = CustomParameters[3].rgb;
    float3 color = lerp(darkColor, lightColor, value);

    // 素朴な陰影だけ足します（ノイズの見え方を邪魔しないように）。
    const float3 lightDirection =
        normalize(float3(0.4f, 0.8f, 0.35f));
    const float diffuse =
        saturate(dot(normalize(input.WorldNormal),
            lightDirection)) * 0.4f + 0.6f;
    color *= diffuse * max(CustomParameters[1].y, 0.0f);

    return float4(color, MaterialColor.a);
}

float4 PSMain(PixelInput input) : SV_Target
{
    return ShadeNoise(input);
}

float4 PSSkinnedMain(SkinnedPixelInput input) : SV_Target
{
    PixelInput pixel;
    pixel.Position = input.Position;
    pixel.WorldPosition = input.WorldPosition.xyz;
    pixel.WorldNormal = input.WorldNormal;
    pixel.TexCoord = input.TexCoord;
    return ShadeNoise(pixel);
}
