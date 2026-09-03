// 画面の中央へ明るい四角を1つ描くだけのテスト専用Shaderです。
// ゲームでは使いません。
//
// 画面エフェクトの「差し込み地点」を確かめるために使います。
// Bloomより前へ差し込めばこの四角は滲み、後ろへ差し込めば滲みません。
// 滲みの有無で、指定した位置に本当に入ったかが分かります。
//
// CustomParameters[0].x = 四角の半径（画面比）
// CustomParameters[0].y = 明るさ（HDRなので1より大きくします）

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
    const float radius = max(CustomParameters[0].x, 0.001f);
    const float intensity = max(CustomParameters[0].y, 0.0f);
    const float4 scene =
        SceneTexture.Sample(SceneSampler, input.TexCoord);

    const float2 offset = abs(input.TexCoord - 0.5f);
    if (offset.x < radius && offset.y < radius)
    {
        return float4(intensity, intensity, intensity, 1.0f);
    }
    return scene;
}
