// 互換名CSMainのCompute入口で出力範囲と0.rgbの定数色を検査する。
// Compute検査の追加値と出力寸法
cbuffer ComputeEffectConstants : register(b0)
{
    // 0.rgb出力定数色・他予約
    float4 CustomParameters[8];
    // 出力幅高さXY・逆幅高さZW
    float4 OutputSize;
};

// u0のCompute出力RGBA
RWTexture2D<float4> OutputTexture : register(u0);

// 範囲内の出力画素へ0.rgbを不透明で書く(threadId: Dispatch内の画素XYZ)。
[numthreads(8, 8, 1)]
void CSMain(uint3 threadId : SV_DispatchThreadID)
{
    // 8×8への切り上げで届く範囲外スレッドは出力へ触れない。
    if (any(threadId.xy >= uint2(OutputSize.xy)))
    {
        return;
    }
    OutputTexture[threadId.xy] = float4(
        CustomParameters[0].rgb,
        1.0f);
}
