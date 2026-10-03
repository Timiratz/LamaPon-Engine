#include "../../assets/shaders/LamaPonScreenDepth.hlsli"

// 画面検査の追加値と画面寸法
cbuffer ScreenParameters : register(b0)
{
    // 0.x距離除数・0.y出力モード
    float4 CustomParameters[8];
    // 画面幅高さXY・逆幅高さZW
    float4 ScreenSize;
    // 投影33・43・深度有効
    float4 DepthParameters;
    // 投影11・22の逆数XY
    float4 DepthUnprojection;
};

// 互換配置のシーン画像
Texture2D SceneTexture : register(t0);
// t3のシーン深度画像
Texture2D SceneDepthTexture : register(t3);
// シーンと補助画像の採取設定
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

// 設定に応じた深度値を上位R・下位Gまたは法線RGBで返す(input: 画面位置とUV)。
float4 PSMain(VertexOutput input) : SV_Target
{
    // 0.yは0距離・1生深度・2法線を選び、無効な深度は識別用の青で返す。
    if (DepthParameters.z < 0.5f)
    {
        return float4(0.0f, 0.0f, 1.0f, 1.0f);
    }

    // 深度を読む画素XY
    const int2 pixel = int2(input.Position.xy);
    // 0～1のデバイス深度
    const float deviceDepth =
        SceneDepthTexture.Load(int3(pixel, 0)).r;

    if (CustomParameters[0].y >= 1.5f)
    {
        // 4近傍から復元したビュー法線
        const float3 normal = LamaPonReconstructViewNormal(
            SceneDepthTexture,
            pixel,
            ScreenSize.zw,
            DepthParameters,
            DepthUnprojection);
        return float4(normal * 0.5f + 0.5f, 1.0f);
    }

    // 0～1へ制限した出力値
    float scaled = 0.0f;
    if (CustomParameters[0].y >= 0.5f)
    {
        scaled = saturate(deviceDepth);
    }
    else
    {
        // 復元した正の視点距離
        const float distance = LamaPonSceneDistance(
            deviceDepth,
            DepthParameters);
        scaled = saturate(
            distance / max(CustomParameters[0].x, 0.001f));
    }
    // 255倍して分ける出力値
    // 8bit画像から値を復元できるよう、Rへ上位・Gへ1/255刻みの残りを書く。
    const float quantized = scaled * 255.0f;
    // Rへ書く1/255刻みの上位
    const float high = floor(quantized) / 255.0f;
    // Gへ書く刻み残りの下位
    const float low = frac(quantized);
    return float4(high, low, 0.0f, 1.0f);
}
