// ParticleSystemのcustom pixel shaderへ、頂点色・UV・t0のparticle texture・
// t1の補助texture・CustomParametersが届くことを検査するテスト専用Shaderです。
// ゲームでは使いません。

Texture2D SpriteTexture : register(t0);
Texture2D AuxiliaryTexture : register(t1);
SamplerState SpriteSampler : register(s0);

cbuffer SpriteParameters : register(b0)
{
    float4 CustomParameters[8];
};

float4 PSMain(
    float4 color : COLOR0,
    float2 uv : TEXCOORD0,
    float4 position : SV_Position) : SV_Target
{
    const float4 particle = SpriteTexture.Sample(SpriteSampler, uv) * color;
    // UVを2倍にして、s0が繰り返しで読むことも比べます。
    const float3 auxiliary =
        AuxiliaryTexture.Sample(SpriteSampler, uv * 2.0f).rgb;
    const float3 gradient = float3(uv, CustomParameters[0].z);
    const float3 mixed = lerp(auxiliary, gradient, CustomParameters[0].x)
        * CustomParameters[0].y;
    return float4(mixed * particle.rgb, particle.a);
}
