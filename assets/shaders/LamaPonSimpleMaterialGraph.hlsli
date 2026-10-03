// Simple Shader Graph生成コードで共有し、定数バッファの順序とregisterをエンジン側へ合わせる。

// CPUと配置を揃える物体定数
cbuffer ObjectBuffer : register(b0)
{
    // 行優先のWorld変換行列
    row_major float4x4 World;
    // 行優先のビュー透視合成行列
    row_major float4x4 ViewProjection;
    // 行優先の法線変換用逆転置
    row_major float4x4 WorldInverseTranspose;
    // 材質のRGBA倍率
    float4 MaterialColor;
    // World視点XYZ・W予約
    float4 CameraPosition;
    // World視線XYZ・W予約
    float4 CameraForward;
    // 粗さ・法線強度有無・金属度
    float4 MaterialParameters;
    // Graphへ渡す8本のカスタム値
    float4 CustomParameters[8];
    // 粗さ・金属・遮蔽有無と強度
    float4 MaterialTextureParameters;
    // 発光RGB・W画像使用フラグ
    float4 EmissiveParameters;
    // 1時間周期秒・差分秒・フレーム数
    float4 TimeParameters;
};

// スキン変形用の骨定数
cbuffer BoneBuffer : register(b2)
{
    // 最大72骨の変形行列
    float4x3 BoneTransforms[72];
};

// t0の表面色画像
Texture2D AlbedoTexture : register(t0);
// t1の法線画像
Texture2D NormalTexture : register(t1);
// t7のGraph追加画像0
Texture2D CustomTexture0 : register(t7);
// t8のGraph追加画像1
Texture2D CustomTexture1 : register(t8);
// t9のGraph追加画像2
Texture2D CustomTexture2 : register(t9);
// t10のGraph追加画像3
Texture2D CustomTexture3 : register(t10);
// 材質画像の採取設定
SamplerState MaterialSampler : register(s0);

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
    // 補間するWorld位置
    float3 WorldPosition : TEXCOORD0;
    // 補間するWorld法線
    float3 WorldNormal : TEXCOORD1;
    // 補間する画像UV
    float2 TexCoord : TEXCOORD2;
};

struct SkinnedPixelInput
{
    // 補間する画像UV
    float2 TexCoord : TEXCOORD0;
    // 補間するWorld同次位置
    float4 WorldPosition : TEXCOORD1;
    // 補間するWorld法線
    float3 WorldNormal : TEXCOORD2;
    // 互換入力の頂点色
    float4 Diffuse : COLOR0;
    // 透視投影後の画面位置
    float4 Position : SV_Position;
};

// World位置・法線と透視位置を生成する(position: ローカル位置, normal: ローカル法線, texCoord: 画像UV)。
PixelInput BuildPixelInput(
    float3 position,
    float3 normal,
    float2 texCoord)
{
    // World変換後のPixel入力
    PixelInput output;
    // World空間の同次位置
    const float4 worldPosition = mul(float4(position, 1.0f), World);
    output.Position = mul(worldPosition, ViewProjection);
    output.WorldPosition = worldPosition.xyz;
    output.WorldNormal = normalize(
        mul(float4(normal, 0.0f), WorldInverseTranspose).xyz);
    output.TexCoord = texCoord;
    return output;
}

// 通常頂点を共通のPixel入力へ変換する(input: 通常頂点)。
PixelInput VSMain(VertexInput input)
{
    return BuildPixelInput(input.Position, input.Normal, input.TexCoord);
}

// 最大4骨を加重合成して位置と法線を変換する(input: スキン頂点, position: 出力位置, normal: 出力法線)。
void SkinVertex(
    SkinnedVertexInput input,
    out float3 position,
    out float3 normal)
{
    // 4骨の加重合成行列
    float4x3 skinning = 0.0f;
    // 合成する骨影響の番号
    [unroll]
    for (uint index = 0u; index < 4u; ++index)
    {
        // 0～71へ制限した骨番号
        const uint bone = min(input.BlendIndices[index], 71u);
        // 現在の骨の影響比
        const float weight = input.BlendWeights[index];
        skinning += BoneTransforms[bone] * weight;
    }
    position = mul(float4(input.Position, 1.0f), skinning);
    normal = normalize(mul(input.Normal, (float3x3)skinning));
}

// 骨変形後の頂点を共通のPixel入力へ変換する(input: スキン頂点)。
PixelInput VSSkinnedMain(SkinnedVertexInput input)
{
    // 骨変形後のローカル位置
    float3 position;
    // 骨変形後のローカル法線
    float3 normal;
    SkinVertex(input, position, normal);
    return BuildPixelInput(position, normal, input.TexCoord);
}

// スキンPS入力を共通の入力構造へ変換する(input: スキンPixel入力)。
PixelInput ToPixelInput(SkinnedPixelInput input)
{
    // 共通構造へ変換したPixel入力
    PixelInput pixel;
    pixel.Position = input.Position;
    pixel.WorldPosition = input.WorldPosition.xyz;
    pixel.WorldNormal = input.WorldNormal;
    pixel.TexCoord = input.TexCoord;
    return pixel;
}
