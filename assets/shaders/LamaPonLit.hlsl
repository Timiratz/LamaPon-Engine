// CPU側の定数配置を保ち、追加項目を省く独自Shaderでは使用範囲まで同じ順序で宣言する。
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
    // 互換配置の追加材質設定
    float4 CustomParameters[8];
    // 粗さ・金属・遮蔽有無と強度
    float4 MaterialTextureParameters;
    // 発光RGB・W画像使用フラグ
    float4 EmissiveParameters;
    // 1時間周期秒・差分秒・フレーム数
    float4 TimeParameters;
};

// t0の表面色画像
Texture2D AlbedoTexture : register(t0);
// RGで採取する法線画像
Texture2D NormalTexture : register(t1);
// 4層の平行光Cascade影
Texture2DArray ShadowTexture : register(t2);
// 主プローブの鏡面天空画像
TextureCube EnvironmentMap : register(t3);
// 4枠のスポット影画像
Texture2DArray SpotShadowTexture : register(t4);
// 選択点光源のCube深度画像
TextureCube PointShadowTexture : register(t5);
// 主プローブの拡散天空画像
TextureCube IrradianceMap : register(t6);
// Gで採取する粗さ画像
// t7～t10は独自Shader用に保ち、PBR画像はG粗さ・B金属度・R遮蔽で読む。
Texture2D RoughnessTexture : register(t11);
// Bで採取する金属度画像
Texture2D MetallicTexture : register(t12);
// Rで採取する遮蔽画像
Texture2D OcclusionTexture : register(t13);
// RGBで採取する発光画像
Texture2D EmissiveTexture : register(t14);
// 間接光用の画面遮蔽画像
Texture2D ScreenAmbientOcclusionTexture : register(t15);

// Compute側と配置を揃え、Y種別は0point・1spot、Z影参照はpoint番号+1・spot枠+1・0影なしとする。
struct ClusterLight
{
    // World位置XYZ・W到達距離
    float4 PositionRange;
    // 光RGB・W強度
    float4 ColorIntensity;
    // 光進行方向XYZ・W内角cos
    float4 DirectionInnerCosine;
    // X外角cos・Y種別・Z影参照
    float4 ExtraParameters;
};
// Computeと共有する光源一覧
StructuredBuffer<ClusterLight> ClusterLights : register(t16);
// Clusterごとの光源番号列
StructuredBuffer<uint> ClusterLightIndexList : register(t17);
// Clusterごとの光源件数
StructuredBuffer<uint> ClusterLightCounts : register(t18);
// 副プローブの鏡面天空画像
TextureCube SecondaryEnvironmentMap : register(t19);
// 副プローブの拡散天空画像
TextureCube SecondaryIrradianceMap : register(t20);
// 前フレームのHDR画像
Texture2D ScreenReflectionColorTexture : register(t21);
// 最短距離のHi-Z画像
Texture2D ScreenReflectionDepthTexture : register(t22);

// 間接光RのL1係数画像
Texture3D BakedGiRedTexture : register(t23);
// 間接光GのL1係数画像
Texture3D BakedGiGreenTexture : register(t24);
// 間接光BのL1係数画像
Texture3D BakedGiBlueTexture : register(t25);
// 材質画像の採取設定
SamplerState MaterialSampler : register(s0);
// 互換配置の影比較採取設定
SamplerComparisonState ShadowSampler : register(s1);

// GGX分布に使う円周率
static const float LamaPonPi = 3.14159265f;

struct DirectionalLight
{
    // 光進行方向XYZ・W強度
    float4 DirectionIntensity;
    // 光RGB・W太陽角半径rad
    float4 Color;
};

struct PointLight
{
    // World位置XYZ・W到達距離
    float4 PositionRange;
    // 光RGB・W強度
    float4 ColorIntensity;
};

struct SpotLight
{
    // World位置XYZ・W到達距離
    float4 PositionRange;
    // 光進行方向XYZ・W内角cos
    float4 DirectionInnerCosine;
    // 光RGB・W強度
    float4 ColorIntensity;
    // X外角cos・Y影枠+1
    float4 OuterCosinePadding;
};

// CPUと配置を揃える光源定数
cbuffer LightingBuffer : register(b1)
{
    // 間接光の基本RGB
    float4 Ambient;
    // 平行・点・スポット・Cascade数
    uint4 LightCounts;
    // 最大4本の平行光
    DirectionalLight DirectionalLights[4];
    // 通常経路の最大16本の点光
    PointLight PointLights[16];
    // 通常経路の最大8本のスポット
    SpotLight SpotLights[8];
    // 4CascadeのWorld影変換
    row_major float4x4 ShadowViewProjections[4];
    // 4Cascadeの終端距離
    float4 ShadowCascadeSplits;
    // 対象+1・深度法線補正・強度
    float4 ShadowParameters;
    // 霧のRGB
    float4 FogColor;
    // 開始・終了距離・密度・有効
    float4 FogParameters;
    // IBL強度・有効・最終Mip
    float4 EnvironmentParameters;
    // 4枠のWorldスポット影変換
    row_major float4x4 SpotShadowViewProjections[4];
    // 深度・法線補正・強度・有効
    float4 SpotShadowParameters[4];
    // 対象+1・深度補正・強度
    float4 PointShadowParameters;
    // Cascade・spot・point画素幅
    float4 ShadowTexelSizes;
    // 逆画面幅高さ・Z有効
    float4 ScreenAmbientOcclusionParameters;
    // 分割XYZ・W有効
    float4 ClusteredParameters;
    // near・far・対数比・灯数上限
    float4 ClusteredDepthParameters;
    // 逆画面幅高さ・Z総灯数
    float4 ClusteredScreenParameters;
    // 主プローブのWorld箱中心
    float4 ReflectionBoxCenter;
    // 主プローブ箱半径XYZ・W有効
    float4 ReflectionBoxParameters;
    // 副プローブのWorld箱中心
    float4 ReflectionSecondaryBoxCenter;
    // 副プローブ箱半径XYZ・W有効
    float4 ReflectionSecondaryBoxParameters;
    // 副混合比・副最終Mip
    float4 ReflectionBlendParameters;
    // SSR強度・有効・距離・反復数
    float4 ScreenReflectionParameters;
    // 逆画面幅高さ・投影係数ZW
    float4 ScreenReflectionScreen;
    // 厚み・粗さ上限・最終Mip
    float4 ScreenReflectionQuality;
    // 前フレームのビュー透視合成
    row_major float4x4 ScreenReflectionPreviousViewProjection;
    // 格子のWorld最小XYZ・W有効
    float4 BakedGiVolumeMinimum;
    // 逆格子寸法XYZ・W強度
    float4 BakedGiInverseSize;
    // 各軸の間接光プローブ数
    float4 BakedGiResolution;
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

// インポーターの頂点配置を保ち、法線画像の接空間はTangent入力を使わず画面微分で作る。
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
    // 通常材質色またはInstance色
    float4 Tint : COLOR0;
};

// 通常頂点をWorld変換して材質色を渡す(input: 通常頂点)。
PixelInput VSMain(VertexInput input)
{
    // World変換後のPixel入力
    PixelInput output;
    // World空間の同次位置
    const float4 worldPosition = mul(float4(input.Position, 1.0f), World);
    output.Position = mul(worldPosition, ViewProjection);
    output.WorldPosition = worldPosition.xyz;
    output.WorldNormal = normalize(
        mul(float4(input.Normal, 0.0f), WorldInverseTranspose).xyz);
    output.TexCoord = input.TexCoord;
    output.Tint = MaterialColor;
    return output;
}

// Instanceの行列と色で頂点を変換する(input: Instance付き頂点)。
PixelInput VSInstancedMain(InstancedVertexInput input)
{
    // World変換後のPixel入力
    PixelInput output;
    // Instanceから組むWorld行列
    const float4x4 world = float4x4(
        input.InstanceWorld0,
        input.InstanceWorld1,
        input.InstanceWorld2,
        input.InstanceWorld3);
    // World空間の同次位置
    const float4 worldPosition =
        mul(float4(input.Position, 1.0f), world);
    output.Position = mul(worldPosition, ViewProjection);
    output.WorldPosition = worldPosition.xyz;
    // Instance法線はWorld行列で近似し、非一様倍率では逆転置と一致しない。
    output.WorldNormal = normalize(
        mul(float4(input.Normal, 0.0f), world).xyz);
    output.TexCoord = input.TexCoord;
    output.Tint = input.InstanceColor;
    return output;
}

// 最大4骨を加重合成してローカル位置と法線を変形する(input: スキン頂点, position: 出力ローカル位置, normal: 出力ローカル法線)。
void SkinVertex(
    SkinnedVertexInput input,
    out float3 position,
    out float3 normal)
{
    // 4骨の加重合成行列
    float4x3 skinning = 0.0f;
    // 走査する光源または骨の番号
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

// 骨変形後の頂点をWorld変換して材質色を渡す(input: スキン頂点)。
PixelInput VSSkinnedMain(SkinnedVertexInput input)
{
    // 骨変形後のローカル位置
    float3 skinnedPosition;
    // 骨変形後のローカル法線
    float3 skinnedNormal;
    SkinVertex(input, skinnedPosition, skinnedNormal);

    // World変換後のPixel入力
    PixelInput output;
    // World空間の同次位置
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

// RG法線画像と画面微分の接空間でWorld法線を作る(input: World位置と画像UV, geometricNormal: World単位幾何法線)。
float3 ApplyNormalMap(PixelInput input, float3 geometricNormal)
{
    if (MaterialParameters.z < 0.5f)
    {
        return geometricNormal;
    }

    // 強度補正した接空間法線
    // BC5のRG画像に対応するため、強度を掛けたXYから正のZを復元する。
    float3 mappedNormal;
    mappedNormal.xy =
        NormalTexture.Sample(MaterialSampler, input.TexCoord).xy
        * 2.0f
        - 1.0f;
    mappedNormal.xy *= MaterialParameters.y;
    mappedNormal.z = sqrt(
        saturate(1.0f - dot(mappedNormal.xy, mappedNormal.xy)));

    // World位置の画面X微分
    const float3 positionDerivativeX = ddx(input.WorldPosition);
    // World位置の画面Y微分
    const float3 positionDerivativeY = ddy(input.WorldPosition);
    // 画像UVの画面X微分
    const float2 uvDerivativeX = ddx(input.TexCoord);
    // 画像UVの画面Y微分
    const float2 uvDerivativeY = ddy(input.TexCoord);
    // 画面Y微分と法線の外積
    const float3 perpendicularY =
        cross(positionDerivativeY, geometricNormal);
    // 法線と画面X微分の外積
    const float3 perpendicularX =
        cross(geometricNormal, positionDerivativeX);
    // 面に沿う接方向
    const float3 tangent =
        perpendicularY * uvDerivativeX.x
        + perpendicularX * uvDerivativeY.x;
    // 面に沿う従接方向
    const float3 bitangent =
        perpendicularY * uvDerivativeX.y
        + perpendicularX * uvDerivativeY.y;
    // 接空間の共通正規化倍率
    const float scale = rsqrt(max(
        max(dot(tangent, tangent), dot(bitangent, bitangent)),
        0.000001f));
    return normalize(
        tangent * (mappedNormal.x * scale)
        + bitangent * (mappedNormal.y * scale)
        + geometricNormal * mappedNormal.z);
}

// 反射方向を光源円盤内の代表方向へ寄せる(toLight: 光源への単位方向, normal: World単位法線, viewDirection: 視点への単位方向, angularRadius: 光源角半径rad)。
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
    // 視線を法線で反射した方向
    const float3 reflected = reflect(-viewDirection, normal);
    // 光源中心と反射方向の内積
    const float alignment = dot(toLight, reflected);
    // 光源角半径の余弦
    const float diskCosine = cos(angularRadius);
    if (alignment >= diskCosine)
    {
        return reflected;
    }
    // 光源方向に直交する反射成分
    const float3 sideways = reflected - alignment * toLight;
    // 直交する反射成分の長さ
    const float sidewaysLength = length(sideways);
    if (sidewaysLength <= 1.0e-5f)
    {
        return toLight;
    }
    return normalize(
        toLight * diskCosine
        + (sideways / sidewaysLength) * sin(angularRadius));
}

// 光源円盤で広がる鏡面反射の量を補正する(roughness: 表面の粗さ, angularRadius: 光源角半径rad)。
float SourceSpecularEnergy(
    float roughness,
    float angularRadius)
{
    if (angularRadius <= 0.0f)
    {
        return 1.0f;
    }
    // 二乗粗さのGGX幅
    const float alpha = max(roughness * roughness, 1.0e-4f);
    // 光源角半径で広げたGGX幅
    const float widened =
        saturate(alpha + sin(angularRadius) * 0.5f);
    // 元の幅と補正後の幅の比
    const float ratio = alpha / max(widened, 1.0e-4f);
    return ratio * ratio;
}

// 拡散は光源中心・鏡面は代表方向でGGX直接光を求める(normal: World単位法線, toLight: 光源への単位方向, viewDirection: 視点への単位方向, albedo: 表面RGB, roughness: 表面の粗さ, metallic: 金属度, radiance: 強度・減衰・影込みRGB, specularToLight: 鏡面用の代表方向, specularEnergy: 鏡面反射量の補正比)。
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
    // 法線と光源中心方向の内積
    const float normalDotLight =
        saturate(dot(normal, toLight));
    if (normalDotLight <= 0.0f)
    {
        return 0.0f.xxx;
    }
    // 代表光と視線の中間方向
    const float3 halfVector =
        normalize(specularToLight + viewDirection);
    // 下限付きの法線と視線内積
    const float normalDotView = max(
        dot(normal, viewDirection),
        0.0001f);
    // 法線と中間方向の内積
    const float normalDotHalf =
        saturate(dot(normal, halfVector));
    // 視線と中間方向の内積
    const float viewDotHalf =
        saturate(dot(viewDirection, halfVector));

    // 二乗粗さのGGX幅
    const float alpha = roughness * roughness;
    // GGX幅の二乗
    const float alphaSquared = alpha * alpha;

    // GGX分母の中間項
    const float denominator =
        normalDotHalf * normalDotHalf
            * (alphaSquared - 1.0f)
        + 1.0f;
    // GGXの法線分布密度
    // 分母の下限を1e-12に保ち、低い粗さの鋭い反射を潰さない。
    const float distribution =
        alphaSquared
        / max(LamaPonPi * denominator * denominator,
            1.0e-12f);

    // Smith減衰の粗さ係数
    const float k = alpha * 0.5f + 0.0001f;
    // 視線側の幾何減衰
    const float geometryView =
        normalDotView / (normalDotView * (1.0f - k) + k);
    // 光源側の幾何減衰
    const float geometryLight =
        normalDotLight
        / (normalDotLight * (1.0f - k) + k);
    // 視線と光源の幾何減衰
    const float geometry = geometryView * geometryLight;

    // 金属度による正面反射RGB
    const float3 f0 = lerp(0.04f.xxx, albedo, metallic);
    // Schlickの角度反射RGB
    const float3 fresnel =
        f0
        + (1.0f.xxx - f0)
            * pow(1.0f - viewDotHalf, 5.0f);

    // 鏡面反射のRGB寄与
    const float3 specular =
        distribution * geometry * fresnel
        * specularEnergy
        / max(4.0f * normalDotView * normalDotLight,
            0.0001f);
    // 拡散反射のRGB寄与
    const float3 diffuse =
        (1.0f.xxx - fresnel)
        * (1.0f - metallic)
        * albedo
        / LamaPonPi;
    return (diffuse + specular)
        * radiance
        * normalDotLight;
}

// 点光源のGGX直接光を求める(normal: World単位法線, toLight: 光源への単位方向, viewDirection: 視点への単位方向, albedo: 表面RGB, roughness: 表面の粗さ, metallic: 金属度, radiance: 強度・減衰・影込みRGB)。
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

// 反射レイと箱の交点をプローブ中心からの方向へ変換する(reflection: World反射方向, worldPosition: 表面のWorld位置, boxCenter: 箱のWorld中心, boxExtents: 箱の各軸の半径)。
float3 ApplyBoxProjection(
    float3 reflection,
    float3 worldPosition,
    float3 boxCenter,
    float3 boxExtents)
{
    // 箱の正側面までのレイ係数
    const float3 firstPlane =
        (boxCenter + boxExtents - worldPosition)
        / reflection;
    // 箱の負側面までのレイ係数
    const float3 secondPlane =
        (boxCenter - boxExtents - worldPosition)
        / reflection;
    // 各軸の遠い側面のレイ係数
    const float3 furthest =
        max(firstPlane, secondPlane);
    // 箱の最初の出口レイ係数
    const float distance = min(
        min(furthest.x, furthest.y),
        furthest.z);
    // World空間の箱との交点
    const float3 intersection =
        worldPosition + reflection * distance;
    return intersection - boxCenter;
}

// 箱補正した反射方向と粗さで環境画像を採取する(probeMap: プローブの天空画像, normal: World単位法線, viewDirection: 視点への単位方向, worldPosition: 表面のWorld位置, roughness: 表面の粗さ, maximumMip: 画像の最終Mip番号, boxCenter: 箱のWorld中心XYZ, boxParameters: 箱半径XYZ・W有効)。
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
    // 箱補正前後の反射方向
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

// L1間接光を格子中心で採取し縁の5%で環境光へ混ぜる(worldPosition: 表面のWorld位置, normal: World単位法線)。
float3 EvaluateBakedAmbient(
    float3 worldPosition,
    float3 normal)
{
    if (BakedGiVolumeMinimum.w < 0.5f)
    {
        return Ambient.rgb;
    }
    // 格子内の正規化World位置
    const float3 volumeUvw =
        (worldPosition - BakedGiVolumeMinimum.xyz)
        * BakedGiInverseSize.xyz;

    // 各軸の間接光プローブ数
    const float3 resolution = BakedGiResolution.xyz;
    // 格子角を画素中心へ移したUV
    // プローブは格子の角に配置されるため、採取UVをテクセル中心へ補正する。
    const float3 texelUvw =
        (volumeUvw * (resolution - 1.0f) + 0.5f)
        / resolution;

    // 法線XYZ・定数1のL1基底
    const float4 basis = float4(normal, 1.0f);
    // ベイク間接光のRGB
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
    gi = max(gi, 0.0f.xxx) * BakedGiInverseSize.w;

    // 5%幅で測る各軸の縁距離
    const float3 edge =
        (0.5f - abs(volumeUvw - 0.5f)) / 0.05f;
    // 現在の骨の影響比
    const float weight = saturate(
        min(min(edge.x, edge.y), edge.z));
    return lerp(Ambient.rgb, gi, weight);
}

// Hi-Zの最細Mipを点読みして表面距離を返す(uv: 画面UV)。
float ScreenReflectionSceneDistance(float2 uv)
{
    // 元の深度画像の幅と高さ
    const float2 screenSize =
        1.0f / max(ScreenReflectionScreen.xy, 1e-6f);
    // 深度画像の最終画素XY
    const int2 lastPixel = max(int2(screenSize) - 1, int2(0, 0));
    // 深度を読む画素XY
    const int2 pixel = clamp(
        int2(saturate(uv) * screenSize),
        int2(0, 0),
        lastPixel);
    // t22は最短視点距離のHi-Zで、輪郭を跨ぐ偽の距離を避けて点読みする。
    return ScreenReflectionDepthTexture.Load(
        int3(pixel, 0)).r;
}

// Hi-Zを区間探索し前フレーム色と信頼度を返す(worldPosition: 表面のWorld位置, reflection: World単位反射方向, viewDirection: 視点への単位方向, roughness: 表面の粗さ)。
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
    // SSRを止める粗さの上限
    const float roughnessCutoff = max(
        ScreenReflectionQuality.y,
        0.0001f);
    if (roughness >= roughnessCutoff)
    {
        return 0.0f;
    }

    // 反射レイのWorld最大距離
    const float maximumDistance = max(
        ScreenReflectionParameters.z,
        0.01f);
    // SSR面の許容厚み
    const float thickness = max(
        ScreenReflectionQuality.x,
        0.001f);
    // 4～128のHi-Z反復上限
    // Hi-Zのサンプル数は区画移動とMip変更の合計反復上限を表す。
    const int maximumSteps = clamp(
        (int)ScreenReflectionParameters.w,
        4,
        128);

    // 反射レイのWorld始点
    const float3 rayStart = worldPosition;
    // Near補正前後のWorld終点
    float3 rayEnd = worldPosition + reflection * maximumDistance;
    // レイ始点の透視同次位置
    float4 clipStart = mul(
        float4(rayStart, 1.0f),
        ViewProjection);
    // レイ終点の透視同次位置
    float4 clipEnd = mul(float4(rayEnd, 1.0f), ViewProjection);

    // Near補正する正のW下限
    // RH透視のWを視点距離として使い、Nearを跨ぐ終点は射影前にWorldで切る。
    const float nearW = 0.05f;
    if (clipStart.w <= nearW)
    {
        return 0.0f;
    }
    if (clipEnd.w <= nearW)
    {
        // Near境界への補間比
        const float clipRatio =
            (nearW - clipStart.w)
            / (clipEnd.w - clipStart.w);
        rayEnd = lerp(rayStart, rayEnd, saturate(clipRatio));
        clipEnd = mul(float4(rayEnd, 1.0f), ViewProjection);
    }

    // レイ始点の画面UV
    const float2 startUv = float2(
        clipStart.x / clipStart.w * 0.5f + 0.5f,
        0.5f - clipStart.y / clipStart.w * 0.5f);
    // レイ終点の画面UV
    const float2 endUv = float2(
        clipEnd.x / clipEnd.w * 0.5f + 0.5f,
        0.5f - clipEnd.y / clipEnd.w * 0.5f);
    // 画面UVの始終差分
    const float2 deltaUv = endUv - startUv;

    // 画面端までのレイ補間上限
    float limitAlpha = 1.0f;
    // 比較する画面XY軸番号
    [unroll]
    for (int axis = 0; axis < 2; ++axis)
    {
        // 選択した画面軸の差分
        const float direction = axis == 0
            ? deltaUv.x
            : deltaUv.y;
        // 選択した画面軸の始点
        const float origin = axis == 0
            ? startUv.x
            : startUv.y;
        if (abs(direction) > 1e-6f)
        {
            // 区画外へ出るレイ補間比
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

    // 元の深度画像の幅と高さ
    const float2 screenSize =
        1.0f / max(ScreenReflectionScreen.xy, 1e-6f);
    // 画面内レイの画素差分
    const float2 pixelDelta =
        deltaUv * limitAlpha * screenSize;
    // 画面内レイの最大軸画素長
    const float pixelLength = max(
        max(abs(pixelDelta.x), abs(pixelDelta.y)),
        1.0f);

    // レイ始点の透視W逆数
    const float inverseStartW = 1.0f / clipStart.w;
    // レイ終点の透視W逆数
    const float inverseEndW = 1.0f / clipEnd.w;

    // Hi-Zの最終Mip番号
    const int maximumLevel = max(
        (int)ScreenReflectionQuality.z,
        0);

    // 自己交差を避ける探索補間比
    float alpha = 0.5f * limitAlpha / pixelLength;
    // 区画境界を越える最小補間幅
    // 区画境界の丸めで同じ区画を再訪しないよう、補間比を微小に進める。
    const float alphaBias = limitAlpha * 1e-5f;
    // 探索中のHi-Z Mip番号
    int level = 0;

    // Hi-Z反復の回数
    [loop]
    for (int step = 0; step < maximumSteps; ++step)
    {
        if (alpha >= limitAlpha)
        {
            break;
        }
        // 探索点の画面UV
        const float2 uv = startUv + deltaUv * alpha;
        // 現在Mipの幅と高さ
        const float2 levelSize = max(
            floor(screenSize / exp2((float)level)),
            1.0f);
        // 現在Mipの区画XY番号
        const float2 cell = floor(
            clamp(uv, 0.0f, 1.0f) * levelSize);
        // 進行方向側の区画端0か1
        const float2 towardEdge = float2(
            deltaUv.x >= 0.0f ? 1.0f : 0.0f,
            deltaUv.y >= 0.0f ? 1.0f : 0.0f);
        // 進行方向側の区画端UV
        const float2 boundaryUv =
            (cell + towardEdge) / levelSize;
        // 各画面軸の区画出口補間比
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
        // 区画外へ出るレイ補間比
        const float exitAlpha = max(
            min(boundaryAlpha.x, boundaryAlpha.y),
            alpha + alphaBias);
        // 画面内に制限した出口補間比
        const float clampedExitAlpha = min(
            exitAlpha,
            limitAlpha);

        // 区画入口のレイ視点距離
        const float entryDistance = 1.0f / max(
            lerp(inverseStartW, inverseEndW, alpha),
            1e-6f);
        // 区画出口のレイ視点距離
        const float exitDistance = 1.0f / max(
            lerp(
                inverseStartW,
                inverseEndW,
                clampedExitAlpha),
            1e-6f);
        // 区画内レイの最短視点距離
        const float rayNear = min(
            entryDistance,
            exitDistance);
        // 区画内レイの最長視点距離
        const float rayFar = max(
            entryDistance,
            exitDistance);

        // 区画内面の最短視点距離
        const float sceneDistance =
            ScreenReflectionDepthTexture.Load(int3(
                int2(min(cell, levelSize - 1.0f)),
                level)).r;

        if (rayFar <= sceneDistance)
        {
            alpha = exitAlpha;
            // 面から2%以上離れた区間だけ粗いMipへ移り、面付近のMip往復を抑える。
            if (rayFar * 1.02f <= sceneDistance)
            {
                level = min(level + 1, maximumLevel);
            }
            continue;
        }
        if (level > 0)
        {
            level = level - 1;
            continue;
        }

        if (rayFar > sceneDistance
            && rayNear < sceneDistance + thickness)
        {
            // 精査区間の入口補間比
            float nearAlpha = alpha;
            // 精査区間の出口補間比
            float farAlpha = clampedExitAlpha;
            // 二分精査の反復番号
            [unroll]
            for (int refine = 0; refine < 4; ++refine)
            {
                // 精査区間の中点補間比
                const float middleAlpha =
                    (nearAlpha + farAlpha) * 0.5f;
                // 精査中点のレイ視点距離
                const float middleDistance = 1.0f / max(
                    lerp(
                        inverseStartW,
                        inverseEndW,
                        middleAlpha),
                    1e-6f);
                // 精査中点の面の視点距離
                const float middleScene =
                    ScreenReflectionSceneDistance(
                        startUv + deltaUv * middleAlpha);
                if (middleDistance > middleScene)
                {
                    farAlpha = middleAlpha;
                }
                else
                {
                    nearAlpha = middleAlpha;
                }
            }
            // 交差点のレイ画面補間比
            const float hitAlpha =
                (nearAlpha + farAlpha) * 0.5f;
            // 交差点の現在画面UV
            const float2 hitUv = startUv + deltaUv * hitAlpha;

            // 透視補正したWorld補間比
            // 画面の補間比を透視WでWorld比へ戻し、交差位置と距離減衰に使う。
            const float worldRatio =
                hitAlpha * clipStart.w
                / max(
                    lerp(clipEnd.w, clipStart.w, hitAlpha),
                    1e-6f);
            // 交差点のWorld位置
            const float3 hitPosition = lerp(
                rayStart,
                rayEnd,
                saturate(worldRatio));

            // 交差点の前フレーム透視位置
            const float4 previousClip = mul(
                float4(hitPosition, 1.0f),
                ScreenReflectionPreviousViewProjection);
            if (previousClip.w <= 0.0001f)
            {
                return 0.0f;
            }
            // 交差点の前フレーム画面UV
            const float2 previousUv = float2(
                previousClip.x / previousClip.w * 0.5f + 0.5f,
                0.5f - previousClip.y / previousClip.w * 0.5f);
            if (previousUv.x < 0.0f || previousUv.x > 1.0f
                || previousUv.y < 0.0f
                || previousUv.y > 1.0f)
            {
                return 0.0f;
            }

            // 現在画面端へのUV距離
            const float2 currentEdge = min(hitUv, 1.0f - hitUv);
            // 前画面端へのUV距離
            const float2 previousEdge =
                min(previousUv, 1.0f - previousUv);
            // 現在と前画面端の最短距離
            const float edgeDistance = min(
                min(currentEdge.x, currentEdge.y),
                min(previousEdge.x, previousEdge.y));
            // 画面端のSSR信頼度
            const float edgeFade = saturate(
                edgeDistance / 0.08f);

            // 交差点までのWorld移動距離
            const float travelled = saturate(worldRatio)
                * length(rayEnd - rayStart);
            // 最大レイ距離に対する移動比
            const float travelledFraction = saturate(
                travelled / maximumDistance);
            // 終端25%のSSR信頼度
            const float distanceFade = saturate(
                (1.0f - travelledFraction) / 0.25f);

            // 反射方向と視線方向の内積
            const float towardCamera = saturate(
                dot(reflection, viewDirection));
            // 視点へ戻るSSRの信頼度
            const float directionFade = saturate(
                (1.0f - towardCamera) / 0.5f);

            // 粗さによるSSR信頼度
            const float roughnessFade = saturate(
                1.0f - roughness / roughnessCutoff);
            // 前フレーム反射の採取RGB
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

        alpha = exitAlpha;
    }
    return 0.0f;
}

// 間接光と2プローブを合成し信頼度でSSRを重ねる(normal: World単位法線, viewDirection: 視点への単位方向, worldPosition: 表面のWorld位置, albedo: 表面RGB, roughness: 表面の粗さ, metallic: 金属度)。
float3 EvaluateEnvironment(
    float3 normal,
    float3 viewDirection,
    float3 worldPosition,
    float3 albedo,
    float roughness,
    float metallic)
{
    // SSR採取RGB・A信頼度
    // SSRは箱射影前の反射方向で探索し、プローブ混合後に信頼度で重ねる。
    const float4 screenReflection =
        EvaluateScreenSpaceReflection(
            worldPosition,
            reflect(-viewDirection, normal),
            viewDirection,
            roughness);

    if (EnvironmentParameters.y < 0.5f)
    {
        // IBLなしの間接光RGB
        float3 flatAmbient =
            EvaluateBakedAmbient(worldPosition, normal)
            * albedo
            * (1.0f - metallic * 0.5f);
        if (screenReflection.a > 0.0f)
        {
            // 金属度による正面反射RGB
            const float3 fresnelZero =
                lerp(0.04f.xxx, albedo, metallic);
            flatAmbient +=
                screenReflection.rgb
                * fresnelZero
                * screenReflection.a;
        }
        return flatAmbient;
    }

    // 事前畳み込み済みの最終Mip
    const float prefilteredMaximumMip =
        EnvironmentParameters.z;
    // 主プローブの最終Mip番号
    float maximumMip = prefilteredMaximumMip;
    if (maximumMip <= 0.0f)
    {
        // 主プローブ画像の幅
        uint width;
        // 主プローブ画像の高さ
        uint height;
        // 主プローブ画像のMip件数
        uint mipCount;
        EnvironmentMap.GetDimensions(
            0,
            width,
            height,
            mipCount);
        maximumMip = max((float)mipCount - 1.0f, 0.0f);
    }

    // 主副混合後の拡散天空RGB
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

    // 主副とSSR混合後の鏡面RGB
    float3 prefiltered = SampleProbeSpecular(
        EnvironmentMap,
        normal,
        viewDirection,
        worldPosition,
        roughness,
        maximumMip,
        ReflectionBoxCenter,
        ReflectionBoxParameters);

    // 副プローブの混合比
    const float blendWeight = ReflectionBlendParameters.x;
    if (blendWeight > 0.0f)
    {
        // 副プローブの最終Mip番号
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

    if (screenReflection.a > 0.0f)
    {
        prefiltered = lerp(
            prefiltered,
            screenReflection.rgb,
            screenReflection.a);
    }

    // 下限付きの法線と視線内積
    const float normalDotView = max(
        dot(normal, viewDirection),
        0.0001f);
    // 金属度による正面反射RGB
    const float3 f0 = lerp(0.04f.xxx, albedo, metallic);

    // BRDF近似の粗さ係数4値
    // LUTを使わずKarisの解析近似でsplit-sumのBRDF項を求める。
    const float4 c0 = float4(
        -1.0f, -0.0275f, -0.572f, 0.022f);
    // BRDF近似の定数係数4値
    const float4 c1 = float4(
        1.0f, 0.0425f, 1.04f, -0.04f);
    // 粗さで混ぜたBRDF係数4値
    const float4 r = roughness * c0 + c1;
    // 視線角込みのBRDF近似項
    const float a004 =
        min(r.x * r.x, exp2(-9.28f * normalDotView))
            * r.x
        + r.y;
    // 反射色倍率と加算のBRDF項
    const float2 brdf =
        float2(-1.04f, 1.04f) * a004 + r.zw;

    // 拡散反射のRGB寄与
    const float3 diffuse =
        irradiance * albedo * (1.0f - metallic);
    // 鏡面反射のRGB寄与
    const float3 specular =
        prefiltered * (f0 * brdf.x + brdf.y);
    return (diffuse + specular)
        * EnvironmentParameters.x
        + EvaluateBakedAmbient(worldPosition, normal)
            * albedo;
}

// 指定Cascadeの3×3 PCFで光の可視率を返す(worldPosition: 表面のWorld位置, normal: World単位法線, cascadeIndex: 0～3のCascade番号)。
float SampleDirectionalShadowCascade(
    float3 worldPosition,
    float3 normal,
    uint cascadeIndex)
{
    // 法線補正後のWorld位置
    const float3 biasedPosition =
        worldPosition
        + normal * ShadowParameters.z;
    // 影投影の同次位置
    const float4 lightPosition = mul(
        float4(biasedPosition, 1.0f),
        ShadowViewProjections[cascadeIndex]);
    // 影透視除算後のXYZ
    const float3 projected =
        lightPosition.xyz
        / max(abs(lightPosition.w), 0.00001f);
    // 影画像の採取UV
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

    // 影による光の可視率
    float visibility = 0.0f;
    // Cascade影の1画素幅
    const float cascadeTexel = ShadowTexelSizes.x;
    // PCFの縦採取オフセット
    [unroll]
    for (int tapY = -1; tapY <= 1; ++tapY)
    {
        // PCFの横採取オフセット
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

// 対象平行光の影を距離で選びCascade末尾10%で混ぜる(worldPosition: 表面のWorld位置, normal: World単位法線, lightIndex: 平行光の番号)。
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

    // カメラ前方向の表面距離
    const float cameraDistance = dot(
        worldPosition - CameraPosition.xyz,
        CameraForward.xyz);
    // 距離で選んだCascade番号
    uint cascadeIndex = 0u;
    // 走査する光源または骨の番号
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

    // 影による光の可視率
    float visibility = SampleDirectionalShadowCascade(
        worldPosition,
        normal,
        cascadeIndex);
    if (cascadeIndex + 1u < LightCounts.w)
    {
        // 1つ前のCascade終端距離
        const float previousSplit = cascadeIndex == 0u
            ? 0.0f
            : ShadowCascadeSplits[cascadeIndex - 1u];
        // 現在Cascadeの距離幅
        const float cascadeRange =
            ShadowCascadeSplits[cascadeIndex]
            - previousSplit;
        // Cascade末尾10%の開始距離
        const float blendStart =
            ShadowCascadeSplits[cascadeIndex]
            - cascadeRange * 0.1f;
        // 次Cascadeへ混ぜる比率
        const float blend = saturate(
            (cameraDistance - blendStart)
            / max(cascadeRange * 0.1f, 0.0001f));
        if (blend > 0.0f)
        {
            // 次Cascadeの光の可視率
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

// 指定スポット影枠を3×3 PCFで採取する(worldPosition: 表面のWorld位置, normal: World単位法線, slot: 0～3のスポット影枠)。
float EvaluateSpotShadow(
    float3 worldPosition,
    float3 normal,
    uint slot)
{
    // スポット影の補正と強度設定
    const float4 parameters =
        SpotShadowParameters[slot];
    if (parameters.w < 0.5f)
    {
        return 1.0f;
    }
    // 法線補正後のWorld位置
    const float3 biasedPosition =
        worldPosition + normal * parameters.y;
    // 影投影の同次位置
    const float4 lightPosition = mul(
        float4(biasedPosition, 1.0f),
        SpotShadowViewProjections[slot]);
    if (lightPosition.w <= 0.0001f)
    {
        return 1.0f;
    }
    // 影透視除算後のXYZ
    const float3 projected =
        lightPosition.xyz / lightPosition.w;
    // 影画像の採取UV
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
    // 影による光の可視率
    float visibility = 0.0f;
    // スポット影の1画素幅
    const float spotTexel = ShadowTexelSizes.y;
    // PCFの縦採取オフセット
    [unroll]
    for (int tapY = -1; tapY <= 1; ++tapY)
    {
        // PCFの横採取オフセット
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

// 対象点光源のCube影を接平面の5点で採取する(worldPosition: 表面のWorld位置, lightIndex: 点光源の番号, lightPosition: 光源のWorld位置, range: 光の到達距離)。
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
    // 点光源から表面への差分
    const float3 fromLight =
        worldPosition - lightPosition;
    // 点光源差分の各軸絶対値
    const float3 absoluteVector = abs(fromLight);
    // Cube面への最大軸距離
    const float majorAxis = max(
        absoluteVector.x,
        max(absoluteVector.y, absoluteVector.z));
    // 影・ClusterのNear距離
    const float nearPlane = 0.1f;
    // Cube影のFar距離
    const float farPlane = max(range, nearPlane + 0.01f);
    // RH透視投影のCube影深度
    const float depth =
        farPlane / (farPlane - nearPlane)
        - farPlane * nearPlane
            / ((farPlane - nearPlane)
                * max(majorAxis, nearPlane));
    // 選択した画面軸の差分
    const float3 direction = normalize(fromLight);
    // 平行を避けるCube影の補助軸
    const float3 axis =
        abs(direction.y) > 0.9f
            ? float3(1.0f, 0.0f, 0.0f)
            : float3(0.0f, 1.0f, 0.0f);
    // 面に沿う接方向
    const float3 tangent =
        normalize(cross(axis, direction));
    // 面に沿う従接方向
    const float3 bitangent =
        cross(direction, tangent);
    // Cube影の2画素ずらし幅
    const float pointTexel =
        ShadowTexelSizes.z * 2.0f;
    // 深度補正後の影比較値
    const float compareDepth =
        depth - PointShadowParameters.y;
    // 影による光の可視率
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

// 材質へ間接光・直接光・発光を加え最後に霧を混ぜる(input: World位置・法線・UV・色)。
float4 PSMain(PixelInput input) : SV_Target
{
    // 法線画像補正後のWorld法線
    const float3 normal = ApplyNormalMap(
        input,
        normalize(input.WorldNormal));
    // 表面から視点への単位方向
    const float3 viewDirection = normalize(
        CameraPosition.xyz - input.WorldPosition);
    // 画像と材質色を掛けたRGBA
    const float4 albedo =
        AlbedoTexture.Sample(MaterialSampler, input.TexCoord)
        * input.Tint;
    // 画像倍率込みの粗さ係数
    float roughnessValue = MaterialParameters.x;
    if (MaterialTextureParameters.x >= 0.5f)
    {
        roughnessValue *= RoughnessTexture.Sample(
            MaterialSampler,
            input.TexCoord).g;
    }
    // 画像倍率込みの金属度係数
    float metallicValue = MaterialParameters.w;
    if (MaterialTextureParameters.y >= 0.5f)
    {
        metallicValue *= MetallicTexture.Sample(
            MaterialSampler,
            input.TexCoord).b;
    }
    // 0.04～1へ制限した粗さ
    const float roughness = clamp(
        roughnessValue,
        0.04f,
        1.0f);
    // 0～1へ制限した金属度
    const float metallic = saturate(metallicValue);

    // 間接光の遮蔽倍率
    // 材質AOとSSAOは間接光だけに掛け、直接光・発光へ適用しない。
    float occlusion = 1.0f;
    if (MaterialTextureParameters.z >= 0.5f)
    {
        // 遮蔽画像のR値
        const float sampled = OcclusionTexture.Sample(
            MaterialSampler,
            input.TexCoord).r;
        occlusion = lerp(
            1.0f,
            sampled,
            saturate(MaterialTextureParameters.w));
    }

    if (ScreenAmbientOcclusionParameters.z >= 0.5f)
    {
        // SSAOを採取する画面UV
        const float2 screenUV =
            input.Position.xy
            * ScreenAmbientOcclusionParameters.xy;
        occlusion *= ScreenAmbientOcclusionTexture.Sample(
            MaterialSampler,
            screenUV).r;
    }

    // 間接・直接光と発光のRGB
    float3 lighting = EvaluateEnvironment(
        normal,
        viewDirection,
        input.WorldPosition,
        albedo.rgb,
        roughness,
        metallic) * occlusion;

    // 走査する光源または骨の番号
    [loop]
    for (uint index = 0; index < min(LightCounts.x, 4u); ++index)
    {
        // 現在計算する光源情報
        const DirectionalLight light = DirectionalLights[index];
        // 現在光源の影による可視率
        const float shadow = EvaluateDirectionalShadow(
            input.WorldPosition,
            normal,
            index);
        // 表面から光源への単位方向
        const float3 toLight =
            normalize(-light.DirectionIntensity.xyz);
        // 平行光源の角半径rad
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

    // Cluster番号はCompute側と同じ画面XYと対数深度で求める。
    if (ClusteredParameters.w >= 0.5f)
    {
        // Clusterの画面横分割数
        const uint gridX = (uint)ClusteredParameters.x;
        // Clusterの画面縦分割数
        const uint gridY = (uint)ClusteredParameters.y;
        // Clusterの深度分割数
        const uint gridZ = (uint)ClusteredParameters.z;
        // 画面内に制限したUV
        const float2 screenRatio = saturate(
            input.Position.xy
            * ClusteredScreenParameters.xy);
        // 画面横のCluster番号
        const uint clusterX = min(
            (uint)(screenRatio.x * gridX),
            gridX - 1u);
        // 画面縦のCluster番号
        const uint clusterY = min(
            (uint)(screenRatio.y * gridY),
            gridY - 1u);
        // 影・ClusterのNear距離
        const float nearPlane =
            ClusteredDepthParameters.x;
        // Near以上の前方向距離
        const float viewDepth = max(
            dot(
                CameraForward.xyz,
                input.WorldPosition
                    - CameraPosition.xyz),
            nearPlane);
        // 対数深度のCluster番号
        const uint clusterZ = min(
            (uint)(log(viewDepth / nearPlane)
                / ClusteredDepthParameters.z
                * gridZ),
            gridZ - 1u);
        // 3軸から平坦化した番号
        const uint cluster =
            clusterZ * gridX * gridY
            + clusterY * gridX
            + clusterX;
        // 1Clusterの灯数上限
        const uint maximumPerCluster =
            (uint)ClusteredDepthParameters.w;
        // Cluster光源列の先頭位置
        const uint clusterOffset =
            cluster * maximumPerCluster;
        // 上限内のCluster光源件数
        const uint clusterLightCount =
            min(ClusterLightCounts[cluster],
                maximumPerCluster);

        // Cluster光源列の走査番号
        [loop]
        for (uint slot = 0;
            slot < clusterLightCount;
            ++slot)
        {
            // 現在計算する光源情報
            const ClusterLight light = ClusterLights[
                ClusterLightIndexList[
                    clusterOffset + slot]];
            // 表面から光源への差分
            const float3 delta =
                light.PositionRange.xyz
                - input.WorldPosition;
            // 表面と光源のWorld距離
            const float distance = length(delta);
            // 下限付きの光到達距離
            const float range = max(
                light.PositionRange.w,
                0.001f);
            // 到達距離による二乗減衰
            const float distanceAttenuation =
                pow(saturate(1.0f - distance / range),
                    2.0f);
            // 表面から光源への単位方向
            const float3 toLight =
                delta / max(distance, 0.0001f);

            // 距離・コーンの光減衰
            float attenuation = distanceAttenuation;
            // 現在光源の影による可視率
            float shadow = 1.0f;
            if (light.ExtraParameters.y < 0.5f)
            {
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
                // 光進行方向と表面方向の内積
                const float cone = dot(
                    normalize(
                        light.DirectionInnerCosine.xyz),
                    -toLight);
                // コーン内外角による光減衰
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
    // 走査する光源または骨の番号
    [loop]
    for (uint index = 0; index < min(LightCounts.y, 16u); ++index)
    {
        // 現在計算する光源情報
        const PointLight light = PointLights[index];
        // 表面から光源への差分
        const float3 delta =
            light.PositionRange.xyz - input.WorldPosition;
        // 表面と光源のWorld距離
        const float distance = length(delta);
        // 下限付きの光到達距離
        const float range = max(light.PositionRange.w, 0.001f);
        // 距離・コーンの光減衰
        const float attenuation =
            pow(saturate(1.0f - distance / range), 2.0f);
        // 現在光源の影による可視率
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

    // 走査する光源または骨の番号
    [loop]
    for (uint index = 0; index < min(LightCounts.z, 8u); ++index)
    {
        // 現在計算する光源情報
        const SpotLight light = SpotLights[index];
        // スポット光源から表面への差分
        const float3 lightToPixel =
            input.WorldPosition - light.PositionRange.xyz;
        // 表面と光源のWorld距離
        const float distance = length(lightToPixel);
        // 下限付きの光到達距離
        const float range = max(light.PositionRange.w, 0.001f);
        // スポット光源からの単位方向
        const float3 rayDirection =
            lightToPixel / max(distance, 0.0001f);
        // 光進行方向と表面方向の内積
        const float cone = dot(
            normalize(light.DirectionInnerCosine.xyz),
            rayDirection);
        // コーン内外角による光減衰
        const float coneAttenuation = smoothstep(
            light.OuterCosinePadding.x,
            light.DirectionInnerCosine.w,
            cone);
        // 到達距離による二乗減衰
        const float distanceAttenuation =
            pow(saturate(1.0f - distance / range), 2.0f);
        // 現在光源の影による可視率
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
    }

    // 画像倍率込みの発光RGB
    float3 emissive = EmissiveParameters.rgb;
    if (EmissiveParameters.w >= 0.5f)
    {
        emissive *= EmissiveTexture.Sample(
            MaterialSampler,
            input.TexCoord).rgb;
    }
    // 発光は影と独立に足し、霧は発光を含む色へ最後に適用する。
    lighting += emissive;

    // 負成分を0へ制限した照明RGB
    const float3 litColor = max(lighting, 0.0f);
    if (FogParameters.w < 0.5f)
    {
        return float4(litColor, albedo.a);
    }
    // 表面とカメラのWorld距離
    const float distanceToCamera =
        length(input.WorldPosition - CameraPosition.xyz);
    // 開始終了距離による霧比
    const float rangeFog = smoothstep(
        FogParameters.x,
        max(FogParameters.y, FogParameters.x + 0.001f),
        distanceToCamera);
    // 密度による指数霧比
    const float exponentialFog =
        1.0f
        - exp(
            -max(FogParameters.z, 0.0f)
            * max(
                distanceToCamera - FogParameters.x,
                0.0f));
    // 距離霧と指数霧の最大比
    const float fogAmount =
        saturate(max(rangeFog, exponentialFog));
    return float4(
        lerp(litColor, FogColor.rgb, fogAmount),
        albedo.a);
}

// DirectXTKのスキン入力を共通構造へ変換して照明を計算する(input: スキンPixel入力)。
float4 PSSkinnedMain(SkinnedPixelInput input) : SV_Target
{
    // 共通構造へ変換したPixel入力
    PixelInput pixel;
    pixel.Position = input.Position;
    pixel.WorldPosition = input.WorldPosition.xyz;
    pixel.WorldNormal = input.WorldNormal;
    pixel.TexCoord = input.TexCoord;
    // DirectXTKのDiffuseと材質色を二重乗算しないようMaterialColorを使う。
    pixel.Tint = MaterialColor;
    return PSMain(pixel);
}
