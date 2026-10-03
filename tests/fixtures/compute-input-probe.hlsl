// Computeのt0・t1・s0の入力配置を検査し、左右の採取RGBへ0.xの倍率を掛ける。
// Compute検査の追加値と出力寸法
cbuffer ComputeParameters : register(b0)
{
    // 0.x出力RGBの倍率・他予約
    float4 CustomParameters[8];
    // 出力幅高さXY・逆幅高さZW
    float4 OutputSize;
};

// t0のCompute入力画像
Texture2D InputTexture0 : register(t0);
// t1のCompute入力画像
Texture2D InputTexture1 : register(t1);
// Compute入力の採取設定
SamplerState InputSampler : register(s0);
// u0のCompute出力RGBA
RWTexture2D<float4> OutputTexture : register(u0);

// 範囲内の出力画素へ左t0・右t1を描く(id: Dispatch内の画素XYZ)。
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    // 8×8への切り上げで届く範囲外スレッドは出力へ触れない。
    if (id.x >= (uint)OutputSize.x
        || id.y >= (uint)OutputSize.y)
    {
        return;
    }

    // 採取色へ掛ける倍率
    const float scale = CustomParameters[0].x;
    // 画像採取UV
    const float2 uv = (float2(id.xy) + 0.5f) * OutputSize.zw;
    if (uv.x < 0.5f)
    {
        // 左半分を0～1へ戻すUV
        const float2 left = float2(uv.x * 2.0f, uv.y);
        OutputTexture[id.xy] = float4(
            InputTexture0.SampleLevel(InputSampler, left, 0.0f).rgb
                * scale,
            1.0f);
        return;
    }
    // 右半分を0～1へ戻すUV
    const float2 right = float2((uv.x - 0.5f) * 2.0f, uv.y);
    OutputTexture[id.xy] = float4(
        InputTexture1.SampleLevel(InputSampler, right, 0.0f).rgb * scale,
        1.0f);
}
