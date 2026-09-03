// Deliberately broken shader for the error placeholder test.
// わざとコンパイルに失敗させるための見本です。直さないでください。
// （存在しない関数を呼びます。構文は通るので、コンパイラが
//   「undeclared identifier」で落ちる形にしています）

cbuffer ObjectBuffer : register(b0)
{
    row_major float4x4 World;
    row_major float4x4 ViewProjection;
};

struct VertexInput
{
    float3 Position : SV_Position;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD0;
};

struct PixelInput
{
    float4 Position : SV_Position;
};

PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    output.Position = mul(
        mul(float4(input.Position, 1.0f), World),
        ViewProjection);
    return output;
}

float4 PSMain(PixelInput input) : SV_Target
{
    return ThisFunctionDoesNotExist(input.Position);
}
