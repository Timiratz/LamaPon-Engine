// Model RendererのMaterial custom shaderで、輪郭（VSOutline／PSOutline）と
// 遮蔽表示（PSOccluded）のpassを検査するためのテスト専用Shaderです。
// ゲームでは使いません。通常の描画は明暗だけの灰色、輪郭は
// CustomParameters[3].yzw、遮蔽表示はCustomParameters[4].rgbの色で返します。

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
    float3 WorldNormal : TEXCOORD0;
    float2 TexCoord : TEXCOORD1;
};

struct OutlinePixelInput
{
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    output.Position = mul(mul(float4(input.Position, 1.0f), World), ViewProjection);
    output.WorldNormal = mul(float4(input.Normal, 0.0f), WorldInverseTranspose).xyz;
    output.TexCoord = input.TexCoord;
    return output;
}

float4 PSMain(PixelInput input) : SV_Target
{
    const float light = 0.35f + 0.65f * saturate(dot(
        normalize(input.WorldNormal),
        normalize(float3(0.3f, 0.6f, 0.75f))));
    return float4(0.8f * light, 0.8f * light, 0.8f * light, 1.0f);
}

// CustomParameters[3].xだけ、頂点を法線の向きへ広げます。
OutlinePixelInput VSOutline(VertexInput input)
{
    const float3 position = input.Position
        + input.Normal * max(CustomParameters[3].x, 0.0f);
    OutlinePixelInput output;
    output.Position = mul(mul(float4(position, 1.0f), World), ViewProjection);
    output.TexCoord = input.TexCoord;
    return output;
}

float4 PSOutline(OutlinePixelInput input) : SV_Target
{
    return float4(saturate(CustomParameters[3].yzw), 1.0f);
}

// 不透明度0.6で返し、非プレマルチプライドの合成で重なることも比べます。
float4 PSOccluded(PixelInput input) : SV_Target
{
    return float4(saturate(CustomParameters[4].rgb), 0.6f);
}
