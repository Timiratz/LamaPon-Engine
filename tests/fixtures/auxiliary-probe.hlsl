// ScreenEffectのt1・t2の入力を左右へ描き、未設定時の白画像とregister配置を検査する。
// 画面検査の追加値と画面寸法
cbuffer ScreenParameters : register(b0)
{
    // 0.x出力RGBの倍率・他予約
    float4 CustomParameters[8];
    // 画面幅高さXY・逆幅高さZW
    float4 ScreenSize;
};

// 互換配置のシーン画像
Texture2D SceneTexture : register(t0);
// 左半分に描くt1の補助画像
Texture2D FirstAuxiliaryTexture : register(t1);
// 右半分に描くt2の補助画像
Texture2D SecondAuxiliaryTexture : register(t2);
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

// 左へt1・右へt2の画像を設定倍率で描く(input: 画面位置とUV)。
float4 PSMain(VertexOutput input) : SV_Target
{
    // 採取色へ掛ける倍率
    const float scale = CustomParameters[0].x;
    if (input.TexCoord.x < 0.5f)
    {
        // 画像採取UV
        const float2 uv = float2(input.TexCoord.x * 2.0f, input.TexCoord.y);
        return float4(
            FirstAuxiliaryTexture.Sample(SceneSampler, uv).rgb * scale,
            1.0f);
    }
    // 画像採取UV
    const float2 uv = float2(
        (input.TexCoord.x - 0.5f) * 2.0f,
        input.TexCoord.y);
    return float4(
        SecondAuxiliaryTexture.Sample(SceneSampler, uv).rgb * scale,
        1.0f);
}
