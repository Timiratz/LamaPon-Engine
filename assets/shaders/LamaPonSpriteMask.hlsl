// マスク内外指定がある標準ワールドSpriteへエンジンが自動適用するクリップシェーダーです。
// ピクセル入力は頂点出力と同じCOLOR0・TEXCOORD0・SV_Positionの順を維持します。

// スプライトの画像
Texture2D SpriteTexture : register(t0);
// スプライト画像の採取設定
SamplerState SpriteSampler : register(s0);

// マスクとスプライト描画の定数
cbuffer SpriteParameters : register(b0)
{
    // 形状・内外指定と中心・半幅
    float4 CustomParameters[8];
};

// 境界を含む矩形または円の内側か返します(pixelPosition: 画面XYピクセル位置)。
bool InsideMask(const float2 pixelPosition)
{
    // 中心XYと半幅・半高
    const float4 mask = CustomParameters[1];
    // マスク中心からのXY変位
    const float2 delta = pixelPosition - mask.xy;
    if (CustomParameters[0].x < 0.5f)
    {

        return abs(delta.x) <= mask.z && abs(delta.y) <= mask.w;
    }

    return dot(delta, delta) <= mask.z * mask.z;
}

// 指定したマスク内外の画素だけを描きます(color: 頂点の乗算RGBA色, uv: 画像UV座標, position: 画面ピクセル位置)。
float4 PSMain(
    float4 color : COLOR0,
    float2 uv : TEXCOORD0,
    float4 position : SV_Position) : SV_Target
{
    // マスク外を表示する指定
    const bool visibleOutside = CustomParameters[0].y >= 0.5f;

    // 画面位置がマスク内の指定
    const bool inside = InsideMask(position.xy);
    clip((inside != visibleOutside) ? 1.0f : -1.0f);

    return SpriteTexture.Sample(SpriteSampler, uv) * color;
}
