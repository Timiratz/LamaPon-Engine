// LamaPon built-in 2D sprite lighting shader.
//
// Light2Dが1つでも有効なシーンで、エンジンが自動的に差し替えます。
// 対象はワールド空間のSprite RendererとTilemap、それにUIを照らす
// 設定にしたLight2Dがあるときのみ UI Sprite Renderer も含みます。
// Inspectorから編集する種類のShaderではありません。
//
// 加算式です。届く灯りが1つも無ければ、スプライトは元の色のまま
// 描かれ、暗くなることはありません。
//
// 【入力の並びは変えないこと】
// SpriteBatchの頂点シェーダーは COLOR0 → TEXCOORD0 → SV_Position の
// 順で出力します。ピクセルシェーダーの引数は入力レジスタへ宣言順に
// 割り当てられるため、この順で書かないと1本ずつずれます。
// SV_Positionを先頭に書くと、colorへUVが入り、uvは未定義になります
// （エラーも警告も出ず、値だけが静かに壊れます）。
//
// 灯りの一覧はb1の専用バッファから読みます。b0のCustomParametersは
// 8本しかなく自作Shaderの持ち物なので、そちらは使いません。

Texture2D SpriteTexture : register(t0);
SamplerState SpriteSampler : register(s0);

cbuffer SpriteParameters : register(b0)
{
    float4 CustomParameters[8];
};

struct Sprite2DLight
{
    // xy=画面ピクセル座標, z=届く半径, w=強さ。
    float4 PositionRadiusIntensity;
    // rgb=色, w=予約。
    float4 Color;
};

// SpriteEffect.h の Sprite2DLighting と並びを合わせること。
cbuffer Sprite2DLightingBuffer : register(b1)
{
    // x=灯数, yzw=予約。
    uint4 Light2DCounts;
    Sprite2DLight Light2DList[16];
};

float3 Light2DContribution(
    const float2 pixelPosition,
    const Sprite2DLight light)
{
    const float radius =
        max(light.PositionRadiusIntensity.z, 0.001f);
    const float distanceToLight = length(
        pixelPosition - light.PositionRadiusIntensity.xy);
    // 縁でぴたりと0になり、中心へ向かって2乗で強まる減衰です。
    // 半径の外は完全に0なので、遠くの灯りを足しても絵は変わりません。
    const float attenuation =
        saturate(1.0f - distanceToLight / radius);
    return light.Color.rgb
        * light.PositionRadiusIntensity.w
        * attenuation
        * attenuation;
}

float4 PSMain(
    float4 color : COLOR0,
    float2 uv : TEXCOORD0,
    float4 position : SV_Position) : SV_Target
{
    float4 pixel =
        SpriteTexture.Sample(SpriteSampler, uv) * color;

    float3 lighting = float3(1.0f, 1.0f, 1.0f);
    const uint count = min(Light2DCounts.x, 16u);
    [loop]
    for (uint index = 0u; index < count; ++index)
    {
        lighting += Light2DContribution(
            position.xy,
            Light2DList[index]);
    }

    pixel.rgb *= lighting;
    return pixel;
}
