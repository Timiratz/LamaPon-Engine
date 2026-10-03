#include "../../assets/shaders/LamaPonNoise.hlsli"

// 画面検査の追加値と画面寸法
cbuffer ScreenParameters : register(b0)
{
    // 座標XY・種別・段数・1.xZ
    float4 CustomParameters[8];
    // 画面幅高さXY・逆幅高さZW
    float4 ScreenSize;
};

// 互換配置のシーン画像
Texture2D SceneTexture : register(t0);
// 互換配置の画像採取設定
SamplerState SceneSampler : register(s0);

struct VertexOutput
{
    // 画面三角形の透視位置
    float4 Position : SV_Position;
    // 画面画像のUV
    float2 TexCoord : TEXCOORD0;
};

// 頂点IDだけで画面を覆う三角形とUVを作る(vertexId: 0～2の頂点番号)。
VertexOutput VSMain(uint vertexId : SV_VertexID)
{
    // 画面全体の三角形頂点
    VertexOutput output;
    // 三角形頂点の画面UV
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

// 種類0～5の指定ノイズを評価して上位R・下位Gで返す(input: 互換用の画面頂点)。
float4 PSMain(VertexOutput input) : SV_Target
{
    // 検査するノイズ座標XY
    const float2 position = CustomParameters[0].xy;
    // 0～5の検査ノイズ種別
    // 種別は0Value2D・1Perlin2D・2fBm・3Worley2D・4Value1D・5Value3D、3DのZは1.xを使う。
    const float kind = CustomParameters[0].z;
    // fBmの1以上の段階数
    const int octaves = (int)max(CustomParameters[0].w, 1.0f);

    // 検査したノイズの出力値
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

    // 255倍して分ける出力値
    // 8bit画像から値を復元できるよう、Rへ上位・Gへ1/255刻みの残りを書く。
    const float quantized = saturate(value) * 255.0f;
    // Rへ書く1/255刻みの上位
    const float high = floor(quantized) / 255.0f;
    // Gへ書く刻み残りの下位
    const float low = frac(quantized);
    return float4(high, low, 0.0f, 1.0f);
}
