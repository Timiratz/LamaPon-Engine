// Computeの出力配置を左の赤と右の緑勾配で検査し、他スレッドの出力を参照しない。
// Compute検査の追加値と出力寸法
cbuffer ComputeParameters : register(b0)
{
    // 0.x左半分の赤強度・他予約
    float4 CustomParameters[8];
    // 出力幅高さXY・逆幅高さZW
    float4 OutputSize;
};

// 互換配置のCompute入力0
Texture2D InputTexture0 : register(t0);
// 互換配置のCompute入力1
Texture2D InputTexture1 : register(t1);
// 互換配置の入力採取設定
SamplerState InputSampler : register(s0);
// u0のCompute出力RGBA
RWTexture2D<float4> OutputTexture : register(u0);

// 範囲内の出力画素へ左の赤と右の縦緑勾配を書く(id: Dispatch内の画素XYZ)。
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    // 8×8への切り上げで届く範囲外スレッドは出力へ触れない。
    if (id.x >= (uint)OutputSize.x
        || id.y >= (uint)OutputSize.y)
    {
        return;
    }

    // 出力左半分の終端画素X
    const uint halfWidth = (uint)OutputSize.x / 2;
    if (id.x < halfWidth)
    {
        OutputTexture[id.xy] = float4(
            CustomParameters[0].x, 0.0f, 0.0f, 1.0f);
        return;
    }

    // 出力Yを正規化した緑の強度
    const float gradient =
        (float)id.y / max(OutputSize.y - 1.0f, 1.0f);
    OutputTexture[id.xy] =
        float4(0.0f, gradient, 0.0f, 1.0f);
}
