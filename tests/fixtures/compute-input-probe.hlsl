// Compute Shaderの入力テクスチャ（t0／t1）とサンプラー（s0）が決まった
// registerへ届くことを検査するためのテスト専用Shaderです。ゲームでは
// 使いません。
//
// 出力の左半分へt0、右半分へt1をUVで引き伸ばして書きます。入力を
// 指定しなかった側は白になります。
//
// CustomParameters[0].x = 書き出す色へ掛ける倍率

cbuffer ComputeParameters : register(b0)
{
    float4 CustomParameters[8];
    // xy=出力の幅と高さ, zw=その逆数。
    float4 OutputSize;
};

Texture2D InputTexture0 : register(t0);
Texture2D InputTexture1 : register(t1);
SamplerState InputSampler : register(s0);
RWTexture2D<float4> OutputTexture : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    // 端数のスレッドグループぶん、はみ出したスレッドが来ます。
    if (id.x >= (uint)OutputSize.x
        || id.y >= (uint)OutputSize.y)
    {
        return;
    }

    const float scale = CustomParameters[0].x;
    // 画素の中心をUVへ直します。
    const float2 uv = (float2(id.xy) + 0.5f) * OutputSize.zw;
    if (uv.x < 0.5f)
    {
        const float2 left = float2(uv.x * 2.0f, uv.y);
        OutputTexture[id.xy] = float4(
            InputTexture0.SampleLevel(InputSampler, left, 0.0f).rgb
                * scale,
            1.0f);
        return;
    }
    const float2 right = float2((uv.x - 0.5f) * 2.0f, uv.y);
    OutputTexture[id.xy] = float4(
        InputTexture1.SampleLevel(InputSampler, right, 0.0f).rgb * scale,
        1.0f);
}
