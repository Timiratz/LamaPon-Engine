// LamaPonに組み込まれているスプライトマスク用シェーダーです。
//
// MaskInteractionがNone以外のワールド空間Sprite Renderer（UI Rect
// Transformなし・独自Shaderなし）へ、SpriteMaskが1つでも有効なとき
// エンジンが自動的に差し替えます。Inspectorから編集する種類の
// Shaderではありません。
//
// クリップは境目がくっきりしています（半透明のフェードなし）。
// 画素はマスクの矩形／円の内か外か、そのSpriteのMaskInteractionの
// 設定に照らして、描かれるか捨てられるかのどちらかです。
//
// 【入力の並びは変えないこと】
// SpriteBatchの頂点シェーダーは COLOR0 → TEXCOORD0 → SV_Position の
// 順で出力します。ピクセルシェーダーの引数は入力レジスタへ宣言順に
// 割り当てられるため、この順で書かないと1本ずつずれます。
//
// CustomParameters[0].x = マスクの形（0=矩形, 1=円）
// CustomParameters[0].y = SpriteのMaskInteraction
//                         （0=マスクの内側を表示, 1=外側を表示）
// CustomParameters[1]   = マスクの中心x, y, 半幅または半径, 半高
//                         （円では半高は未使用）

Texture2D SpriteTexture : register(t0);
SamplerState SpriteSampler : register(s0);

cbuffer SpriteParameters : register(b0)
{
    float4 CustomParameters[8];
};

bool InsideMask(const float2 pixelPosition)
{
    const float4 mask = CustomParameters[1];
    const float2 delta = pixelPosition - mask.xy;
    if (CustomParameters[0].x < 0.5f)
    {
        // 矩形マスク。
        return abs(delta.x) <= mask.z && abs(delta.y) <= mask.w;
    }
    // 円マスク（半径はmask.z）。
    return dot(delta, delta) <= mask.z * mask.z;
}

float4 PSMain(
    float4 color : COLOR0,
    float2 uv : TEXCOORD0,
    float4 position : SV_Position) : SV_Target
{
    const bool visibleOutside = CustomParameters[0].y >= 0.5f;
    // マスクの内外判定だけは画面座標で行います（マスクはSprite
    // ごとではなく画面上の場所で決まるため）。
    const bool inside = InsideMask(position.xy);
    clip((inside != visibleOutside) ? 1.0f : -1.0f);

    return SpriteTexture.Sample(SpriteSampler, uv) * color;
}
