// Particleの頂点RGBA・UV・t0・t1・追加値を検査し、補助画像は2倍UVで繰り返し採取する。
// t0のParticle表面画像
Texture2D SpriteTexture : register(t0);
// t1のParticle補助画像
Texture2D AuxiliaryTexture : register(t1);
// Particle画像の採取設定
SamplerState SpriteSampler : register(s0);

// Particle検査の追加定数
cbuffer SpriteParameters : register(b0)
{
    // 0.x混合比・0.y強度・0.z青
    float4 CustomParameters[8];
};

// 補助画像とUV色を設定比で混ぜ表面RGBAへ掛ける(color: 頂点RGBA, uv: Particle画像UV, position: 互換用の画面位置)。
float4 PSMain(
    float4 color : COLOR0,
    float2 uv : TEXCOORD0,
    float4 position : SV_Position) : SV_Target
{
    // 表面画像と頂点RGBAの積
    const float4 particle = SpriteTexture.Sample(SpriteSampler, uv) * color;
    // 2倍UVの補助画像RGB
    const float3 auxiliary =
        AuxiliaryTexture.Sample(SpriteSampler, uv * 2.0f).rgb;
    // UVのRGと指定Bの検査色
    const float3 gradient = float3(uv, CustomParameters[0].z);
    // 混合と強度設定後のRGB
    const float3 mixed = lerp(auxiliary, gradient, CustomParameters[0].x)
        * CustomParameters[0].y;
    return float4(mixed * particle.rgb, particle.a);
}
