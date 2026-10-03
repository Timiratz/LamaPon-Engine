#include "LamaPonNoise.hlsli"

/* LAMAPON_RENDER_STATE
{ "blend": "opaque", "cull": "back", "depthWrite": true }
*/

/* LAMAPON_PROPERTIES
[
  { "target": "0.x", "type": "float",
    "name": "種類（0=Value 1=Perlin 2=fBm 3=Worley 4=Curl）",
    "min": 0.0, "max": 4.0, "default": 2.0 },
  { "target": "0.y", "type": "float", "name": "大きさ（スケール）",
    "min": 0.5, "max": 64.0, "default": 8.0 },
  { "target": "0.z", "type": "float", "name": "重ねる回数（fBmのみ）",
    "min": 1.0, "max": 8.0, "default": 5.0 },
  { "target": "0.w", "type": "float", "name": "流れる速さ",
    "min": 0.0, "max": 2.0, "default": 0.0 },
  { "target": "1.x", "type": "float", "name": "コントラスト",
    "min": 0.1, "max": 4.0, "default": 1.0 },
  { "target": "1.y", "type": "float", "name": "明るさ",
    "min": 0.0, "max": 3.0, "default": 1.0 },
  { "target": "2.rgb", "type": "color", "name": "暗い側の色",
    "default": [0.05, 0.12, 0.25] },
  { "target": "3.rgb", "type": "color", "name": "明るい側の色",
    "default": [0.95, 0.9, 0.7] }
]
*/

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
    // ノイズ・色・照明設定の8本の値
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

// 互換配置の表面色画像
Texture2D AlbedoTexture : register(t0);
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
    const float4 worldPosition =
        mul(float4(position, 1.0f), World);
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
    return BuildPixelInput(
        input.Position,
        input.Normal,
        input.TexCoord);
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
    return BuildPixelInput(
        position,
        normal,
        input.TexCoord);
}

// 選択したノイズを簡易照明と色へ変換する(input: World法線とUVのPixel入力)。
float4 ShadeNoise(PixelInput input)
{
    // 表示するノイズの種類0～4
    const float kind = CustomParameters[0].x;
    // 採取するUVの拡大倍率
    const float scale = max(CustomParameters[0].y, 0.001f);
    // fBmを重ねる回数
    const int octaves = (int)max(CustomParameters[0].z, 1.0f);
    // 毎秒のUV移動量
    const float flowSpeed = CustomParameters[0].w;

    // 1時間で巻き戻る経過秒
    const float time = TimeParameters.x;


    // 拡大と時間移動後の採取UV
    float2 samplePosition =
        input.TexCoord * scale
        + float2(time * flowSpeed, time * flowSpeed * 0.6f);

    // 色混合へ使うノイズ値
    float value = 0.0f;
    if (kind < 0.5f)
    {
        value = LamaPonValueNoise2D(samplePosition);
    }
    else if (kind < 1.5f)
    {
        value = LamaPonPerlinNoise2D(samplePosition);
    }
    else if (kind < 2.5f)
    {
        value = LamaPonFractalNoise2D(
            samplePosition, octaves, 2.0f, 0.5f);
    }
    else if (kind < 3.5f)
    {
        value = LamaPonWorleyNoise2D(samplePosition);
    }
    else
    {

        // Curlノイズの2D方向
        const float2 curl =
            LamaPonCurlNoise2D(samplePosition, 0.01f);
        value = saturate(length(curl) * 0.5f);
    }


    // 0.5を中心に伸縮する倍率
    const float contrast = max(CustomParameters[1].x, 0.001f);
    value = saturate((value - 0.5f) * contrast + 0.5f);

    // 値0に対応するRGB
    const float3 darkColor = CustomParameters[2].rgb;
    // 値1に対応するRGB
    const float3 lightColor = CustomParameters[3].rgb;
    // ノイズ値で混合したRGB
    float3 color = lerp(darkColor, lightColor, value);


    // 表面から簡易光源への単位方向
    const float3 lightDirection =
        normalize(float3(0.4f, 0.8f, 0.35f));
    // 環境分を含む簡易照明倍率
    const float diffuse =
        saturate(dot(normalize(input.WorldNormal),
            lightDirection)) * 0.4f + 0.6f;
    color *= diffuse * max(CustomParameters[1].y, 0.0f);

    return float4(color, MaterialColor.a);
}

// 通常Meshへノイズ色を適用する(input: 通常Pixel入力)。
float4 PSMain(PixelInput input) : SV_Target
{
    return ShadeNoise(input);
}

// スキン入力を共通構造へ変換してノイズ色を適用する(input: スキンPixel入力)。
float4 PSSkinnedMain(SkinnedPixelInput input) : SV_Target
{
    // 共通構造へ変換したPixel入力
    PixelInput pixel;
    pixel.Position = input.Position;
    pixel.WorldPosition = input.WorldPosition.xyz;
    pixel.WorldNormal = input.WorldNormal;
    pixel.TexCoord = input.TexCoord;
    return ShadeNoise(pixel);
}
