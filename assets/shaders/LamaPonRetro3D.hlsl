// UVのアフィン補間・頂点スナップ・色量子化とディザでレトロ3Dを再現する見本Shader。

/* LAMAPON_RENDER_STATE
{ "blend": "opaque", "cull": "back", "depthWrite": true }
*/

/* LAMAPON_PROPERTIES
[
  { "target": "0.x", "type": "float", "name": "テクスチャの泳ぎ",
    "min": 0.0, "max": 1.0, "default": 1.0 },
  { "target": "0.y", "type": "float", "name": "頂点のカクつき",
    "min": 0.0, "max": 1.0, "default": 1.0 },
  { "target": "0.z", "type": "float", "name": "頂点の粗さ（格子数）",
    "min": 16.0, "max": 640.0, "default": 160.0 },
  { "target": "0.w", "type": "float", "name": "色の段数",
    "min": 2.0, "max": 64.0, "default": 16.0 },
  { "target": "1.x", "type": "float", "name": "ディザの強さ",
    "min": 0.0, "max": 1.0, "default": 0.5 },
  { "target": "1.y", "type": "float", "name": "明るさ",
    "min": 0.0, "max": 3.0, "default": 1.0 },
  { "target": "2.xy", "type": "vector", "name": "UVの拡大",
    "default": [1.0, 1.0] },
  { "target": "2.zw", "type": "vector", "name": "UVのずらし",
    "default": [0.0, 0.0] },
  { "target": "7.w", "type": "bool",
    "name": "テクスチャを補間しない（ドット感）", "default": true }
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
    // 表面色のRGBA倍率
    float4 MaterialColor;
    // 互換配置の視点位置
    float4 CameraPosition;
    // 互換配置の視線方向
    float4 CameraForward;
    // 互換配置の材質設定
    float4 MaterialParameters;
    // レトロ効果とUV設定の8本の値
    float4 CustomParameters[8];
};

// スキン変形用の骨定数
cbuffer BoneBuffer : register(b2)
{
    // 最大72骨の変形行列
    float4x3 BoneTransforms[72];
};

// 表面色画像
Texture2D AlbedoTexture : register(t0);
// 色画像の採取設定
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

    // 透視補正なしで補間するUV
    noperspective float2 AffineTexCoord : TEXCOORD2;
    // 透視補正して補間するUV
    float2 TexCoord : TEXCOORD3;
};

// スキンPS入力はDirectXTKの出力順を保ち、UVのアフィン補間を適用しない。
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

// 頂点を格子へ丸め、2種類の補間UVを生成する(position: ローカル位置, normal: ローカル法線, texCoord: 画像UV)。
PixelInput BuildPixelInput(
    float3 position,
    float3 normal,
    float2 texCoord)
{
    // 投影後のPixel入力
    PixelInput output;
    // World空間の同次位置
    const float4 worldPosition =
        mul(float4(position, 1.0f), World);
    // 丸め前後のクリップ同次位置
    float4 clip = mul(worldPosition, ViewProjection);

    // Wを保ったままNDCを格子へ丸め、透視除算後の位置だけを変更する。
    // 頂点スナップの混合比
    const float snapAmount = saturate(CustomParameters[0].y);
    // NDCを丸める格子数
    const float grid = max(CustomParameters[0].z, 1.0f);
    if (snapAmount > 0.001f && clip.w > 0.0001f)
    {
        // 丸め前の正規化画面XY
        const float2 ndc = clip.xy / clip.w;
        // 格子へ丸めた画面XY
        const float2 snapped = round(ndc * grid) / grid;
        clip.xy = lerp(ndc, snapped, snapAmount) * clip.w;
    }

    output.Position = clip;
    output.WorldPosition = worldPosition.xyz;
    output.WorldNormal = normalize(
        mul(float4(normal, 0.0f), WorldInverseTranspose).xyz);
    output.AffineTexCoord = texCoord;
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

// 骨変形後の位置へ頂点スナップを適用する(input: スキン頂点)。
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


// 4×4 Bayer行列から-0.5以上0.5未満のディザを返す(pixel: 画面画素位置)。
float BayerDither(float2 pixel)
{
    // 4×4 Bayerのディザ順序
    static const float pattern[16] = {
         0.0f,  8.0f,  2.0f, 10.0f,
        12.0f,  4.0f, 14.0f,  6.0f,
         3.0f, 11.0f,  1.0f,  9.0f,
        15.0f,  7.0f, 13.0f,  5.0f
    };
    // ディザ行列内のXY番号
    const uint2 coordinate = uint2(pixel) % 4u;
    return pattern[coordinate.y * 4u + coordinate.x]
        / 16.0f - 0.5f;
}

// UV補間を混ぜ、照明色を量子化する(affineUv: 透視補正なしのUV, correctUv: 透視補正したUV, worldNormal: World法線, pixelPosition: 画面画素位置)。
float4 ShadeRetro(
    float2 affineUv,
    float2 correctUv,
    float3 worldNormal,
    float2 pixelPosition)
{
    // 零に近い値を1とするUV倍率
    float2 uvScale = CustomParameters[2].xy;
    uvScale = float2(
        abs(uvScale.x) < 0.0001f ? 1.0f : uvScale.x,
        abs(uvScale.y) < 0.0001f ? 1.0f : uvScale.y);


    // アフィンUVの混合比
    const float warp = saturate(CustomParameters[0].x);
    // 拡縮と移動後の採取UV
    const float2 uv =
        lerp(correctUv, affineUv, warp) * uvScale
        + CustomParameters[2].zw;

    // 画像と材質色を掛けたRGBA
    float4 albedo =
        AlbedoTexture.Sample(MaterialSampler, uv)
        * MaterialColor;
    clip(albedo.a - 0.08f);


    // 表面から簡易光源への単位方向
    const float3 lightDirection =
        normalize(float3(0.4f, 0.8f, 0.35f));
    // 環境分を含む簡易照明倍率
    const float diffuse =
        saturate(dot(normalize(worldNormal), lightDirection))
        * 0.65f + 0.35f;
    // 量子化前後の表面RGB
    float3 color = albedo.rgb
        * diffuse
        * max(CustomParameters[1].y, 0.0f);


    // 最低2の色段数
    const float levels = max(CustomParameters[0].w, 2.0f);
    // 色段数と強度を反映したディザ
    const float dither =
        BayerDither(pixelPosition)
        * saturate(CustomParameters[1].x)
        / levels;
    color = floor(
        saturate(color + dither) * levels + 0.5f) / levels;

    return float4(color, albedo.a);
}

// 通常MeshへアフィンUVとレトロの色処理を適用する(input: 通常Pixel入力)。
float4 PSMain(PixelInput input) : SV_Target
{
    return ShadeRetro(
        input.AffineTexCoord,
        input.TexCoord,
        input.WorldNormal,
        input.Position.xy);
}

// スキンMeshへ色処理を適用し、UVの泳ぎは除く(input: スキンPixel入力)。
float4 PSSkinnedMain(SkinnedPixelInput input) : SV_Target
{

    return ShadeRetro(
        input.TexCoord,
        input.TexCoord,
        input.WorldNormal,
        input.Position.xy);
}
