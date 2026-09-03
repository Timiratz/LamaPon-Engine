// レトロ3D（PS1風）の見本Shaderです。
//
// 当時のハードウェアが「できなかったこと」を意図的に再現します。
// 現代のGPUは正しく描いてしまうので、正しさをわざと外す形になります。
//
//  1. テクスチャの泳ぎ（アフィンテクスチャマッピング）
//     PS1はUVを補間するときに奥行き（W）で割る処理が無く、画面上で
//     まっすぐ補間していました。HLSLの noperspective がまさにこの
//     挙動なので、TexCoordへ付けるだけで再現できます。
//     ★ポリゴンが大きいほど激しく泳ぎます。細かく分割されたモデル
//       ではほとんど見えません（当時の床が派手に泳いだのは、床が
//       数枚の巨大な四角だったからです）。
//     ★逆に大きすぎると効果が過剰になります。Plane1枚（＝三角形2枚）
//       の巨大な床を浅い角度で見ると、遠近が完全に失われて模様が
//       横帯に伸びます。当時らしい「揺れ」にしたいなら、床を
//       格子状に分割してください（小さいPlaneを並べる／分割された
//       メッシュを使う）。当時のゲームもポリゴンを分割することで
//       破綻を抑えつつ、あの独特の揺れを出していました。
//
//  2. 頂点のカクつき（頂点スナップ）
//     頂点座標が整数精度しか無かったため、カメラを動かすと頂点が
//     カクカク跳びました。クリップ空間で格子へ丸めて再現します。
//
//  3. ドットの粗さ／色数の少なさ
//     色の量子化とディザ（網目状の混色）で再現します。解像度自体を
//     落としたいときは、品質設定の「レンダースケール」を下げて
//     ください（このShaderとは独立に効きます）。
//
// 当時の「ポリゴンの前後がおかしくなる」現象は再現しません。
// 深度バッファが無くポリゴン単位で並べ替えていたことに由来し、
// 今の環境で正確に真似るには深度テストを切る必要があって、
// 味ではなく不具合に見えるためです。

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

Texture2D AlbedoTexture : register(t0);
SamplerState MaterialSampler : register(s0);

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
    float3 WorldPosition : TEXCOORD0;
    float3 WorldNormal : TEXCOORD1;
    // ★これが本題。noperspective で透視補正が切れます。
    // 補間を切ったUVと、切らないUVの両方を渡して、
    // ピクセル側で「泳ぎ」の量として混ぜられるようにします。
    noperspective float2 AffineTexCoord : TEXCOORD2;
    float2 TexCoord : TEXCOORD3;
};

// スキニングモデル用（DirectXTKの頂点シェーダー出力の並びに合わせる
// 必要があるため、こちらは泳ぎを掛けられません）。
struct SkinnedPixelInput
{
    float2 TexCoord : TEXCOORD0;
    float4 WorldPosition : TEXCOORD1;
    float3 WorldNormal : TEXCOORD2;
    float4 Diffuse : COLOR0;
    float4 Position : SV_Position;
};

PixelInput BuildPixelInput(
    float3 position,
    float3 normal,
    float2 texCoord)
{
    PixelInput output;
    const float4 worldPosition =
        mul(float4(position, 1.0f), World);
    float4 clip = mul(worldPosition, ViewProjection);

    // 頂点スナップ。クリップ空間をWで割ってNDC（-1〜1）にし、
    // 格子へ丸めてから戻します。Wを保ったまま丸めるのが要点で、
    // ここでWごと壊すと遠近そのものが崩れます。
    const float snapAmount = saturate(CustomParameters[0].y);
    const float grid = max(CustomParameters[0].z, 1.0f);
    if (snapAmount > 0.001f && clip.w > 0.0001f)
    {
        const float2 ndc = clip.xy / clip.w;
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

PixelInput VSMain(VertexInput input)
{
    return BuildPixelInput(
        input.Position,
        input.Normal,
        input.TexCoord);
}

void SkinVertex(
    SkinnedVertexInput input,
    out float3 position,
    out float3 normal)
{
    float4x3 skinning = 0.0f;
    [unroll]
    for (uint index = 0u; index < 4u; ++index)
    {
        const uint bone = min(input.BlendIndices[index], 71u);
        const float weight = input.BlendWeights[index];
        skinning += BoneTransforms[bone] * weight;
    }
    position = mul(float4(input.Position, 1.0f), skinning);
    normal = normalize(mul(input.Normal, (float3x3)skinning));
}

PixelInput VSSkinnedMain(SkinnedVertexInput input)
{
    float3 position;
    float3 normal;
    SkinVertex(input, position, normal);
    return BuildPixelInput(
        position,
        normal,
        input.TexCoord);
}

// 4x4の秩序あるディザ行列（Bayer）。色数を落としたときの段差を、
// 網目状の混色でごまかします。当時のハードも似た手法でした。
float BayerDither(float2 pixel)
{
    static const float pattern[16] = {
         0.0f,  8.0f,  2.0f, 10.0f,
        12.0f,  4.0f, 14.0f,  6.0f,
         3.0f, 11.0f,  1.0f,  9.0f,
        15.0f,  7.0f, 13.0f,  5.0f
    };
    const uint2 coordinate = uint2(pixel) % 4u;
    return pattern[coordinate.y * 4u + coordinate.x]
        / 16.0f - 0.5f;
}

float4 ShadeRetro(
    float2 affineUv,
    float2 correctUv,
    float3 worldNormal,
    float2 pixelPosition)
{
    float2 uvScale = CustomParameters[2].xy;
    uvScale = float2(
        abs(uvScale.x) < 0.0001f ? 1.0f : uvScale.x,
        abs(uvScale.y) < 0.0001f ? 1.0f : uvScale.y);

    // 泳ぎの量。1で当時どおりのアフィン、0で現代の正しい補間。
    const float warp = saturate(CustomParameters[0].x);
    const float2 uv =
        lerp(correctUv, affineUv, warp) * uvScale
        + CustomParameters[2].zw;

    float4 albedo =
        AlbedoTexture.Sample(MaterialSampler, uv)
        * MaterialColor;
    clip(albedo.a - 0.08f);

    // 当時の質感に合わせて、明暗は頂点法線ベースの素朴な陰影だけ。
    // PBRの鏡面は入れません（当時に存在しなかったため）。
    const float3 lightDirection =
        normalize(float3(0.4f, 0.8f, 0.35f));
    const float diffuse =
        saturate(dot(normalize(worldNormal), lightDirection))
        * 0.65f + 0.35f;
    float3 color = albedo.rgb
        * diffuse
        * max(CustomParameters[1].y, 0.0f);

    // 色の量子化＋ディザ。段数を落とすほどレトロになります。
    const float levels = max(CustomParameters[0].w, 2.0f);
    const float dither =
        BayerDither(pixelPosition)
        * saturate(CustomParameters[1].x)
        / levels;
    color = floor(
        saturate(color + dither) * levels + 0.5f) / levels;

    return float4(color, albedo.a);
}

float4 PSMain(PixelInput input) : SV_Target
{
    return ShadeRetro(
        input.AffineTexCoord,
        input.TexCoord,
        input.WorldNormal,
        input.Position.xy);
}

float4 PSSkinnedMain(SkinnedPixelInput input) : SV_Target
{
    // スキニング側は補間指定を変えられないため、泳ぎは出ません
    // （色の量子化と陰影は同じように掛かります）。
    return ShadeRetro(
        input.TexCoord,
        input.TexCoord,
        input.WorldNormal,
        input.Position.xy);
}
