// 3D Shaderのコンパイル失敗時に、骨姿勢を保ってマゼンタで描く内部用の代替Shader。

/* LAMAPON_RENDER_STATE
{ "blend": "opaque", "cull": "none", "depthWrite": true }
*/


// CPUと配置を揃える物体定数
cbuffer ObjectBuffer : register(b0)
{
    // 行優先のWorld変換行列
    row_major float4x4 World;
    // 行優先のビュー透視合成行列
    row_major float4x4 ViewProjection;
    // 互換配置の法線変換行列
    row_major float4x4 WorldInverseTranspose;
    // 互換配置の材質RGBA
    float4 MaterialColor;
    // 互換配置の視点位置
    float4 CameraPosition;
    // 互換配置の視線方向
    float4 CameraForward;
    // 互換配置の材質設定
    float4 MaterialParameters;
    // 互換配置の8本のカスタム値
    float4 CustomParameters[8];
};

// スキン変形用の骨定数
cbuffer BoneBuffer : register(b2)
{
    // 最大72骨の変形行列
    float4x3 BoneTransforms[72];
};

struct VertexInput
{
    // ローカル頂点位置
    float3 Position : SV_Position;
    // 互換入力の頂点法線
    float3 Normal : NORMAL;
    // 互換入力の画像UV
    float2 TexCoord : TEXCOORD0;
};

struct SkinnedVertexInput
{
    // 骨変形前の頂点位置
    float3 Position : SV_Position;
    // 互換入力の頂点法線
    float3 Normal : NORMAL;
    // 互換入力の頂点接線
    float4 Tangent : TANGENT;
    // 互換入力の頂点色
    float4 Color : COLOR;
    // 互換入力の画像UV
    float2 TexCoord : TEXCOORD0;
    // 影響する4骨の番号
    uint4 BlendIndices : BLENDINDICES0;
    // 4骨の影響比
    float4 BlendWeights : BLENDWEIGHT0;
};

struct PixelInput
{
    // 透視投影後の画面位置
    float4 Position : SV_Position;
};

// 通常頂点を透視投影する(input: 通常頂点)。
PixelInput VSMain(VertexInput input)
{
    // 投影後のPixel入力
    PixelInput output;
    output.Position = mul(
        mul(float4(input.Position, 1.0f), World),
        ViewProjection);
    return output;
}

// 4骨の姿勢を反映して頂点を透視投影する(input: スキン頂点)。
PixelInput VSSkinnedMain(SkinnedVertexInput input)
{

    // 4骨の加重合成行列
    float4x3 skinning = 0.0f;
    // 合成する骨影響の番号
    [unroll]
    for (uint index = 0u; index < 4u; ++index)
    {
        // 0～71へ制限した骨番号
        const uint bone = min(input.BlendIndices[index], 71u);
        skinning += BoneTransforms[bone] * input.BlendWeights[index];
    }
    // 骨変形後のローカル位置
    const float3 position = mul(
        float4(input.Position, 1.0f),
        skinning);

    // 投影後のPixel入力
    PixelInput output;
    output.Position = mul(
        mul(float4(position, 1.0f), World),
        ViewProjection);
    return output;
}

// 照明に依存しないマゼンタのエラー色を返す。
float4 ShadeError()
{
    // 照明を使わずRGBを1以内に保ち、エラー色を返す。
    return float4(1.0f, 0.0f, 1.0f, 1.0f);
}

// 通常Meshのエラー色を返す(input: 互換入力の画面位置)。
float4 PSMain(PixelInput input) : SV_Target
{
    return ShadeError();
}

// スキンMeshのエラー色を返す(input: 互換入力の画面位置)。
float4 PSSkinnedMain(PixelInput input) : SV_Target
{
    return ShadeError();
}
