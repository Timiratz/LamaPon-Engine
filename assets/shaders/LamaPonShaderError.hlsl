// LamaPonのシェーダーでエラーを表示するための代替シェーダーです。
// 自作Shaderがコンパイルできなかったとき、この見た目で描かれます。
// 標準Litで代役を務めると「動いているように見えて実は壊れている」
// 状態になるため、一目で分かるマゼンタにしています。
// このファイル自体はエンジンが直接読みます（マテリアルへ割り当てて
// 使うものではありません）。

/* LAMAPON_RENDER_STATE
{ "blend": "opaque", "cull": "none", "depthWrite": true }
*/
// 裏面も描きます。法線が反転したメッシュでも「見えない」ままに
// ならず、壊れていることが必ず伝わるようにするためです。

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

cbuffer BoneBuffer : register(b2)
{
    float4x3 BoneTransforms[72];
};

struct VertexInput
{
    float3 Position : SV_Position;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD0;
};

struct SkinnedVertexInput
{
    float3 Position : SV_Position;
    float3 Normal : NORMAL;
    float4 Tangent : TANGENT;
    float4 Color : COLOR;
    float2 TexCoord : TEXCOORD0;
    uint4 BlendIndices : BLENDINDICES0;
    float4 BlendWeights : BLENDWEIGHT0;
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

PixelInput VSSkinnedMain(SkinnedVertexInput input)
{
    // 骨は動かします。ポーズが崩れると「どのオブジェクトが
    // 壊れているのか」が分からなくなるためです。
    float4x3 skinning = 0.0f;
    [unroll]
    for (uint index = 0u; index < 4u; ++index)
    {
        const uint bone = min(input.BlendIndices[index], 71u);
        skinning += BoneTransforms[bone] * input.BlendWeights[index];
    }
    const float3 position = mul(
        float4(input.Position, 1.0f),
        skinning);

    PixelInput output;
    output.Position = mul(
        mul(float4(position, 1.0f), World),
        ViewProjection);
    return output;
}

float4 ShadeError()
{
    // ライティングを通しません。暗い場所でも必ず同じ色で見えます。
    // 1.0を超えるとBloomが拾って輪郭がにじむので、ちょうど1.0。
    return float4(1.0f, 0.0f, 1.0f, 1.0f);
}

float4 PSMain(PixelInput input) : SV_Target
{
    return ShadeError();
}

float4 PSSkinnedMain(PixelInput input) : SV_Target
{
    return ShadeError();
}
