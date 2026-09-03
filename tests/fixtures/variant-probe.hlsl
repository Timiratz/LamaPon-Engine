// Shaderバリアントの検証用。テストからのみ使います。
//
// キーワードが立っているかどうかで、まったく違う色を返します。
// 「#defineがコンパイルへ届いているか」「マテリアルのキーワードで
// 正しいバリアントが選ばれているか」を、画素の色だけで判定できる
// ようにするためです。

#pragma multi_compile _ VARIANT_PROBE_GREEN
#pragma shader_feature _ VARIANT_PROBE_BRIGHT

cbuffer ObjectBuffer : register(b0)
{
    row_major float4x4 World;
    row_major float4x4 ViewProjection;
    row_major float4x4 WorldInverseTranspose;
    float4 MaterialColor;
    float4 CameraPosition;
    float4 CameraForward;
    float4 MaterialParameters;
    float4 CustomParameters[8];
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
    float3 WorldPosition : TEXCOORD0;
    float3 WorldNormal : TEXCOORD1;
    float2 TexCoord : TEXCOORD2;
};

PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    const float4 worldPosition =
        mul(float4(input.Position, 1.0f), World);
    output.Position = mul(worldPosition, ViewProjection);
    output.WorldPosition = worldPosition.xyz;
    output.WorldNormal = normalize(
        mul(float4(input.Normal, 0.0f),
            WorldInverseTranspose).xyz);
    output.TexCoord = input.TexCoord;
    return output;
}

float4 PSMain(PixelInput input) : SV_Target
{
    // 分岐ではなくプリプロセッサで切り替えます。これがバリアントの
    // 要点で、選ばれなかった側はコンパイル結果に一切残りません。
#if defined(VARIANT_PROBE_GREEN)
    float3 color = float3(0.0f, 1.0f, 0.0f);
#else
    float3 color = float3(1.0f, 0.0f, 0.0f);
#endif
#if defined(VARIANT_PROBE_BRIGHT)
    color *= 1.0f;
#else
    color *= 0.25f;
#endif
    return float4(color, 1.0f);
}
