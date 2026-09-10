cbuffer ComputeEffectConstants : register(b0)
{
    float4 CustomParameters[8];
    float4 OutputSize;
};

RWTexture2D<float4> OutputTexture : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 threadId : SV_DispatchThreadID)
{
    if (any(threadId.xy >= uint2(OutputSize.xy)))
    {
        return;
    }
    OutputTexture[threadId.xy] = float4(
        CustomParameters[0].rgb,
        1.0f);
}
