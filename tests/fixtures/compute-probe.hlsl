// Compute Shaderの公開APIが動くことを検査するテスト専用Shaderです。
// ゲームでは使いません。
//
// 出力テクスチャへ決まった模様を書きます。左半分は一様な赤、
// 右半分は上から下への緑のグラデーションです。テスト側は表示された
// 絵を読んで、この模様になっているかを確かめます。
//
// 同じディスパッチの中で他のスレッドが書いた画素は読みません。
// スレッドグループ間の実行順は決まっていないので、読むと結果が
// 毎回変わります（最初そう書いて、競合に気付いて直しました）。
//
// CustomParameters[0].x = 左半分へ書く赤の強さ（0〜1）

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

    // halfはHLSLの型名なので変数名には使えません。
    const uint halfWidth = (uint)OutputSize.x / 2;
    if (id.x < halfWidth)
    {
        OutputTexture[id.xy] = float4(
            CustomParameters[0].x, 0.0f, 0.0f, 1.0f);
        return;
    }

    const float gradient =
        (float)id.y / max(OutputSize.y - 1.0f, 1.0f);
    OutputTexture[id.xy] =
        float4(0.0f, gradient, 0.0f, 1.0f);
}
