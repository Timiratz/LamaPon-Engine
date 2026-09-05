// エラー時の代替表示を検証するため、コンパイルに失敗するシェーダーです。
// 存在しない関数を呼び、undeclared identifierエラーを発生させます。

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
