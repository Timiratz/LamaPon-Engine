// 有効なLight2Dがあるとき、対象Sprite・Tilemap・UIのPSへ加算照明を適用する。

// スプライトの表面画像
Texture2D SpriteTexture : register(t0);
// 表面画像の採取設定
SamplerState SpriteSampler : register(s0);

// CPUと配置を揃える描画効果
cbuffer SpriteParameters : register(b0)
{
    // b0互換の8本のカスタム値
    float4 CustomParameters[8];
};

struct Sprite2DLight
{

    // XY画面位置・半径・強度
    float4 PositionRadiusIntensity;

    // RGBライト色・W予約
    float4 Color;
};

// b1の並びはSpriteEffect.hのSprite2DLightingと一致させる。
// 画面内の最大16本の光源
cbuffer Sprite2DLightingBuffer : register(b1)
{

    // Xライト数・YZW予約
    uint4 Light2DCounts;
    // 最大16灯の2Dライト配列
    Sprite2DLight Light2DList[16];
};

// 半径内の加算光を二乗減衰で求める(pixelPosition: 画面XY位置, light: 2Dライト情報)。
float3 Light2DContribution(
    const float2 pixelPosition,
    const Sprite2DLight light)
{
    // 零除算を避けたライト半径
    const float radius =
        max(light.PositionRadiusIntensity.z, 0.001f);
    // 画面位置から光源までの距離
    const float distanceToLight = length(
        pixelPosition - light.PositionRadiusIntensity.xy);

    // 半径外で零となる一次減衰
    const float attenuation =
        saturate(1.0f - distanceToLight / radius);
    return light.Color.rgb
        * light.PositionRadiusIntensity.w
        * attenuation
        * attenuation;
}

// SpriteBatchの入力順をCOLOR0・TEXCOORD0・SV_Positionに保つ。
// 元の表面色へ最大16灯の加算照明を掛ける(color: 頂点RGBA, uv: 画像UV, position: 画面位置)。
float4 PSMain(
    float4 color : COLOR0,
    float2 uv : TEXCOORD0,
    float4 position : SV_Position) : SV_Target
{
    // 画像と頂点色を掛けたRGBA
    float4 pixel =
        SpriteTexture.Sample(SpriteSampler, uv) * color;

    // 元色1に加えるRGB照明倍率
    float3 lighting = float3(1.0f, 1.0f, 1.0f);
    // 使用するライト数の上限
    const uint count = min(Light2DCounts.x, 16u);
    // 寄与を加えるライト番号
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
