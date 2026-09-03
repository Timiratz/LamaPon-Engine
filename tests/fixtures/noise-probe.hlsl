// CPU実装（src/LamaPon/Core/Noise.h）とGPU実装
// （assets/shaders/LamaPonNoise.hlsli）の一致を検査するための
// テスト専用Shaderです。ゲームでは使いません。
//
// 画面を横に並んだ帯として使い、各帯でノイズを1点だけ評価して
// 値をそのまま色（グレースケール）へ書き出します。テスト側は
// 読み取った色とCPUで求めた値を比べます。
//
// CustomParameters[0].xy = 評価する座標
// CustomParameters[0].z  = 種類（0=Value2D 1=Perlin2D 2=fBm
//                                3=Worley2D 4=Value1D 5=Value3D）
// CustomParameters[0].w  = fBmのオクターブ数
// CustomParameters[1].x  = Value3D の z 座標

// このファイルはassetsの外（tests/fixtures）にあるため、共有実装は
// 相対パスで取り込みます（インクルードは取り込む側のフォルダーから
// 解決されます）。テスト専用なのでassetsを汚しません。
#include "../../assets/shaders/LamaPonNoise.hlsli"

cbuffer ScreenParameters : register(b0)
{
    float4 CustomParameters[8];
    float4 ScreenSize;
};

Texture2D SceneTexture : register(t0);
SamplerState SceneSampler : register(s0);

struct VertexOutput
{
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

VertexOutput VSMain(uint vertexId : SV_VertexID)
{
    VertexOutput output;
    const float2 uv = float2(
        (vertexId << 1) & 2,
        vertexId & 2);
    output.Position = float4(
        uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f),
        0.0f,
        1.0f);
    output.TexCoord = uv;
    return output;
}

float4 PSMain(VertexOutput input) : SV_Target
{
    const float2 position = CustomParameters[0].xy;
    const float kind = CustomParameters[0].z;
    const int octaves = (int)max(CustomParameters[0].w, 1.0f);

    float value = 0.0f;
    if (kind < 0.5f)
    {
        value = LamaPonValueNoise2D(position);
    }
    else if (kind < 1.5f)
    {
        value = LamaPonPerlinNoise2D(position);
    }
    else if (kind < 2.5f)
    {
        value = LamaPonFractalNoise2D(
            position, octaves, 2.0f, 0.5f);
    }
    else if (kind < 3.5f)
    {
        value = LamaPonWorleyNoise2D(position);
    }
    else if (kind < 4.5f)
    {
        value = LamaPonValueNoise1D(position.x);
    }
    else
    {
        value = LamaPonValueNoise3D(
            float3(position, CustomParameters[1].x));
    }

    // 8bitのバックバッファへ書くため、値を上位・下位に分けて
    // 精度を確保します。R=上位、G=下位（1/255刻みの残り）。
    const float quantized = saturate(value) * 255.0f;
    const float high = floor(quantized) / 255.0f;
    const float low = frac(quantized);
    return float4(high, low, 0.0f, 1.0f);
}
