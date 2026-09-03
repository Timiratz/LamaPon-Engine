// LamaPon 2D shader error placeholder.
// 自作の2DShader（スプライト／UI／パーティクル）がコンパイルできな
// かったときの代役です。3D側のLamaPonShaderError.hlslと同じ役割で、
// こちらはピクセルシェーダーだけを差し替えます。
// マテリアルへ割り当てて使うものではありません。

Texture2D SpriteTexture : register(t0);
SamplerState SpriteSampler : register(s0);

// 引数の並びはCOLOR0→TEXCOORD0→SV_Position。入力レジスタは宣言順に
// 割り当てられるので、変えると値が静かにずれます。
float4 PSMain(
    float4 color : COLOR0,
    float2 uv : TEXCOORD0,
    float4 position : SV_Position) : SV_Target
{
    // 元の絵の形は残します。長方形で塗り潰すと、どのスプライトが
    // 壊れているのかがかえって分かりにくくなるためです。
    const float alpha =
        SpriteTexture.Sample(SpriteSampler, uv).a * color.a;
    clip(alpha - 0.01f);
    return float4(1.0f, 0.0f, 1.0f, alpha);
}
