// ScreenEffectの補助テクスチャ（t1／t2）が決まったregisterへ届くことを
// 検査するためのテスト専用Shaderです。ゲームでは使いません。
//
// 画面の左半分へt1、右半分へt2をUVで引き伸ばして書き出します。
// 補助テクスチャを指定しなかった側は白になります。
// CustomParameters[0].x = 書き出す色へ掛ける倍率

cbuffer ScreenParameters : register(b0)
{
    float4 CustomParameters[8];
    float4 ScreenSize;
};

Texture2D SceneTexture : register(t0);
Texture2D FirstAuxiliaryTexture : register(t1);
Texture2D SecondAuxiliaryTexture : register(t2);
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
    const float scale = CustomParameters[0].x;
    if (input.TexCoord.x < 0.5f)
    {
        const float2 uv = float2(input.TexCoord.x * 2.0f, input.TexCoord.y);
        return float4(
            FirstAuxiliaryTexture.Sample(SceneSampler, uv).rgb * scale,
            1.0f);
    }
    const float2 uv = float2(
        (input.TexCoord.x - 0.5f) * 2.0f,
        input.TexCoord.y);
    return float4(
        SecondAuxiliaryTexture.Sample(SceneSampler, uv).rgb * scale,
        1.0f);
}
