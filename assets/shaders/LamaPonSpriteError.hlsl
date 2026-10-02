// 2D Shaderのコンパイル失敗時に、輪郭を保ってマゼンタで描く内部用の代替PS。

// スプライトの表面画像
Texture2D SpriteTexture : register(t0);
// 表面画像の採取設定
SamplerState SpriteSampler : register(s0);

// SpriteBatchの入力順をCOLOR0・TEXCOORD0・SV_Positionに保つ。
// 画像のAlphaを保ってエラー色を返す(color: 頂点RGBA, uv: 画像UV, position: 互換入力の画面位置)。
float4 PSMain(
    float4 color : COLOR0,
    float2 uv : TEXCOORD0,
    float4 position : SV_Position) : SV_Target
{

    // 画像と頂点色を掛けたAlpha
    const float alpha =
        SpriteTexture.Sample(SpriteSampler, uv).a * color.a;
    clip(alpha - 0.01f);
    return float4(1.0f, 0.0f, 1.0f, alpha);
}
