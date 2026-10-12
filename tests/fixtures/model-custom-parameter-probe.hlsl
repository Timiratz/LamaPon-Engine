// 読み込みモデルの部品描画へ届いたCustomParameters[0]のRGBを不透明で塗る。
// 骨なし・スキンの両方の頂点入口を持ち、どちらの描画経路でも同じ色を返す。

// CPUと配置を揃える物体定数
cbuffer ObjectBuffer : register(b0)
{
    // 行優先のWorld変換行列
    row_major float4x4 World;
    // 行優先のビュー透視合成行列
    row_major float4x4 ViewProjection;
    // 行優先の法線変換用逆転置
    row_major float4x4 WorldInverseTranspose;
    // 通常・スキンの材質RGBA
    float4 MaterialColor;
    // World視点XYZ・W予約
    float4 CameraPosition;
    // World視線XYZ・W予約
    float4 CameraForward;
    // 粗さ・法線強度有無・金属度
    float4 MaterialParameters;
    // 0.rgbが検査色
    float4 CustomParameters[8];
};

// 最大72骨のスキン定数
cbuffer BoneBuffer : register(b2)
{
    // 最大72骨の変形行列
    float4x3 BoneTransforms[72];
};

struct VertexInput
{
    // ローカル頂点位置
    float3 Position : SV_Position;
    // ローカル頂点法線
    float3 Normal : NORMAL;
    // 画像UV
    float2 TexCoord : TEXCOORD0;
};

struct SkinnedVertexInput
{
    // 骨変形前の頂点位置
    float3 Position : SV_Position;
    // 骨変形前の頂点法線
    float3 Normal : NORMAL;
    // 互換入力の頂点接線
    float4 Tangent : TANGENT;
    // 互換入力の頂点色
    float4 Color : COLOR;
    // 画像UV
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

// DirectXTKのper-pixel lighting出力に合わせ、TEXCOORD0をUV・1をWorld位置・2を法線に保つ。
struct SkinnedPixelInput
{
    // 補間する画像UV
    float2 TexCoord : TEXCOORD0;
    // 補間するWorld同次位置
    float4 WorldPosition : TEXCOORD1;
    // 補間するWorld法線
    float3 WorldNormal : TEXCOORD2;
    // 材質色と重複する互換頂点色
    float4 Diffuse : COLOR0;
    // 透視投影後の画面位置
    float4 Position : SV_Position;
};

// 通常頂点をWorld変換する(input: 通常頂点)。
PixelInput VSMain(VertexInput input)
{
    // 画面位置だけのPixel入力
    PixelInput output;
    output.Position = mul(
        mul(float4(input.Position, 1.0f), World),
        ViewProjection);
    return output;
}

// 骨変形後の頂点をWorld変換する(input: スキン頂点)。
PixelInput VSSkinnedMain(SkinnedVertexInput input)
{
    // 4骨の加重合成行列
    float4x3 skinning = 0.0f;
    // 合成する骨の番号
    [unroll]
    for (uint index = 0u; index < 4u; ++index)
    {
        // 0～71へ制限した骨番号
        const uint bone = min(input.BlendIndices[index], 71u);
        skinning += BoneTransforms[bone] * input.BlendWeights[index];
    }
    // 画面位置だけのPixel入力
    PixelInput output;
    output.Position = mul(
        mul(float4(mul(float4(input.Position, 1.0f), skinning), 1.0f),
            World),
        ViewProjection);
    return output;
}

// 検査色を不透明で返す(input: 画面位置)。
float4 PSMain(PixelInput input) : SV_Target
{
    return float4(CustomParameters[0].rgb, 1.0f);
}

// DirectXTKの頂点変形を使う経路でも検査色を返す(input: DirectXTK互換のPixel入力)。
float4 PSSkinnedMain(SkinnedPixelInput input) : SV_Target
{
    return float4(CustomParameters[0].rgb, 1.0f);
}
