// LamaPonのカスタムマテリアル用シェーダーテンプレートです。
// エントリーポイント名はVSMainとPSMainのままにします（Shader Model 5.0）。
// インスペクターの値はCustomParameters[0..7]から参照できます。

/* LAMAPON_RENDER_STATE
{ "blend": "opaque", "cull": "back", "depthWrite": true }
*/
// ↑描画状態の指定です（この宣言は実行時にも使われます）。
//   blend: opaque（既定）／alpha（ガラス・フェード）／
//          additive（発光・エフェクト）／premultiplied
//   cull:  back（既定・裏面を描かない）／front／none（両面）
//   depthWrite / depthTest: 深度への書き込みとテスト。
//   blendがopaque以外でdepthWriteを省略した場合は、半透明の
//   重ね合わせを保つため自動的にfalseになります。

/* LAMAPON_PROPERTIES
[
  { "target": "0.rgb", "type": "color", "name": "着色",
    "default": [1.0, 1.0, 1.0] },
  { "target": "0.a", "type": "float", "name": "着色の強さ",
    "min": 0.0, "max": 1.0, "default": 0.0 },
  { "target": "1.x", "type": "float", "name": "発光量",
    "min": 0.0, "max": 4.0, "default": 0.0 },
  { "target": "1.y", "type": "float", "name": "縁の光り（リム）",
    "min": 0.0, "max": 2.0, "default": 0.0 },
  { "target": "2.xy", "type": "vector", "name": "UVの拡大",
    "default": [1.0, 1.0] },
  { "target": "2.zw", "type": "vector", "name": "UVのずらし",
    "default": [0.0, 0.0] },
  { "target": "t7", "type": "texture", "name": "マスク（未使用）" }
]
*/
// ↑この宣言をInspectorが読み取り、名前付きの調整UIを出します。
// target は CustomParameters の「番号.成分」です（成分はx/y/z/wでも
// r/g/b/aでも書けます）。type は float / color / bool / vector。
// min と max を両方書くとスライダー、省略すると数値入力になります。
// default があると「既定値に戻す」で戻せます。
// 宣言を消しても動きます（その場合は生のfloat4を8本編集するUI）。

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
    // ここから下はエンジンが後から足した枠です。ObjectBufferの末尾に
    // 積んでいるだけなので、この行を書いていない自作Shaderもそのまま
    // 動きます（要る行まで宣言して、そこで止めて構いません）。
    float4 MaterialTextureParameters;  // PBRマップの有無
    float4 EmissiveParameters;         // 発光
    // 経過時間。x=秒（1時間で巻き戻る）、y=前フレームからの秒数、
    // z=フレーム数、w=予約。スクリプトを書かなくても、これで
    // 「揺れる・流れる・点滅する」が書けます。
    float4 TimeParameters;
};

cbuffer BoneBuffer : register(b2)
{
    float4x3 BoneTransforms[72];
};

Texture2D AlbedoTexture : register(t0);
Texture2D NormalTexture : register(t1);
Texture2DArray ShadowTexture : register(t2);
// t3〜t6もエンジンが使います（環境マップ、スポット影、ポイント影、
// 放射照度）。自作Shaderが自由に使える枠はt7〜t10です。宣言で
// { "target": "t7", "type": "texture", "name": "マスク" } と書くと、
// Inspectorから画像を割り当てられます（未設定の枠は白になります）。
Texture2D CustomTexture0 : register(t7);
Texture2D CustomTexture1 : register(t8);
Texture2D CustomTexture2 : register(t9);
Texture2D CustomTexture3 : register(t10);
SamplerState MaterialSampler : register(s0);
SamplerComparisonState ShadowSampler : register(s1);

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
    float2 TexCoord : TEXCOORD2;
};

struct SkinnedPixelInput
{
    float2 TexCoord : TEXCOORD0;
    float4 WorldPosition : TEXCOORD1;
    float3 WorldNormal : TEXCOORD2;
    float4 Diffuse : COLOR0;
    float4 Position : SV_Position;
};

struct OutlinePixelInput
{
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

PixelInput BuildPixelInput(
    float3 position,
    float3 normal,
    float2 texCoord)
{
    PixelInput output;
    const float4 worldPosition = mul(float4(position, 1.0f), World);
    output.Position = mul(worldPosition, ViewProjection);
    output.WorldPosition = worldPosition.xyz;
    output.WorldNormal = normalize(
        mul(float4(normal, 0.0f), WorldInverseTranspose).xyz);
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
    return BuildPixelInput(position, normal, input.TexCoord);
}

OutlinePixelInput VSSkinnedOutline(SkinnedVertexInput input)
{
    float3 position;
    float3 normal;
    SkinVertex(input, position, normal);
    position += normal * max(CustomParameters[3].x, 0.0f);
    OutlinePixelInput output;
    output.Position = mul(
        mul(float4(position, 1.0f), World),
        ViewProjection);
    output.TexCoord = input.TexCoord;
    return output;
}

float4 ShadeMaterial(PixelInput input)
{
    // パラメーター1: rgb = 色合い、a = 色合いの合成率（0..1）
    // パラメーター2: x = 発光量、y = リムライト量
    // パラメーター3: xy = UVスケール（0なら1を使用）、zw = UVオフセット
    float2 uvScale = CustomParameters[2].xy;
    uvScale = float2(
        abs(uvScale.x) < 0.0001f ? 1.0f : uvScale.x,
        abs(uvScale.y) < 0.0001f ? 1.0f : uvScale.y);
    const float2 uv = input.TexCoord * uvScale + CustomParameters[2].zw;
    const float4 albedo = AlbedoTexture.Sample(MaterialSampler, uv)
        * MaterialColor;
    clip(albedo.a - 0.08f);
    const float3 tint = lerp(
        float3(1.0f, 1.0f, 1.0f),
        CustomParameters[0].rgb,
        saturate(CustomParameters[0].a));
    const float3 viewDirection = normalize(
        CameraPosition.xyz - input.WorldPosition);
    const float rim = pow(
        1.0f - saturate(dot(normalize(input.WorldNormal), viewDirection)),
        3.0f) * max(CustomParameters[1].y, 0.0f);
    const float brightness = 1.0f
        + max(CustomParameters[1].x, 0.0f)
        + rim;
    return float4(albedo.rgb * tint * brightness, albedo.a);
}

float4 PSMain(PixelInput input) : SV_Target
{
    return ShadeMaterial(input);
}

float4 PSSkinnedMain(SkinnedPixelInput input) : SV_Target
{
    PixelInput pixel;
    pixel.Position = input.Position;
    pixel.WorldPosition = input.WorldPosition.xyz;
    pixel.WorldNormal = input.WorldNormal;
    pixel.TexCoord = input.TexCoord;
    return ShadeMaterial(pixel);
}

float4 PSOutline(OutlinePixelInput input) : SV_Target
{
    const float alpha =
        AlbedoTexture.Sample(MaterialSampler, input.TexCoord).a
        * MaterialColor.a;
    clip(alpha - 0.08f);
    return float4(saturate(CustomParameters[3].yzw), 1.0f);
}
