// カスタム材質の見本で、Shader Model 5.0のVSMain・PSMainを入口に保つ。

/* LAMAPON_RENDER_STATE
{ "blend": "opaque", "cull": "back", "depthWrite": true }
*/
// 非opaqueでdepthWriteを省略すると、透過の重ね合わせ用にfalseを適用する。

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
// targetは0～7の番号.成分または追加Texture枠で、min/max併記はスライダー・defaultは復元値に使う。

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
    // 着色・発光・輪郭・遮蔽設定
    float4 CustomParameters[8];
    // ObjectBuffer末尾の追加値は、使用する行まで同じ順序で宣言すればよい。
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
// 互換配置の平行光影画像
Texture2DArray ShadowTexture : register(t2);
// t3～t6はエンジン予約、t7～t10はInspectorの追加画像で未設定時は白を使う。
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
// 互換配置の影比較採取設定
SamplerComparisonState ShadowSampler : register(s1);

struct VertexInput
{
    // ローカル頂点位置
    float3 Position : SV_Position;
    // ローカル頂点法線
    float3 Normal : NORMAL;
    // 画像UV
    float2 TexCoord : TEXCOORD0;
};

struct InstancedVertexInput
{
    // ローカル頂点位置
    float3 Position : SV_Position;
    // ローカル頂点法線
    float3 Normal : NORMAL;
    // 画像UV
    float2 TexCoord : TEXCOORD0;
    // Instance World行列の第0行
    float4 InstanceWorld0 : INSTANCE_TRANSFORM0;
    // Instance World行列の第1行
    float4 InstanceWorld1 : INSTANCE_TRANSFORM1;
    // Instance World行列の第2行
    float4 InstanceWorld2 : INSTANCE_TRANSFORM2;
    // Instance World行列の第3行
    float4 InstanceWorld3 : INSTANCE_TRANSFORM3;
    // 互換入力のInstance色
    float4 InstanceColor : INSTANCE_COLOR0;
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

struct OutlinePixelInput
{
    // 輪郭頂点の画面位置
    float4 Position : SV_Position;
    // 輪郭の画像UV
    float2 TexCoord : TEXCOORD0;
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
    return BuildPixelInput(
        input.Position,
        input.Normal,
        input.TexCoord);
}

// InstanceのWorld行列で位置と余因子法線を変換する(input: Instance付き頂点)。
PixelInput VSInstancedMain(InstancedVertexInput input)
{
    // Instanceから組むWorld行列
    const float4x4 world = float4x4(
        input.InstanceWorld0,
        input.InstanceWorld1,
        input.InstanceWorld2,
        input.InstanceWorld3);
    // 余因子行列と行列式の符号で、非一様倍率や鏡像の法線方向を補正する。
    // World変換の線形成分
    const float3x3 basis = (float3x3)world;
    // 法線変換の余因子行列
    const float3x3 cofactor = float3x3(
        cross(basis[1], basis[2]),
        cross(basis[2], basis[0]),
        cross(basis[0], basis[1]));
    // World行列式の符号
    const float handedness =
        dot(basis[0], cofactor[0]) < 0.0f ? -1.0f : 1.0f;
    // World変換後のPixel入力
    PixelInput output;
    // World空間の同次位置
    const float4 worldPosition = mul(float4(input.Position, 1.0f), world);
    output.Position = mul(worldPosition, ViewProjection);
    output.WorldPosition = worldPosition.xyz;
    output.WorldNormal = normalize(
        mul(input.Normal, cofactor) * handedness);
    output.TexCoord = input.TexCoord;
    return output;
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

// 骨変形後の位置を法線方向へ広げて輪郭を作る(input: スキン頂点)。
OutlinePixelInput VSSkinnedOutline(SkinnedVertexInput input)
{
    // 骨変形後のローカル位置
    float3 position;
    // 骨変形後のローカル法線
    float3 normal;
    SkinVertex(input, position, normal);
    position += normal * max(CustomParameters[3].x, 0.0f);
    // World変換後のPixel入力
    OutlinePixelInput output;
    output.Position = mul(
        mul(float4(position, 1.0f), World),
        ViewProjection);
    output.TexCoord = input.TexCoord;
    return output;
}

// 表面画像へ着色・発光・リムを適用する(input: World位置・法線とUV)。
float4 ShadeMaterial(PixelInput input)
{
    // CustomParametersは0が着色RGBA、1が発光・リム、2がUV拡縮・移動、3が輪郭幅・色、4が遮蔽時RGBA。
    // 零に近い値を1とするUV倍率
    float2 uvScale = CustomParameters[2].xy;
    uvScale = float2(
        abs(uvScale.x) < 0.0001f ? 1.0f : uvScale.x,
        abs(uvScale.y) < 0.0001f ? 1.0f : uvScale.y);
    // 拡縮と移動後の採取UV
    const float2 uv = input.TexCoord * uvScale + CustomParameters[2].zw;
    // 画像と材質色を掛けたRGBA
    const float4 albedo = AlbedoTexture.Sample(MaterialSampler, uv)
        * MaterialColor;
    clip(albedo.a - 0.08f);
    // 強度で混合した着色RGB
    const float3 tint = lerp(
        float3(1.0f, 1.0f, 1.0f),
        CustomParameters[0].rgb,
        saturate(CustomParameters[0].a));
    // 表面から視点への単位方向
    const float3 viewDirection = normalize(
        CameraPosition.xyz - input.WorldPosition);
    // 視線角から求めたリム強度
    const float rim = pow(
        1.0f - saturate(dot(normalize(input.WorldNormal), viewDirection)),
        3.0f) * max(CustomParameters[1].y, 0.0f);
    // 発光とリムを足したRGB倍率
    const float brightness = 1.0f
        + max(CustomParameters[1].x, 0.0f)
        + rim;
    return float4(albedo.rgb * tint * brightness, albedo.a);
}

// 通常Meshへカスタム材質を適用する(input: 通常Pixel入力)。
float4 PSMain(PixelInput input) : SV_Target
{
    return ShadeMaterial(input);
}

// スキン入力を共通構造へ変換して材質を適用する(input: スキンPixel入力)。
float4 PSSkinnedMain(SkinnedPixelInput input) : SV_Target
{
    // 共通構造へ変換したPixel入力
    PixelInput pixel;
    pixel.Position = input.Position;
    pixel.WorldPosition = input.WorldPosition.xyz;
    pixel.WorldNormal = input.WorldNormal;
    pixel.TexCoord = input.TexCoord;
    return ShadeMaterial(pixel);
}

// 画像の輪郭を保って指定の輪郭色を返す(input: 輪郭位置とUV)。
float4 PSOutline(OutlinePixelInput input) : SV_Target
{
    // 輪郭表示を判定するAlpha
    const float alpha =
        AlbedoTexture.Sample(MaterialSampler, input.TexCoord).a
        * MaterialColor.a;
    clip(alpha - 0.08f);
    return float4(saturate(CustomParameters[3].yzw), 1.0f);
}

// 遮蔽時の指定RGBAを返す(input: 互換用の通常Pixel入力)。
float4 PSOccluded(PixelInput input) : SV_Target
{
    return float4(
        saturate(CustomParameters[4].xyz),
        saturate(CustomParameters[4].w));
}

// 遮蔽時の指定RGBAを返す(input: 互換用のスキンPixel入力)。
float4 PSSkinnedOccluded(SkinnedPixelInput input) : SV_Target
{
    return float4(
        saturate(CustomParameters[4].xyz),
        saturate(CustomParameters[4].w));
}
