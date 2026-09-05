// ScreenEffectがシーンの深度（t3）を読めることを検査するための
// テスト専用Shaderです。ゲームでは使いません。
//
// 深度をカメラからの距離へ直し、CustomParameters[0].xで割った値を
// グレースケールで書き出します。テスト側は「手前の物」と「奥の背景」
// の画素を比べ、手前の方が暗い（近い）ことを確かめます。
//
// 深度が刺さっていないと、Shaderは0を読みます。0のときの距離は
// ニアクリップ面そのものになるため、画面が一様に暗くなります。
// 手前と奥で差が出ない場合は、深度入力を確認します。
//
// CustomParameters[0].x = 距離を0〜1へ収めるための除数（メートル）
// CustomParameters[0].y = 0なら距離、1なら生の深度をそのまま出力
//                         （切り分け用。距離が全部上限に張り付いた
//                           ときに、深度が空なのか式が違うのかを
//                           分けるために要ります）、2なら再構成した
//                           ビュー空間の法線をRGBへ（n*0.5+0.5）

// 距離・法線の式は共有実装から取り込みます（テスト専用なので
// assetsの外にあり、相対パスで参照します）。
#include "../../assets/shaders/LamaPonScreenDepth.hlsli"

cbuffer ScreenParameters : register(b0)
{
    float4 CustomParameters[8];
    float4 ScreenSize;
    // 末尾。x=射影の_33、y=射影の_43、z=深度が有効なら1。
    float4 DepthParameters;
    // x=1/射影の_11、y=1/射影の_22（法線の再構成用）。
    float4 DepthUnprojection;
};

Texture2D SceneTexture : register(t0);
Texture2D SceneDepthTexture : register(t3);
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
    // 深度を利用できない場合は青を返します。通常の距離出力では
    // Bを使わないため、テスト側で深度の欠落を判別できます。
    if (DepthParameters.z < 0.5f)
    {
        return float4(0.0f, 0.0f, 1.0f, 1.0f);
    }

    const int2 pixel = int2(input.Position.xy);
    const float deviceDepth =
        SceneDepthTexture.Load(int3(pixel, 0)).r;

    if (CustomParameters[0].y >= 1.5f)
    {
        // 共有実装で法線を組み立てます。カメラを向いている面は-z。
        const float3 normal = LamaPonReconstructViewNormal(
            SceneDepthTexture,
            pixel,
            ScreenSize.zw,
            DepthParameters,
            DepthUnprojection);
        return float4(normal * 0.5f + 0.5f, 1.0f);
    }

    float scaled = 0.0f;
    if (CustomParameters[0].y >= 0.5f)
    {
        scaled = saturate(deviceDepth);
    }
    else
    {
        const float distance = LamaPonSceneDistance(
            deviceDepth,
            DepthParameters);
        scaled = saturate(
            distance / max(CustomParameters[0].x, 0.001f));
    }
    // 8bitのバックバッファへ書くため上位・下位へ分けます。
    const float quantized = scaled * 255.0f;
    const float high = floor(quantized) / 255.0f;
    const float low = frac(quantized);
    return float4(high, low, 0.0f, 1.0f);
}
