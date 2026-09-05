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
    // PBRマップの有効フラグと遮蔽の強さ。
    // x=粗さマップ, y=金属度マップ, z=遮蔽マップ, w=遮蔽の強さ。
    // ObjectBufferの末尾なので、この行を持たない既存の自作Shaderも
    // そのまま動きます。
    float4 MaterialTextureParameters;
    // 発光。rgb=発光色（強度を掛け込んだ値）、w=発光マップの有無。
    // 末尾に配置し、この項目を持たない自作Shaderとの互換性を保ちます。
    float4 EmissiveParameters;
    // 経過時間。x=秒（1時間で巻き戻る）、y=前フレームからの秒数、
    // z=フレーム数、w=予約。エンジンが毎描画入れるので、揺れや流れは
    // スクリプト無しで書けます（LamaPonWater.hlslが使っています）。
    float4 TimeParameters;
};

Texture2D AlbedoTexture : register(t0);
Texture2D NormalTexture : register(t1);
Texture2DArray ShadowTexture : register(t2);
TextureCube EnvironmentMap : register(t3);
Texture2DArray SpotShadowTexture : register(t4);
TextureCube PointShadowTexture : register(t5);
// 事前フィルタ済みの拡散用放射照度キューブ（IBL）。
TextureCube IrradianceMap : register(t6);
// PBRマップ。t7〜t10はカスタムShader用の枠なので、エンジンの追加分は
// t11以降へ置いています（既存の自作Shaderを壊さないため）。
// 読むチャンネルはglTFのmetallicRoughness規約に合わせてG=粗さ、
// B=金属度です。FBXのように粗さ・金属度が別画像でも、グレースケール
// ならR=G=Bなので同じ読み方で一致します。
Texture2D RoughnessTexture : register(t11);
Texture2D MetallicTexture : register(t12);
// 遮蔽（AO）マップ。Rチャンネルを使い、環境光／IBLにだけ掛けます。
Texture2D OcclusionTexture : register(t13);
// 発光（emissive）マップ。ライティングとは独立に加算されるので、
// 暗い場所でも光ります。強く光らせるとBloomが自動で滲ませます。
Texture2D EmissiveTexture : register(t14);
// 画面空間の遮蔽（SSAO）。深度プリパスの後、ライティングより前に
// 用意されたものが入ります。画面全体で1枚なので、UVはピクセル座標
// （SV_Position）から作ります。半解像度ですが線形補間で読みます。
Texture2D ScreenAmbientOcclusionTexture : register(t15);

// クラスタライトカリング（Forward+）
// LamaPonLightCulling.hlslのCompute Shaderが作った、クラスタごとの
// ライト番号表です。有効なとき、ポイント／スポットはこの表の分だけ
// 計算します（定数バッファの16灯上限に縛られません）。
struct ClusterLight
{
    float4 PositionRange;
    float4 ColorIntensity;
    float4 DirectionInnerCosine;
    // x=外側cos, y=種別(0=点,1=スポット), z=影参照, w=予約。
    float4 ExtraParameters;
};
StructuredBuffer<ClusterLight> ClusterLights : register(t16);
StructuredBuffer<uint> ClusterLightIndexList : register(t17);
StructuredBuffer<uint> ClusterLightCounts : register(t18);
// 2個目のリフレクションプローブ（境界で映り込みが飛ぶのを防ぐため、
// 隣のプローブと混ぜるときだけ入ります）。t7〜t10は自作Shaderの
// カスタムテクスチャ枠なので、クラスタの後ろへ置いています。
TextureCube SecondaryEnvironmentMap : register(t19);
TextureCube SecondaryIrradianceMap : register(t20);
// SSR（画面空間反射）。t21は前フレームのHDRカラー、t22は深度
// プリパスの深度です。今描いている絵はまだ完成していないので読めず、
// 1フレーム前の絵を再投影して使います。
Texture2D ScreenReflectionColorTexture : register(t21);
Texture2D ScreenReflectionDepthTexture : register(t22);

// ベイクした間接光（照度ボリューム）。L1球面調和の係数を
// RGBチャンネル別に詰めたTexture3Dです。texelは
// (x係数, y係数, z係数, 定数項) で、dot(float4(法線,1), texel) が
// その場所・その向きの環境光になります。
Texture3D BakedGiRedTexture : register(t23);
Texture3D BakedGiGreenTexture : register(t24);
Texture3D BakedGiBlueTexture : register(t25);
SamplerState MaterialSampler : register(s0);
SamplerComparisonState ShadowSampler : register(s1);

static const float LamaPonPi = 3.14159265f;

struct DirectionalLight
{
    float4 DirectionIntensity;
    float4 Color;
};

struct PointLight
{
    float4 PositionRange;
    float4 ColorIntensity;
};

struct SpotLight
{
    float4 PositionRange;
    float4 DirectionInnerCosine;
    float4 ColorIntensity;
    float4 OuterCosinePadding;
};

cbuffer LightingBuffer : register(b1)
{
    float4 Ambient;
    uint4 LightCounts;
    DirectionalLight DirectionalLights[4];
    PointLight PointLights[16];
    SpotLight SpotLights[8];
    row_major float4x4 ShadowViewProjections[4];
    float4 ShadowCascadeSplits;
    float4 ShadowParameters;
    float4 FogColor;
    float4 FogParameters;
    float4 EnvironmentParameters;
    row_major float4x4 SpotShadowViewProjections[4];
    float4 SpotShadowParameters[4];
    float4 PointShadowParameters;
    // PCF用テクセルサイズ（x=カスケード, y=スポット, z=ポイント）。
    float4 ShadowTexelSizes;
    // 画面空間AO。x=1/画面幅, y=1/画面高さ, z=有効, w=予約。
    // 末尾に配置し、この項目を持たない自作Shaderとの互換性を保ちます。
    float4 ScreenAmbientOcclusionParameters;
    // クラスタライトカリング。x=横分割, y=縦分割, z=奥行き分割,
    // w=有効。こちらも末尾追加なので既存Shaderに影響しません。
    float4 ClusteredParameters;
    // x=near, y=far, z=log(far/near), w=クラスタあたり上限。
    float4 ClusteredDepthParameters;
    // x=1/画面幅, y=1/画面高さ, z=ライト総数, w=予約。
    float4 ClusteredScreenParameters;
    // リフレクションプローブのボックス射影。
    // xyz=箱の中心（ワールド）, w=予約。
    float4 ReflectionBoxCenter;
    // xyz=箱の半径（各軸）, w=有効。
    float4 ReflectionBoxParameters;
    // 2個目のプローブのボックス射影（同じ意味）。
    float4 ReflectionSecondaryBoxCenter;
    float4 ReflectionSecondaryBoxParameters;
    // x=2個目を混ぜる比率(0-1。0なら混ぜない),
    // y=2個目の最終ミップ番号, z/w=予約。
    float4 ReflectionBlendParameters;
    // SSR。x=強さ, y=有効, z=最大距離, w=サンプル数。
    float4 ScreenReflectionParameters;
    // x=1/画面幅, y=1/画面高さ, z=projection._33, w=projection._43。
    // zとwは深度をビュー空間のZへ戻すのに使います（SSAOと同じ）。
    float4 ScreenReflectionScreen;
    // x=物の厚み, y=粗さの上限, z/w=予約。
    float4 ScreenReflectionQuality;
    // 前フレームのビュー射影。当たった点を前フレームの画面座標へ
    // 戻すために使います（カメラが動いても位置がずれないように）。
    row_major float4x4 ScreenReflectionPreviousViewProjection;
    // ベイクした間接光。xyz=ボリュームの最小コーナー, w=有効。
    float4 BakedGiVolumeMinimum;
    // xyz=1/大きさ, w=強さ。
    float4 BakedGiInverseSize;
    // xyz=各軸のプローブ数, w=予約。
    float4 BakedGiResolution;
};

// GPUスキニング用のボーン行列（glTF/FBXモデル）。
// LitEffectをskinned=trueで作ったときだけ使われます。
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

// スキニングモデルの頂点。glTF/FBXインポーターが作る
// VertexPositionNormalTangentColorTextureSkinningと一致します。
// タンジェントは受け取りますが、ApplyNormalMapが画面空間微分から
// 接空間を作るため、法線マッピングには使いません。
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

// スキニング時のピクセルシェーダー入力。DirectXTKの
// per-pixel lighting頂点シェーダーの出力並びと一致させます
// （PSSkinnedMainの説明も参照）。
struct SkinnedPixelInput
{
    float2 TexCoord : TEXCOORD0;
    float4 WorldPosition : TEXCOORD1;
    float3 WorldNormal : TEXCOORD2;
    float4 Diffuse : COLOR0;
    float4 Position : SV_Position;
};

// インスタンス描画用：スロット1からワールド行列と色を受け取ります。
struct InstancedVertexInput
{
    float3 Position : SV_Position;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD0;
    float4 InstanceWorld0 : INSTANCE_TRANSFORM0;
    float4 InstanceWorld1 : INSTANCE_TRANSFORM1;
    float4 InstanceWorld2 : INSTANCE_TRANSFORM2;
    float4 InstanceWorld3 : INSTANCE_TRANSFORM3;
    float4 InstanceColor : INSTANCE_COLOR0;
};

struct PixelInput
{
    float4 Position : SV_Position;
    float3 WorldPosition : TEXCOORD0;
    float3 WorldNormal : TEXCOORD1;
    float2 TexCoord : TEXCOORD2;
    float4 Tint : COLOR0;
};

PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    const float4 worldPosition = mul(float4(input.Position, 1.0f), World);
    output.Position = mul(worldPosition, ViewProjection);
    output.WorldPosition = worldPosition.xyz;
    output.WorldNormal = normalize(
        mul(float4(input.Normal, 0.0f), WorldInverseTranspose).xyz);
    output.TexCoord = input.TexCoord;
    output.Tint = MaterialColor;
    return output;
}

PixelInput VSInstancedMain(InstancedVertexInput input)
{
    PixelInput output;
    const float4x4 world = float4x4(
        input.InstanceWorld0,
        input.InstanceWorld1,
        input.InstanceWorld2,
        input.InstanceWorld3);
    const float4 worldPosition =
        mul(float4(input.Position, 1.0f), world);
    output.Position = mul(worldPosition, ViewProjection);
    output.WorldPosition = worldPosition.xyz;
    // 逆転置の代わりに正規化で近似します（非一様スケールでは
    // 法線に誤差が出ます）。
    output.WorldNormal = normalize(
        mul(float4(input.Normal, 0.0f), world).xyz);
    output.TexCoord = input.TexCoord;
    output.Tint = input.InstanceColor;
    return output;
}

// ボーン4本の線形ブレンドスキニング。位置と法線をモデル空間で
// 変形してから、通常どおりWorldでワールド空間へ移します。
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
    float3 skinnedPosition;
    float3 skinnedNormal;
    SkinVertex(input, skinnedPosition, skinnedNormal);

    PixelInput output;
    const float4 worldPosition =
        mul(float4(skinnedPosition, 1.0f), World);
    output.Position = mul(worldPosition, ViewProjection);
    output.WorldPosition = worldPosition.xyz;
    output.WorldNormal = normalize(
        mul(float4(skinnedNormal, 0.0f), WorldInverseTranspose).xyz);
    output.TexCoord = input.TexCoord;
    output.Tint = MaterialColor;
    return output;
}

float3 ApplyNormalMap(PixelInput input, float3 geometricNormal)
{
    if (MaterialParameters.z < 0.5f)
    {
        return geometricNormal;
    }

    // Zはサンプルせずxyから復元します。法線マップはBC5（RGの2
    // チャンネルのみ、Bは0が返る）で読み込まれることがあるためで、
    // 非圧縮のRGBでも単位ベクトルなら同じ値になります。強さを
    // 掛けた後に復元するので、傾けても長さが1に保たれます。
    float3 mappedNormal;
    mappedNormal.xy =
        NormalTexture.Sample(MaterialSampler, input.TexCoord).xy
        * 2.0f
        - 1.0f;
    mappedNormal.xy *= MaterialParameters.y;
    mappedNormal.z = sqrt(
        saturate(1.0f - dot(mappedNormal.xy, mappedNormal.xy)));

    const float3 positionDerivativeX = ddx(input.WorldPosition);
    const float3 positionDerivativeY = ddy(input.WorldPosition);
    const float2 uvDerivativeX = ddx(input.TexCoord);
    const float2 uvDerivativeY = ddy(input.TexCoord);
    const float3 perpendicularY =
        cross(positionDerivativeY, geometricNormal);
    const float3 perpendicularX =
        cross(geometricNormal, positionDerivativeX);
    const float3 tangent =
        perpendicularY * uvDerivativeX.x
        + perpendicularX * uvDerivativeY.x;
    const float3 bitangent =
        perpendicularY * uvDerivativeX.y
        + perpendicularX * uvDerivativeY.y;
    const float scale = rsqrt(max(
        max(dot(tangent, tangent), dot(bitangent, bitangent)),
        0.000001f));
    return normalize(
        tangent * (mappedNormal.x * scale)
        + bitangent * (mappedNormal.y * scale)
        + geometricNormal * mappedNormal.z);
}

// 太陽（や電球）の「見かけの大きさ」を反射に反映するための代表点。
//
// 光源を点として扱うと、つるつるの物体のハイライトは数学的な点に
// なり、1画素より小さくなって消えたように見えます。実際の太陽は
// 空に0.53度の円盤として見えていて、水面や金属のハイライトはその
// 円盤の像です。
//
// 円盤を積分する代わりに、反射の向きが円盤から外れているときだけ
// 円盤の縁の一番近い点へ寄せます（代表点法）。円盤の中を見ている
// 間は反射ベクトルがそのまま使われるので、ハイライトは「点」では
// なく「円盤の見た目の大きさ」に広がります。
float3 SourceRepresentativeDirection(
    float3 toLight,
    float3 normal,
    float3 viewDirection,
    float angularRadius)
{
    if (angularRadius <= 0.0f)
    {
        return toLight;
    }
    const float3 reflected = reflect(-viewDirection, normal);
    const float alignment = dot(toLight, reflected);
    const float diskCosine = cos(angularRadius);
    if (alignment >= diskCosine)
    {
        // 反射の向きが円盤の中を向いている＝そのまま鏡で見える。
        return reflected;
    }
    const float3 sideways = reflected - alignment * toLight;
    const float sidewaysLength = length(sideways);
    if (sidewaysLength <= 1.0e-5f)
    {
        return toLight;
    }
    return normalize(
        toLight * diskCosine
        + (sideways / sidewaysLength) * sin(angularRadius));
}

// 代表点へ寄せたぶん、ハイライトの峰が広い範囲で最大値のままに
// なって明るくなりすぎます。円盤の広がりを粗さへ足した値との比で
// 割って戻します（Karisのsphere light正規化と同じ考え方）。
float SourceSpecularEnergy(
    float roughness,
    float angularRadius)
{
    if (angularRadius <= 0.0f)
    {
        return 1.0f;
    }
    const float alpha = max(roughness * roughness, 1.0e-4f);
    const float widened =
        saturate(alpha + sin(angularRadius) * 0.5f);
    const float ratio = alpha / max(widened, 1.0e-4f);
    return ratio * ratio;
}

// Cook-Torrance GGXによる直接光の寄与。
// radianceはライト色×強度×減衰×影を掛けた値。
//
// specularToLightは鏡面反射だけに使う向きです（光源の見かけの
// 大きさを反映した代表点）。拡散反射と陰りの判定は、代表点では
// なく本当の光の向き（toLight）で行います。
float3 EvaluateLightPbrSized(
    float3 normal,
    float3 toLight,
    float3 specularToLight,
    float3 viewDirection,
    float3 albedo,
    float roughness,
    float metallic,
    float3 radiance,
    float specularEnergy)
{
    const float normalDotLight =
        saturate(dot(normal, toLight));
    if (normalDotLight <= 0.0f)
    {
        return 0.0f.xxx;
    }
    const float3 halfVector =
        normalize(specularToLight + viewDirection);
    const float normalDotView = max(
        dot(normal, viewDirection),
        0.0001f);
    const float normalDotHalf =
        saturate(dot(normal, halfVector));
    const float viewDotHalf =
        saturate(dot(viewDirection, halfVector));

    const float alpha = roughness * roughness;
    const float alphaSquared = alpha * alpha;

    // 法線分布（GGX）
    const float denominator =
        normalDotHalf * normalDotHalf
            * (alphaSquared - 1.0f)
        + 1.0f;
    // 0除算を防ぎながら鋭いハイライトを保つため、粗さの下限0.04で
    // 生じる分母より十分に小さい値を使用します。
    const float distribution =
        alphaSquared
        / max(LamaPonPi * denominator * denominator,
            1.0e-12f);

    // 幾何減衰（Smith-Schlick近似）
    const float k = alpha * 0.5f + 0.0001f;
    const float geometryView =
        normalDotView / (normalDotView * (1.0f - k) + k);
    const float geometryLight =
        normalDotLight
        / (normalDotLight * (1.0f - k) + k);
    const float geometry = geometryView * geometryLight;

    // フレネル（Schlick）
    const float3 f0 = lerp(0.04f.xxx, albedo, metallic);
    const float3 fresnel =
        f0
        + (1.0f.xxx - f0)
            * pow(1.0f - viewDotHalf, 5.0f);

    const float3 specular =
        distribution * geometry * fresnel
        * specularEnergy
        / max(4.0f * normalDotView * normalDotLight,
            0.0001f);
    const float3 diffuse =
        (1.0f.xxx - fresnel)
        * (1.0f - metallic)
        * albedo
        / LamaPonPi;
    return (diffuse + specular)
        * radiance
        * normalDotLight;
}

// 見かけの大きさを持たない光源（ポイント／スポット）用の入口。
// 従来どおり、光の向きをそのまま鏡面にも使います。
float3 EvaluateLightPbr(
    float3 normal,
    float3 toLight,
    float3 viewDirection,
    float3 albedo,
    float roughness,
    float metallic,
    float3 radiance)
{
    return EvaluateLightPbrSized(
        normal,
        toLight,
        toLight,
        viewDirection,
        albedo,
        roughness,
        metallic,
        radiance,
        1.0f);
}

// キューブマップ環境光（IBL）。無効時は従来のフラット環境光。
// リフレクションプローブのボックス射影。
//
// キューブマップは「無限遠の景色」として作られているので、反射
// ベクトルをそのまま当てると、部屋の壁が無限に遠くにあるように
// 映ります（動いても壁の映り込みが動かない）。プローブを箱と
// みなして反射レイと箱の交点を求め、その点への方向でサンプル
// すると、壁・床・天井が正しい距離で映ります。
//
// boxParameters.w が0のときは補正しません（箱の指定なし）。
float3 ApplyBoxProjection(
    float3 reflection,
    float3 worldPosition,
    float3 boxCenter,
    float3 boxExtents)
{
    // レイと軸平行の箱（AABB）の交差。成分ごとに「箱の面へ届く
    // までの距離」を求め、一番手前の面を採用します。
    const float3 firstPlane =
        (boxCenter + boxExtents - worldPosition)
        / reflection;
    const float3 secondPlane =
        (boxCenter - boxExtents - worldPosition)
        / reflection;
    const float3 furthest =
        max(firstPlane, secondPlane);
    const float distance = min(
        min(furthest.x, furthest.y),
        furthest.z);
    // 交点への方向。プローブ中心から見た向きにするのが要点で、
    // これでキューブマップの向きと一致します。
    const float3 intersection =
        worldPosition + reflection * distance;
    return intersection - boxCenter;
}

// プローブ1個ぶんのスペキュラ。反射ベクトルはプローブごとに
// 自分の箱で補正します（混ぜる2個が別の部屋にいても正しい）。
float3 SampleProbeSpecular(
    TextureCube probeMap,
    float3 normal,
    float3 viewDirection,
    float3 worldPosition,
    float roughness,
    float maximumMip,
    float4 boxCenter,
    float4 boxParameters)
{
    float3 reflection = reflect(-viewDirection, normal);
    if (boxParameters.w >= 0.5f)
    {
        reflection = ApplyBoxProjection(
            reflection,
            worldPosition,
            boxCenter.xyz,
            boxParameters.xyz);
    }
    return probeMap.SampleLevel(
        MaterialSampler,
        reflection,
        roughness * maximumMip).rgb;
}

// 場所ごとに変化する環境光（Ambient）。
//
// ボリューム内ではベイクした間接光を返し、範囲外や無効時は通常の
// 環境光を返します。範囲の縁では滑らかに混ぜ、境界線を隠します。
float3 EvaluateBakedAmbient(
    float3 worldPosition,
    float3 normal)
{
    if (BakedGiVolumeMinimum.w < 0.5f)
    {
        return Ambient.rgb;
    }
    const float3 volumeUvw =
        (worldPosition - BakedGiVolumeMinimum.xyz)
        * BakedGiInverseSize.xyz;

    // プローブは格子の角にあるので、テクスチャ座標へは
    // 「テクセル中心」へ寄せて変換します（寄せないと端の
    // プローブが半セルぶん内側にあるように見えます）。
    const float3 resolution = BakedGiResolution.xyz;
    const float3 texelUvw =
        (volumeUvw * (resolution - 1.0f) + 0.5f)
        / resolution;

    // L1球面調和の評価。texel = (x, y, z, 定数項)。
    const float4 basis = float4(normal, 1.0f);
    float3 gi;
    gi.r = dot(
        basis,
        BakedGiRedTexture.SampleLevel(
            MaterialSampler, texelUvw, 0.0f));
    gi.g = dot(
        basis,
        BakedGiGreenTexture.SampleLevel(
            MaterialSampler, texelUvw, 0.0f));
    gi.b = dot(
        basis,
        BakedGiBlueTexture.SampleLevel(
            MaterialSampler, texelUvw, 0.0f));
    // L1の再構成は強い明暗差で負へ振れることがあります。
    gi = max(gi, 0.0f.xxx) * BakedGiInverseSize.w;

    // 縁のフェード。ボリュームの5%の帯で従来のAmbientへ戻します。
    const float3 edge =
        (0.5f - abs(volumeUvw - 0.5f)) / 0.05f;
    const float weight = saturate(
        min(min(edge.x, edge.y), edge.z));
    return lerp(Ambient.rgb, gi, weight);
}

// t22はHi-Z深度ピラミッドです。深度→距離の変換はピラミッドを
// 作るとき（PSReflectionDepthLinearize）に済んでいるので、ここは
// 読むだけです。ミップNは「そのミップの区画で最も手前の距離」
// （2x2の最小値）を持っています。
//
// 点（Load）で読みます。バイリニアで読むと、輪郭をまたいだ
// ところで「手前と奥の中間」という存在しない距離が出て、そこに
// 偽の当たりが生まれます。
float ScreenReflectionSceneDistance(float2 uv)
{
    const float2 screenSize =
        1.0f / max(ScreenReflectionScreen.xy, 1e-6f);
    const int2 lastPixel = max(int2(screenSize) - 1, int2(0, 0));
    const int2 pixel = clamp(
        int2(saturate(uv) * screenSize),
        int2(0, 0),
        lastPixel);
    return ScreenReflectionDepthTexture.Load(
        int3(pixel, 0)).r;
}

// SSR（画面空間反射）。
//
// 反射レイを画面空間のHi-Zトラバーサルで進めます。深度ピラミッド
// （t22。各ミップが「その区画で最も手前の距離」＝2x2の最小値）を
// 使い、
//   ・「区画の最も手前より、レイの区間全体が手前」なら、その区画に
//     当たりは存在しない → 区画の出口まで一気に進み、1段粗い
//     ミップへ上がる（何も無い空間を大股で飛ぶ）
//   ・またぐかもしれないなら、進まずに1段細かいミップへ下りる
//   ・最細ミップ（＝1画素）でまたいだら、その区間を二分で詰める
// これで歩数は「画面の距離ぶん」ではなく「およそlog2(距離)」で済み、
// 同じ反復上限で画面の端から端まで到達します（1画素ずつのDDAは
// 上限128歩＝128画素で頭打ちでした）。
//
// 画面空間で進めること自体の利点は従来と同じです。ワールド等間隔
// だと近くで画素を飛び越し、遠くで同じ画素を何度も読みます。判定は
// 「点」ではなく「区間」で行うので、歩を飛び越して奥へ抜けることが
// 原理的に起きません。
//
// 返り値のaは信頼度です。画面の外へ出た／当たらなかった／粗すぎる
// ときは0になり、呼ぶ側は環境反射（プローブやSky）へ戻します。
// 画面に写っていないものは映せないので、0へ滑らかに落とすことが
// 品質の要点になります（急に切れると縁が目立ちます）。
//
// 信頼度を落とす4条件は、画面空間に情報がない領域を
// 環境反射へ渡すための固定値です。
//   (1)画面の縁に近い（今のフレームと前フレームの両方で見ます）
//   (2)レイが最大距離の近くまで進んだ
//   (3)反射がカメラへ向かっている
//   (4)粗さが上限に近い
float4 EvaluateScreenSpaceReflection(
    float3 worldPosition,
    float3 reflection,
    float3 viewDirection,
    float roughness)
{
    if (ScreenReflectionParameters.y < 0.5f)
    {
        return 0.0f;
    }
    // ざらざらした面の反射はぼやけていて、1本のレイでは表せません。
    const float roughnessCutoff = max(
        ScreenReflectionQuality.y,
        0.0001f);
    if (roughness >= roughnessCutoff)
    {
        return 0.0f;
    }

    const float maximumDistance = max(
        ScreenReflectionParameters.z,
        0.01f);
    const float thickness = max(
        ScreenReflectionQuality.x,
        0.001f);
    // 設定の「サンプル数」は、Hi-Zでは反復の上限として働きます。
    // 1反復は「区画を1つ飛ぶ／ミップを1段動く」で、何も無い空間は
    // 大股で越えるため、既定の24でも画面の端から端まで届きます。
    const int maximumSteps = clamp(
        (int)ScreenReflectionParameters.w,
        4,
        128);

    // レイの両端をクリップ空間へ。透視射影ではwがそのまま
    // カメラからの距離になります（右手系なのでw = -z_view）。
    const float3 rayStart = worldPosition;
    float3 rayEnd = worldPosition + reflection * maximumDistance;
    float4 clipStart = mul(
        float4(rayStart, 1.0f),
        ViewProjection);
    float4 clipEnd = mul(float4(rayEnd, 1.0f), ViewProjection);

    // カメラより手前へ回った側は射影が破綻するので、世界空間で
    // 詰めます。画面座標にしてから直そうとしても、符号が反転した
    // 座標からは戻せません。
    const float nearW = 0.05f;
    if (clipStart.w <= nearW)
    {
        return 0.0f;
    }
    if (clipEnd.w <= nearW)
    {
        const float clipRatio =
            (nearW - clipStart.w)
            / (clipEnd.w - clipStart.w);
        rayEnd = lerp(rayStart, rayEnd, saturate(clipRatio));
        clipEnd = mul(float4(rayEnd, 1.0f), ViewProjection);
    }

    const float2 startUv = float2(
        clipStart.x / clipStart.w * 0.5f + 0.5f,
        0.5f - clipStart.y / clipStart.w * 0.5f);
    const float2 endUv = float2(
        clipEnd.x / clipEnd.w * 0.5f + 0.5f,
        0.5f - clipEnd.y / clipEnd.w * 0.5f);
    const float2 deltaUv = endUv - startUv;

    // 画面外には参照できる情報がないため、レイを画面端で打ち切ります。
    float limitAlpha = 1.0f;
    [unroll]
    for (int axis = 0; axis < 2; ++axis)
    {
        const float direction = axis == 0
            ? deltaUv.x
            : deltaUv.y;
        const float origin = axis == 0
            ? startUv.x
            : startUv.y;
        if (abs(direction) > 1e-6f)
        {
            // 進む向き側の辺までの比率。反対側の辺は負になるので、
            // 大きいほうを採ります。
            const float exitAlpha = max(
                (0.0f - origin) / direction,
                (1.0f - origin) / direction);
            if (exitAlpha > 0.0f)
            {
                limitAlpha = min(limitAlpha, exitAlpha);
            }
        }
    }
    limitAlpha = clamp(limitAlpha, 0.0f, 1.0f);

    const float2 screenSize =
        1.0f / max(ScreenReflectionScreen.xy, 1e-6f);
    // 自己ヒットよけに、出発点を半画素ずらします。
    const float2 pixelDelta =
        deltaUv * limitAlpha * screenSize;
    const float pixelLength = max(
        max(abs(pixelDelta.x), abs(pixelDelta.y)),
        1.0f);

    // 1/wは画面空間で線形なので、行列を掛け直さずに補間で距離が出ます。
    // これで歩ごとのmulが消え、詰めるところも補間だけで済みます。
    const float inverseStartW = 1.0f / clipStart.w;
    const float inverseEndW = 1.0f / clipEnd.w;

    // Hi-Zピラミッドの最終ミップ番号（cbuffer経由。0ならミップ無し
    // ＝実質1画素ずつのDDAに落ちます）。
    const int maximumLevel = max(
        (int)ScreenReflectionQuality.z,
        0);

    float alpha = 0.5f * limitAlpha / pixelLength;
    // 前進の最小量。区画の辺の上に立ったとき、浮動小数の丸めで
    // 同じ区画を永遠に再訪しないための保険です（値は最細ミップの
    // 1画素よりずっと小さいので、取りこぼしにはなりません）。
    const float alphaBias = limitAlpha * 1e-5f;
    int level = 0;

    [loop]
    for (int step = 0; step < maximumSteps; ++step)
    {
        if (alpha >= limitAlpha)
        {
            break;
        }
        const float2 uv = startUv + deltaUv * alpha;
        // このミップでの「今いる区画」と、その出口までのα。
        const float2 levelSize = max(
            floor(screenSize / exp2((float)level)),
            1.0f);
        const float2 cell = floor(
            clamp(uv, 0.0f, 1.0f) * levelSize);
        const float2 towardEdge = float2(
            deltaUv.x >= 0.0f ? 1.0f : 0.0f,
            deltaUv.y >= 0.0f ? 1.0f : 0.0f);
        const float2 boundaryUv =
            (cell + towardEdge) / levelSize;
        float2 boundaryAlpha = float2(1e9f, 1e9f);
        if (abs(deltaUv.x) > 1e-8f)
        {
            boundaryAlpha.x =
                (boundaryUv.x - startUv.x) / deltaUv.x;
        }
        if (abs(deltaUv.y) > 1e-8f)
        {
            boundaryAlpha.y =
                (boundaryUv.y - startUv.y) / deltaUv.y;
        }
        const float exitAlpha = max(
            min(boundaryAlpha.x, boundaryAlpha.y),
            alpha + alphaBias);
        const float clampedExitAlpha = min(
            exitAlpha,
            limitAlpha);

        // この区画を通るあいだの、レイの距離の範囲。
        const float entryDistance = 1.0f / max(
            lerp(inverseStartW, inverseEndW, alpha),
            1e-6f);
        const float exitDistance = 1.0f / max(
            lerp(
                inverseStartW,
                inverseEndW,
                clampedExitAlpha),
            1e-6f);
        const float rayNear = min(
            entryDistance,
            exitDistance);
        const float rayFar = max(
            entryDistance,
            exitDistance);

        // この区画で最も手前の面。
        const float sceneDistance =
            ScreenReflectionDepthTexture.Load(int3(
                int2(min(cell, levelSize - 1.0f)),
                level)).r;

        if (rayFar <= sceneDistance)
        {
            // 区間全体が最も手前の面よりさらに手前 → この区画に
            // 当たりは無い。出口まで飛びます。
            alpha = exitAlpha;
            // 面から2%以上離れた場合だけ粗いミップへ移り、面の近くで
            // ミップを往復して反復回数を消費することを防ぎます。
            if (rayFar * 1.02f <= sceneDistance)
            {
                level = min(level + 1, maximumLevel);
            }
            continue;
        }
        if (level > 0)
        {
            // またぐかもしれない。進まずに1段細かく見る。
            level = level - 1;
            continue;
        }

        // 最細ミップ（1画素）。またいだ区間だけを精査します。
        if (rayFar > sceneDistance
            && rayNear < sceneDistance + thickness)
        {
            // 当たった区間を二分して詰めます。刻みのままだと当たり位置が
            // 歩の単位に量子化されて反射に縞が出ます。ここは補間だけ
            // なので、4回でも行列を掛けません。
            float nearAlpha = alpha;
            float farAlpha = clampedExitAlpha;
            [unroll]
            for (int refine = 0; refine < 4; ++refine)
            {
                const float middleAlpha =
                    (nearAlpha + farAlpha) * 0.5f;
                const float middleDistance = 1.0f / max(
                    lerp(
                        inverseStartW,
                        inverseEndW,
                        middleAlpha),
                    1e-6f);
                const float middleScene =
                    ScreenReflectionSceneDistance(
                        startUv + deltaUv * middleAlpha);
                if (middleDistance > middleScene)
                {
                    // まだ面の裏。手前側を詰めます。
                    farAlpha = middleAlpha;
                }
                else
                {
                    nearAlpha = middleAlpha;
                }
            }
            const float hitAlpha =
                (nearAlpha + farAlpha) * 0.5f;
            const float2 hitUv = startUv + deltaUv * hitAlpha;

            // 画面空間の比率αを、ワールド空間の比率へ直します。
            // 透視補間の関係 t = α*w0 / lerp(w1, w0, α) です。距離
            // フェードと当たり位置の復元に要ります。
            const float worldRatio =
                hitAlpha * clipStart.w
                / max(
                    lerp(clipEnd.w, clipStart.w, hitAlpha),
                    1e-6f);
            const float3 hitPosition = lerp(
                rayStart,
                rayEnd,
                saturate(worldRatio));

            // 当たり。前フレームの画面座標へ戻して色を読みます。
            const float4 previousClip = mul(
                float4(hitPosition, 1.0f),
                ScreenReflectionPreviousViewProjection);
            if (previousClip.w <= 0.0001f)
            {
                return 0.0f;
            }
            const float2 previousUv = float2(
                previousClip.x / previousClip.w * 0.5f + 0.5f,
                0.5f - previousClip.y / previousClip.w * 0.5f);
            if (previousUv.x < 0.0f || previousUv.x > 1.0f
                || previousUv.y < 0.0f
                || previousUv.y > 1.0f)
            {
                return 0.0f;
            }

            // (1)画面の縁へ近いほど弱めます。縁で急に消えると、
            // 反射が四角く切り取られて見えるためです。
            //
            // 現在と前のフレームの両方の位置で確認します。前
            // フレームだけだと、カメラが大きく動いたときに「今は画面の
            // 端ぎりぎりだが前フレームでは中央だった」当たりが全強度で
            // 返り、次のフレームで画面の外に出て消えます。
            const float2 currentEdge = min(hitUv, 1.0f - hitUv);
            const float2 previousEdge =
                min(previousUv, 1.0f - previousUv);
            const float edgeDistance = min(
                min(currentEdge.x, currentEdge.y),
                min(previousEdge.x, previousEdge.y));
            const float edgeFade = saturate(
                edgeDistance / 0.08f);

            // (2)レイが進んだ距離で弱めます。最大距離のところで急に
            // 途切れると、カメラが少し動くだけで反射が現れたり消えたり
            // します（12mで切っているとき、11.9mで当たれば全強度、
            // 12.1mになった瞬間に0）。最後の1/4で滑らかに落とします。
            //
            // 全区間を通して線形に落とすやり方は採りません。すぐ隣の
            // ものの映り込みまで薄くなり、いちばん見せたい足元の反射が
            // 弱くなります。
            const float travelled = saturate(worldRatio)
                * length(rayEnd - rayStart);
            const float travelledFraction = saturate(
                travelled / maximumDistance);
            const float distanceFade = saturate(
                (1.0f - travelledFraction) / 0.25f);

            // (3)反射がカメラへ向かっているほど弱めます。
            //
            // 画面空間には「物の裏側」の情報がありません。反射が
            // カメラの方へ戻ってくる向きのとき、当たった先で読める色は
            // その物の手前の面で、本来映るべき裏の面ではありません。
            // ここは原理的に正しくできないので、素直に環境反射へ
            // 譲ります。viewDirectionは面からカメラへ向かう向きなので、
            // 内積が1に近いほどまっすぐカメラへ戻っています。
            const float towardCamera = saturate(
                dot(reflection, viewDirection));
            const float directionFade = saturate(
                (1.0f - towardCamera) / 0.5f);

            // (4)粗さが上限に近いほど弱めます。
            const float roughnessFade = saturate(
                1.0f - roughness / roughnessCutoff);
            const float3 color =
                ScreenReflectionColorTexture.SampleLevel(
                    MaterialSampler,
                    previousUv,
                    0.0f).rgb;
            return float4(
                color,
                edgeFade
                    * distanceFade
                    * directionFade
                    * roughnessFade
                    * saturate(
                        ScreenReflectionParameters.x));
        }

        // 面の裏を（厚みの外で）通り過ぎた。次の区画へ進みます。
        // 面の裏側では粗いミップでも区間を省略できないため、現在の
        // ミップを維持します。
        alpha = exitAlpha;
    }
    return 0.0f;
}

float3 EvaluateEnvironment(
    float3 normal,
    float3 viewDirection,
    float3 worldPosition,
    float3 albedo,
    float roughness,
    float metallic)
{
    // SSR（画面空間反射）。画面に写っているものが当たれば、その色を
    // 環境反射の上へ被せます。信頼度が0のところ（画面の外、当たら
    // なかった、粗すぎる）はそのまま環境反射が残るので、映せない
    // 部分が黒く抜けることはありません。
    //
    // 反射ベクトルはボックス射影より前のものを使います。ボックス
    // 射影はキューブマップを引くための補正なので、実際の空間を
    // 進むレイに掛けると当たる場所がずれます。
    const float4 screenReflection =
        EvaluateScreenSpaceReflection(
            worldPosition,
            reflect(-viewDirection, normal),
            viewDirection,
            roughness);

    if (EnvironmentParameters.y < 0.5f)
    {
        float3 flatAmbient =
            EvaluateBakedAmbient(worldPosition, normal)
            * albedo
            * (1.0f - metallic * 0.5f);
        // キューブマップの環境反射が無いシーンでも、SSRが当たった
        // 分は映します。split-sumのBRDFは下の経路にしか無いので、
        // ここはF0相当（誘電体0.04、金属はアルベド）の重みで
        // 足すだけにしています。
        if (screenReflection.a > 0.0f)
        {
            const float3 fresnelZero =
                lerp(0.04f.xxx, albedo, metallic);
            flatAmbient +=
                screenReflection.rgb
                * fresnelZero
                * screenReflection.a;
        }
        return flatAmbient;
    }

    // zに事前フィルタ済みスペキュラの最終ミップ番号が入ります。
    // 0のときは事前フィルタなし（ソース直接）の近似経路です。
    const float prefilteredMaximumMip =
        EnvironmentParameters.z;
    float maximumMip = prefilteredMaximumMip;
    if (maximumMip <= 0.0f)
    {
        uint width;
        uint height;
        uint mipCount;
        EnvironmentMap.GetDimensions(
            0,
            width,
            height,
            mipCount);
        maximumMip = max((float)mipCount - 1.0f, 0.0f);
    }

    // 拡散：事前フィルタ済みならコサイン畳み込みの放射照度
    // マップ、なければ最粗ミップで近似します。
    float3 irradiance =
        prefilteredMaximumMip > 0.0f
            ? IrradianceMap.SampleLevel(
                MaterialSampler,
                normal,
                0.0f).rgb
            : EnvironmentMap.SampleLevel(
                MaterialSampler,
                normal,
                maximumMip).rgb;

    // スペキュラ：粗さに応じたミップの事前畳み込み結果。
    float3 prefiltered = SampleProbeSpecular(
        EnvironmentMap,
        normal,
        viewDirection,
        worldPosition,
        roughness,
        maximumMip,
        ReflectionBoxCenter,
        ReflectionBoxParameters);

    // リフレクションプローブが2個あるときは重みで混ぜます。
    // 比率が0のフレームではこの中へ入らないので、プローブ1個の
    // ときの結果は1ビットも変わりません。
    const float blendWeight = ReflectionBlendParameters.x;
    if (blendWeight > 0.0f)
    {
        const float secondaryMaximumMip =
            ReflectionBlendParameters.y;
        irradiance = lerp(
            irradiance,
            SecondaryIrradianceMap.SampleLevel(
                MaterialSampler,
                normal,
                0.0f).rgb,
            blendWeight);
        prefiltered = lerp(
            prefiltered,
            SampleProbeSpecular(
                SecondaryEnvironmentMap,
                normal,
                viewDirection,
                worldPosition,
                roughness,
                secondaryMaximumMip,
                ReflectionSecondaryBoxCenter,
                ReflectionSecondaryBoxParameters),
            blendWeight);
    }

    // SSRは最後に被せます（プローブのブレンドの上）。
    if (screenReflection.a > 0.0f)
    {
        prefiltered = lerp(
            prefiltered,
            screenReflection.rgb,
            screenReflection.a);
    }

    const float normalDotView = max(
        dot(normal, viewDirection),
        0.0001f);
    const float3 f0 = lerp(0.04f.xxx, albedo, metallic);

    // split-sumのBRDF項はKarisの解析近似で評価します
    // （LUT不要のEnvBRDFApprox）。
    const float4 c0 = float4(
        -1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 c1 = float4(
        1.0f, 0.0425f, 1.04f, -0.04f);
    const float4 r = roughness * c0 + c1;
    const float a004 =
        min(r.x * r.x, exp2(-9.28f * normalDotView))
            * r.x
        + r.y;
    const float2 brdf =
        float2(-1.04f, 1.04f) * a004 + r.zw;

    const float3 diffuse =
        irradiance * albedo * (1.0f - metallic);
    const float3 specular =
        prefiltered * (f0 * brdf.x + brdf.y);
    return (diffuse + specular)
        * EnvironmentParameters.x
        + EvaluateBakedAmbient(worldPosition, normal)
            * albedo;
}

float SampleDirectionalShadowCascade(
    float3 worldPosition,
    float3 normal,
    uint cascadeIndex)
{
    const float3 biasedPosition =
        worldPosition
        + normal * ShadowParameters.z;
    const float4 lightPosition = mul(
        float4(biasedPosition, 1.0f),
        ShadowViewProjections[cascadeIndex]);
    const float3 projected =
        lightPosition.xyz
        / max(abs(lightPosition.w), 0.00001f);
    const float2 shadowUv =
        projected.xy * float2(0.5f, -0.5f)
        + 0.5f;
    if (shadowUv.x < 0.0f
        || shadowUv.x > 1.0f
        || shadowUv.y < 0.0f
        || shadowUv.y > 1.0f
        || projected.z <= 0.0f
        || projected.z >= 1.0f)
    {
        return 1.0f;
    }

    // 3x3 PCFで影の輪郭を柔らかくします。
    float visibility = 0.0f;
    const float cascadeTexel = ShadowTexelSizes.x;
    [unroll]
    for (int tapY = -1; tapY <= 1; ++tapY)
    {
        [unroll]
        for (int tapX = -1; tapX <= 1; ++tapX)
        {
            visibility +=
                ShadowTexture.SampleCmpLevelZero(
                    ShadowSampler,
                    float3(
                        shadowUv
                            + float2(tapX, tapY)
                                * cascadeTexel,
                        cascadeIndex),
                    projected.z - ShadowParameters.y);
        }
    }
    return visibility / 9.0f;
}

float EvaluateDirectionalShadow(
    float3 worldPosition,
    float3 normal,
    uint lightIndex)
{
    if (ShadowParameters.x < 0.5f
        || lightIndex + 1u
            != (uint)ShadowParameters.x)
    {
        return 1.0f;
    }

    const float cameraDistance = dot(
        worldPosition - CameraPosition.xyz,
        CameraForward.xyz);
    uint cascadeIndex = 0u;
    [unroll]
    for (uint index = 0u; index < 4u; ++index)
    {
        if (index >= LightCounts.w)
        {
            return 1.0f;
        }
        cascadeIndex = index;
        if (cameraDistance <= ShadowCascadeSplits[index])
        {
            break;
        }
    }
    if (cameraDistance
        > ShadowCascadeSplits[cascadeIndex])
    {
        return 1.0f;
    }

    float visibility = SampleDirectionalShadowCascade(
        worldPosition,
        normal,
        cascadeIndex);
    if (cascadeIndex + 1u < LightCounts.w)
    {
        const float previousSplit = cascadeIndex == 0u
            ? 0.0f
            : ShadowCascadeSplits[cascadeIndex - 1u];
        const float cascadeRange =
            ShadowCascadeSplits[cascadeIndex]
            - previousSplit;
        const float blendStart =
            ShadowCascadeSplits[cascadeIndex]
            - cascadeRange * 0.1f;
        const float blend = saturate(
            (cameraDistance - blendStart)
            / max(cascadeRange * 0.1f, 0.0001f));
        if (blend > 0.0f)
        {
            const float nextVisibility =
                SampleDirectionalShadowCascade(
                    worldPosition,
                    normal,
                    cascadeIndex + 1u);
            visibility = lerp(
                visibility,
                nextVisibility,
                blend);
        }
    }

    return lerp(
        1.0f,
        visibility,
        saturate(ShadowParameters.w));
}

// スポットライトの影。slotはSpotShadowViewProjectionsの添字。
float EvaluateSpotShadow(
    float3 worldPosition,
    float3 normal,
    uint slot)
{
    const float4 parameters =
        SpotShadowParameters[slot];
    if (parameters.w < 0.5f)
    {
        return 1.0f;
    }
    const float3 biasedPosition =
        worldPosition + normal * parameters.y;
    const float4 lightPosition = mul(
        float4(biasedPosition, 1.0f),
        SpotShadowViewProjections[slot]);
    if (lightPosition.w <= 0.0001f)
    {
        return 1.0f;
    }
    const float3 projected =
        lightPosition.xyz / lightPosition.w;
    const float2 shadowUv =
        projected.xy * float2(0.5f, -0.5f)
        + 0.5f;
    if (shadowUv.x < 0.0f
        || shadowUv.x > 1.0f
        || shadowUv.y < 0.0f
        || shadowUv.y > 1.0f
        || projected.z <= 0.0f
        || projected.z >= 1.0f)
    {
        return 1.0f;
    }
    // 3x3 PCFで影の輪郭を柔らかくします。
    float visibility = 0.0f;
    const float spotTexel = ShadowTexelSizes.y;
    [unroll]
    for (int tapY = -1; tapY <= 1; ++tapY)
    {
        [unroll]
        for (int tapX = -1; tapX <= 1; ++tapX)
        {
            visibility +=
                SpotShadowTexture.SampleCmpLevelZero(
                    ShadowSampler,
                    float3(
                        shadowUv
                            + float2(tapX, tapY)
                                * spotTexel,
                        slot),
                    projected.z - parameters.x);
        }
    }
    visibility /= 9.0f;
    return lerp(1.0f, visibility, saturate(parameters.z));
}

// ポイントライトの影（キューブ深度）。
float EvaluatePointShadow(
    float3 worldPosition,
    uint lightIndex,
    float3 lightPosition,
    float range)
{
    if (PointShadowParameters.x < 0.5f
        || lightIndex + 1u
            != (uint)PointShadowParameters.x)
    {
        return 1.0f;
    }
    const float3 fromLight =
        worldPosition - lightPosition;
    const float3 absoluteVector = abs(fromLight);
    const float majorAxis = max(
        absoluteVector.x,
        max(absoluteVector.y, absoluteVector.z));
    const float nearPlane = 0.1f;
    const float farPlane = max(range, nearPlane + 0.01f);
    // 90度透視射影（RH）の深度をシェーダー側で再構成します。
    const float depth =
        farPlane / (farPlane - nearPlane)
        - farPlane * nearPlane
            / ((farPlane - nearPlane)
                * max(majorAxis, nearPlane));
    // 方向ベクトルを接平面内でずらした5タップPCF。
    const float3 direction = normalize(fromLight);
    const float3 axis =
        abs(direction.y) > 0.9f
            ? float3(1.0f, 0.0f, 0.0f)
            : float3(0.0f, 1.0f, 0.0f);
    const float3 tangent =
        normalize(cross(axis, direction));
    const float3 bitangent =
        cross(direction, tangent);
    const float pointTexel =
        ShadowTexelSizes.z * 2.0f;
    const float compareDepth =
        depth - PointShadowParameters.y;
    float visibility =
        PointShadowTexture.SampleCmpLevelZero(
            ShadowSampler,
            direction,
            compareDepth);
    visibility +=
        PointShadowTexture.SampleCmpLevelZero(
            ShadowSampler,
            normalize(
                direction
                + (tangent + bitangent) * pointTexel),
            compareDepth);
    visibility +=
        PointShadowTexture.SampleCmpLevelZero(
            ShadowSampler,
            normalize(
                direction
                + (tangent - bitangent) * pointTexel),
            compareDepth);
    visibility +=
        PointShadowTexture.SampleCmpLevelZero(
            ShadowSampler,
            normalize(
                direction
                - (tangent - bitangent) * pointTexel),
            compareDepth);
    visibility +=
        PointShadowTexture.SampleCmpLevelZero(
            ShadowSampler,
            normalize(
                direction
                - (tangent + bitangent) * pointTexel),
            compareDepth);
    visibility /= 5.0f;
    return lerp(
        1.0f,
        visibility,
        saturate(PointShadowParameters.z));
}

float4 PSMain(PixelInput input) : SV_Target
{
    const float3 normal = ApplyNormalMap(
        input,
        normalize(input.WorldNormal));
    const float3 viewDirection = normalize(
        CameraPosition.xyz - input.WorldPosition);
    const float4 albedo =
        AlbedoTexture.Sample(MaterialSampler, input.TexCoord)
        * input.Tint;
    // マップがある場合は係数へ掛けます（glTF仕様と同じ扱い）。
    float roughnessValue = MaterialParameters.x;
    if (MaterialTextureParameters.x >= 0.5f)
    {
        roughnessValue *= RoughnessTexture.Sample(
            MaterialSampler,
            input.TexCoord).g;
    }
    float metallicValue = MaterialParameters.w;
    if (MaterialTextureParameters.y >= 0.5f)
    {
        metallicValue *= MetallicTexture.Sample(
            MaterialSampler,
            input.TexCoord).b;
    }
    const float roughness = clamp(
        roughnessValue,
        0.04f,
        1.0f);
    const float metallic = saturate(metallicValue);

    // 遮蔽（AO）は間接光だけを暗くします。直接光に掛けると
    // 影の中がさらに暗くなって汚れて見えるためです。
    float occlusion = 1.0f;
    if (MaterialTextureParameters.z >= 0.5f)
    {
        const float sampled = OcclusionTexture.Sample(
            MaterialSampler,
            input.TexCoord).r;
        occlusion = lerp(
            1.0f,
            sampled,
            saturate(MaterialTextureParameters.w));
    }

    // SSAOも同じ扱いで間接光だけへ掛けます。深度プリパスで
    // ライティングより前に用意されているので、完成した色へ掛けて
    // いた従来のやり方と違って直接光や影の中を暗くしません。
    if (ScreenAmbientOcclusionParameters.z >= 0.5f)
    {
        const float2 screenUV =
            input.Position.xy
            * ScreenAmbientOcclusionParameters.xy;
        occlusion *= ScreenAmbientOcclusionTexture.Sample(
            MaterialSampler,
            screenUV).r;
    }

    float3 lighting = EvaluateEnvironment(
        normal,
        viewDirection,
        input.WorldPosition,
        albedo.rgb,
        roughness,
        metallic) * occlusion;

    [loop]
    for (uint index = 0; index < min(LightCounts.x, 4u); ++index)
    {
        const DirectionalLight light = DirectionalLights[index];
        const float shadow = EvaluateDirectionalShadow(
            input.WorldPosition,
            normal,
            index);
        // Color.wに太陽の角半径（ラジアン）を格納し、
        // cbufferのレイアウトを維持します。
        const float3 toLight =
            normalize(-light.DirectionIntensity.xyz);
        const float angularRadius = light.Color.w;
        lighting += EvaluateLightPbrSized(
            normal,
            toLight,
            SourceRepresentativeDirection(
                toLight,
                normal,
                viewDirection,
                angularRadius),
            viewDirection,
            albedo.rgb,
            roughness,
            metallic,
            light.Color.rgb
                * light.DirectionIntensity.w
                * shadow,
            SourceSpecularEnergy(roughness, angularRadius));
    }

    if (ClusteredParameters.w >= 0.5f)
    {
        // クラスタ経路（Forward+）。自分のピクセルが入っている
        // クラスタの番号表だけを見てポイント／スポットを計算します。
        // 表はLamaPonLightCulling.hlslが作っています。
        const uint gridX = (uint)ClusteredParameters.x;
        const uint gridY = (uint)ClusteredParameters.y;
        const uint gridZ = (uint)ClusteredParameters.z;
        const float2 screenRatio = saturate(
            input.Position.xy
            * ClusteredScreenParameters.xy);
        const uint clusterX = min(
            (uint)(screenRatio.x * gridX),
            gridX - 1u);
        const uint clusterY = min(
            (uint)(screenRatio.y * gridY),
            gridY - 1u);
        // カメラからの奥行き（前方向への射影距離）から、指数分割の
        // スライス番号を求めます。カリング側と同じ式です。
        const float nearPlane =
            ClusteredDepthParameters.x;
        const float viewDepth = max(
            dot(
                CameraForward.xyz,
                input.WorldPosition
                    - CameraPosition.xyz),
            nearPlane);
        const uint clusterZ = min(
            (uint)(log(viewDepth / nearPlane)
                / ClusteredDepthParameters.z
                * gridZ),
            gridZ - 1u);
        const uint cluster =
            clusterZ * gridX * gridY
            + clusterY * gridX
            + clusterX;
        const uint maximumPerCluster =
            (uint)ClusteredDepthParameters.w;
        const uint clusterOffset =
            cluster * maximumPerCluster;
        const uint clusterLightCount =
            min(ClusterLightCounts[cluster],
                maximumPerCluster);

        [loop]
        for (uint slot = 0;
            slot < clusterLightCount;
            ++slot)
        {
            const ClusterLight light = ClusterLights[
                ClusterLightIndexList[
                    clusterOffset + slot]];
            const float3 delta =
                light.PositionRange.xyz
                - input.WorldPosition;
            const float distance = length(delta);
            const float range = max(
                light.PositionRange.w,
                0.001f);
            const float distanceAttenuation =
                pow(saturate(1.0f - distance / range),
                    2.0f);
            const float3 toLight =
                delta / max(distance, 0.0001f);

            float attenuation = distanceAttenuation;
            float shadow = 1.0f;
            if (light.ExtraParameters.y < 0.5f)
            {
                // ポイントライト。影の参照はライト番号+1です
                // （EvaluatePointShadowが対象かどうかを自分で
                // 照合します）。
                if (light.ExtraParameters.z >= 1.0f)
                {
                    shadow = EvaluatePointShadow(
                        input.WorldPosition,
                        (uint)light.ExtraParameters.z
                            - 1u,
                        light.PositionRange.xyz,
                        range);
                }
            }
            else
            {
                // スポットライト。コーン減衰は従来経路と同じく
                // 2乗で締めます。
                const float cone = dot(
                    normalize(
                        light.DirectionInnerCosine.xyz),
                    -toLight);
                const float coneAttenuation = smoothstep(
                    light.ExtraParameters.x,
                    light.DirectionInnerCosine.w,
                    cone);
                attenuation *=
                    coneAttenuation * coneAttenuation;
                if (light.ExtraParameters.z >= 1.0f)
                {
                    shadow = EvaluateSpotShadow(
                        input.WorldPosition,
                        normal,
                        (uint)light.ExtraParameters.z
                            - 1u);
                }
            }

            lighting += EvaluateLightPbr(
                normal,
                toLight,
                viewDirection,
                albedo.rgb,
                roughness,
                metallic,
                light.ColorIntensity.rgb
                    * light.ColorIntensity.w
                    * attenuation
                    * shadow);
        }
    }
    else
    {
    [loop]
    for (uint index = 0; index < min(LightCounts.y, 16u); ++index)
    {
        const PointLight light = PointLights[index];
        const float3 delta =
            light.PositionRange.xyz - input.WorldPosition;
        const float distance = length(delta);
        const float range = max(light.PositionRange.w, 0.001f);
        const float attenuation =
            pow(saturate(1.0f - distance / range), 2.0f);
        const float shadow = EvaluatePointShadow(
            input.WorldPosition,
            index,
            light.PositionRange.xyz,
            range);
        lighting += EvaluateLightPbr(
            normal,
            delta / max(distance, 0.0001f),
            viewDirection,
            albedo.rgb,
            roughness,
            metallic,
            light.ColorIntensity.rgb
                * light.ColorIntensity.w
                * attenuation
                * shadow);
    }

    [loop]
    for (uint index = 0; index < min(LightCounts.z, 8u); ++index)
    {
        const SpotLight light = SpotLights[index];
        const float3 lightToPixel =
            input.WorldPosition - light.PositionRange.xyz;
        const float distance = length(lightToPixel);
        const float range = max(light.PositionRange.w, 0.001f);
        const float3 rayDirection =
            lightToPixel / max(distance, 0.0001f);
        const float cone = dot(
            normalize(light.DirectionInnerCosine.xyz),
            rayDirection);
        const float coneAttenuation = smoothstep(
            light.OuterCosinePadding.x,
            light.DirectionInnerCosine.w,
            cone);
        const float distanceAttenuation =
            pow(saturate(1.0f - distance / range), 2.0f);
        // OuterCosinePadding.y = 影スロット+1（0なら影なし）
        float shadow = 1.0f;
        if (light.OuterCosinePadding.y >= 1.0f)
        {
            shadow = EvaluateSpotShadow(
                input.WorldPosition,
                normal,
                (uint)light.OuterCosinePadding.y - 1u);
        }
        lighting += EvaluateLightPbr(
            normal,
            -rayDirection,
            viewDirection,
            albedo.rgb,
            roughness,
            metallic,
            light.ColorIntensity.rgb
                * light.ColorIntensity.w
                * distanceAttenuation
                * coneAttenuation
                * coneAttenuation
                * shadow);
    }
    } // 従来経路（クラスタ無効時）の終わり

    // 発光はライティングとは無関係に足します（影の中でも光る）。
    // Fogは発光後に掛けるので、遠くのネオンは霧に沈みます。
    float3 emissive = EmissiveParameters.rgb;
    if (EmissiveParameters.w >= 0.5f)
    {
        emissive *= EmissiveTexture.Sample(
            MaterialSampler,
            input.TexCoord).rgb;
    }
    lighting += emissive;

    const float3 litColor = max(lighting, 0.0f);
    if (FogParameters.w < 0.5f)
    {
        return float4(litColor, albedo.a);
    }
    const float distanceToCamera =
        length(input.WorldPosition - CameraPosition.xyz);
    const float rangeFog = smoothstep(
        FogParameters.x,
        max(FogParameters.y, FogParameters.x + 0.001f),
        distanceToCamera);
    const float exponentialFog =
        1.0f
        - exp(
            -max(FogParameters.z, 0.0f)
            * max(
                distanceToCamera - FogParameters.x,
                0.0f));
    const float fogAmount =
        saturate(max(rangeFog, exponentialFog));
    return float4(
        lerp(litColor, FogColor.rgb, fogAmount),
        albedo.a);
}

// スキニング用のピクセルシェーダー。
// スキニングモデルの頂点変形はDirectXTKのSkinnedEffectが行い、
// エンジンはピクセルシェーダーだけを差し替えます。そのため入力は
// VSSkinnedMainの出力ではなく、DirectXTK側のper-pixel lighting
// 出力（TexCoordがTEXCOORD0、WorldPositionがTEXCOORD1…）に
// 合わせる必要があります。並びを詰め替えてPSMainへ渡します。
float4 PSSkinnedMain(SkinnedPixelInput input) : SV_Target
{
    PixelInput pixel;
    pixel.Position = input.Position;
    pixel.WorldPosition = input.WorldPosition.xyz;
    pixel.WorldNormal = input.WorldNormal;
    pixel.TexCoord = input.TexCoord;
    // DirectXTK側のDiffuseにも同じ色が乗っているため、二重に
    // 掛からないようMaterialColorを使います。
    pixel.Tint = MaterialColor;
    return PSMain(pixel);
}
