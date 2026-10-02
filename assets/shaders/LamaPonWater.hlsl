#include "LamaPonNoise.hlsli"

/* LAMAPON_RENDER_STATE
{ "blend": "alpha", "cull": "none", "depthWrite": false }
*/

/* LAMAPON_PROPERTIES
[
  { "target": "0.x", "type": "float", "name": "うねりの細かさ",
    "min": 0.01, "max": 2.0, "default": 0.25 },
  { "target": "0.y", "type": "float", "name": "さざ波の細かさ",
    "min": 0.05, "max": 8.0, "default": 1.1 },
  { "target": "0.z", "type": "float", "name": "波の高さ",
    "min": 0.0, "max": 3.0, "default": 1.0 },
  { "target": "0.w", "type": "float", "name": "流れる速さ",
    "min": 0.0, "max": 3.0, "default": 0.6 },
  { "target": "1.x", "type": "float", "name": "水面のざらつき",
    "min": 0.02, "max": 0.6, "default": 0.35 },
  { "target": "1.y", "type": "float", "name": "太陽の輝き",
    "min": 0.0, "max": 8.0, "default": 1.0 },
  { "target": "1.z", "type": "float", "name": "きらめき",
    "min": 0.0, "max": 2.0, "default": 0.35 },
  { "target": "1.w", "type": "float", "name": "空の映り込み（要Skybox）",
    "min": 0.0, "max": 1.0, "default": 0.0 },
  { "target": "2.rgb", "type": "color", "name": "浅いところの色",
    "default": [0.10, 0.42, 0.45] },
  { "target": "2.a", "type": "float", "name": "水の濃さ（不透明度）",
    "min": 0.0, "max": 1.0, "default": 0.72 },
  { "target": "3.rgb", "type": "color", "name": "深いところの色",
    "default": [0.01, 0.09, 0.16] },
  { "target": "4.rgb", "type": "color", "name": "空（真上）の色",
    "default": [0.24, 0.45, 0.85] },
  { "target": "5.rgb", "type": "color", "name": "空（地平）の色",
    "default": [0.72, 0.82, 0.93] }
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
    // 最終Alphaに使う材質色
    float4 MaterialColor;
    // World視点XYZ・W予約
    float4 CameraPosition;
    // 互換配置の視線方向
    float4 CameraForward;
    // 互換配置の材質設定
    float4 MaterialParameters;
    // 波・反射・水色・天空色の設定
    float4 CustomParameters[8];
    // 互換配置の材質画像設定
    float4 MaterialTextureParameters;
    // 互換配置の発光設定
    float4 EmissiveParameters;

    // 1時間周期秒・差分秒・フレーム数
    float4 TimeParameters;
};

// LightingBufferはLamaPonLit.hlslと同じ順序で、使う範囲まで宣言する。
struct DirectionalLight
{

    // 光の進行方向XYZ・W強度
    float4 DirectionIntensity;

    // 光RGB・W太陽角半径rad
    float4 Color;
};

// CPUと配置を揃える光源定数
cbuffer LightingBuffer : register(b1)
{
    // 互換配置の環境光
    float4 Ambient;
    // X平行・Y点・Zスポット数
    uint4 LightCounts;
    // 先頭を太陽に使う4本の平行光
    DirectionalLight DirectionalLights[4];
};

// スキン変形用の骨定数
cbuffer BoneBuffer : register(b2)
{
    // 最大72骨の変形行列
    float4x3 BoneTransforms[72];
};

// 反射方向で採取する天空画像
TextureCube EnvironmentMap : register(t3);
// 天空画像の採取設定
SamplerState MaterialSampler : register(s0);

// GGX分布に使う円周率
static const float WaterPi = 3.14159265f;

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
    const float3 position,
    const float3 normal,
    const float2 texCoord)
{
    // World変換後のPixel入力
    PixelInput output;
    // World空間の同次位置
    const float4 worldPosition =
        mul(float4(position, 1.0f), World);
    output.Position = mul(worldPosition, ViewProjection);
    output.WorldPosition = worldPosition.xyz;
    output.WorldNormal = normalize(
        mul(float4(normal, 0.0f),
            WorldInverseTranspose).xyz);
    output.TexCoord = texCoord;
    return output;
}

// 通常頂点を水面のPixel入力へ変換する(input: 通常頂点)。
PixelInput VSMain(VertexInput input)
{
    return BuildPixelInput(
        input.Position,
        input.Normal,
        input.TexCoord);
}


// 最大4骨を加重合成して水面のPixel入力へ変換する(input: スキン頂点)。
PixelInput VSSkinnedMain(SkinnedVertexInput input)
{
    // 4骨の加重合成行列
    float4x3 skinning = 0.0f;
    // 合成する骨影響の番号
    [unroll]
    for (uint index = 0u; index < 4u; ++index)
    {
        // 0～71へ制限した骨番号
        const uint bone = min(input.BlendIndices[index], 71u);
        skinning +=
            BoneTransforms[bone] * input.BlendWeights[index];
    }
    return BuildPixelInput(
        mul(float4(input.Position, 1.0f), skinning),
        normalize(mul(input.Normal, (float3x3)skinning)),
        input.TexCoord);
}


// WorldのXZで2層の移動ノイズを採取する(position: WorldのXZ位置)。
float WaveHeight(const float2 position)
{
    // 1時間周期の経過秒
    const float time = TimeParameters.x;
    // 波の模様の移動速度
    const float speed = CustomParameters[0].w;
    // うねりのWorld座標倍率
    const float swellScale = max(CustomParameters[0].x, 0.001f);
    // さざ波のWorld座標倍率
    const float rippleScale = max(CustomParameters[0].y, 0.001f);


    // 3段階のうねりノイズ
    const float swell = LamaPonFractalPerlin2D(
        position * swellScale
            + float2(0.42f, 0.18f) * (time * speed),
        3,
        2.0f,
        0.5f);
    // 2段階のさざ波ノイズ
    const float ripple = LamaPonFractalPerlin2D(
        position * rippleScale
            + float2(-0.23f, 0.51f) * (time * speed * 1.7f),
        2,
        2.0f,
        0.5f);
    return swell * 0.75f + ripple * 0.25f;
}


// 中心差分の波の傾きを面の向きへ合成する(position: WorldのXZ位置, geometricNormal: 視点側のWorld法線)。
float3 WaveNormal(
    const float2 position,
    const float3 geometricNormal)
{
    // さざ波の周期の5%を中心差分の間隔とし、山谷の平均化を抑える。
    // 中心差分のWorld間隔
    const float step =
        0.05f / max(CustomParameters[0].y, 0.001f);
    // XZの正X側の高さ
    const float heightRight =
        WaveHeight(position + float2(step, 0.0f));
    // XZの負X側の高さ
    const float heightLeft =
        WaveHeight(position - float2(step, 0.0f));
    // XZの正Z側の高さ
    const float heightForward =
        WaveHeight(position + float2(0.0f, step));
    // XZの負Z側の高さ
    const float heightBack =
        WaveHeight(position - float2(0.0f, step));

    // 法線へ反映する波の振幅
    const float amplitude = max(CustomParameters[0].z, 0.0f);
    // 中心差分の波勾配
    const float2 gradient = float2(
        (heightRight - heightLeft) / (2.0f * step),
        (heightForward - heightBack) / (2.0f * step))
        * amplitude;


    // 面の単位幾何法線
    const float3 up = normalize(geometricNormal);
    // 平行を避けた補助軸
    const float3 helper = abs(up.y) > 0.99f
        ? float3(0.0f, 0.0f, 1.0f)
        : float3(0.0f, 1.0f, 0.0f);
    // 面に沿う右単位方向
    const float3 right = normalize(cross(up, helper));
    // 鏡像になる逆順を避け、forwardはright×upで作る。
    // 面に沿う前単位方向
    const float3 forward = cross(right, up);

    return normalize(
        right * (-gradient.x)
        + up
        + forward * (-gradient.y));
}


// 太陽円盤の代表点からGGX反射と広がり補正を求める(normal: 表面の単位法線, viewDirection: 視点への単位方向, toSun: 太陽への単位方向, sunColor: 強度込みの太陽RGB, sunAngularRadius: 太陽角半径rad, roughness: 表面の粗さ)。
float3 SunSpecular(
    const float3 normal,
    const float3 viewDirection,
    const float3 toSun,
    const float3 sunColor,
    const float sunAngularRadius,
    const float roughness)
{
    // 二乗粗さの下限付きGGX幅
    const float alpha = max(roughness * roughness, 1.0e-4f);


    // 視線を法線で反射した方向
    const float3 reflected =
        reflect(-viewDirection, normal);
    // 太陽と反射方向の内積
    const float sunDotReflected = dot(toSun, reflected);
    // 太陽角半径の余弦
    const float diskCosine = cos(sunAngularRadius);
    // 太陽円盤内の代表光方向
    float3 representative = reflected;
    if (sunDotReflected < diskCosine)
    {
        // 太陽方向に直交する反射成分
        const float3 sideways =
            reflected - sunDotReflected * toSun;
        // 直交する反射成分の長さ
        const float sidewaysLength = length(sideways);
        if (sidewaysLength > 1.0e-5f)
        {
            representative = normalize(
                toSun * diskCosine
                + (sideways / sidewaysLength)
                    * sin(sunAngularRadius));
        }
        else
        {
            representative = toSun;
        }
    }

    // 法線と太陽方向の内積
    const float normalDotLight = saturate(dot(normal, toSun));
    if (normalDotLight <= 0.0f)
    {
        return 0.0f.xxx;
    }
    // 下限付きの法線と視線内積
    const float normalDotView =
        max(dot(normal, viewDirection), 1.0e-4f);
    // 代表光と視線の中間方向
    const float3 halfVector =
        normalize(representative + viewDirection);
    // 法線と中間方向の内積
    const float normalDotHalf =
        saturate(dot(normal, halfVector));
    // 視線と中間方向の内積
    const float viewDotHalf =
        saturate(dot(viewDirection, halfVector));


    // GGX粗さの二乗
    const float alphaSquared = alpha * alpha;
    // GGX分母の中間項
    const float denominator =
        normalDotHalf * normalDotHalf * (alphaSquared - 1.0f)
        + 1.0f;
    // GGX分母の下限を1e-12に保ち、鋭い反射の減衰を抑える。
    // GGXの法線分布密度
    const float distribution = alphaSquared
        / max(WaterPi * denominator * denominator, 1.0e-12f);


    // 太陽角半径で広げた粗さ
    const float widened =
        saturate(alpha + sin(sunAngularRadius) * 0.5f);
    // 代表点の反射量補正比
    const float energy =
        (alpha / max(widened, 1.0e-4f))
        * (alpha / max(widened, 1.0e-4f));


    // Smith減衰の粗さ係数
    const float k = alpha * 0.5f + 1.0e-4f;
    // Smith-Schlick幾何減衰
    const float geometry =
        (normalDotView / (normalDotView * (1.0f - k) + k))
        * (normalDotLight
            / (normalDotLight * (1.0f - k) + k));
    // 水のF0を使う反射率
    const float fresnel = 0.02f
        + (1.0f - 0.02f) * pow(1.0f - viewDotHalf, 5.0f);

    return sunColor
        * distribution
        * energy
        * geometry
        * fresnel
        * normalDotLight
        / max(4.0f * normalDotView * normalDotLight, 1.0e-4f);
}


// 天空の勾配色へ環境画像を混合する(direction: 採取する単位方向, mixEnvironment: 環境画像の混合比)。
float3 SkyColor(const float3 direction, const float mixEnvironment)
{
    // 上下方向の天空補間比
    const float upward = saturate(direction.y * 0.5f + 0.5f);
    // 上下方向の天空勾配RGB
    const float3 gradient = lerp(
        CustomParameters[5].rgb,
        CustomParameters[4].rgb,
        pow(upward, 0.6f));
    if (mixEnvironment <= 0.0f)
    {
        return gradient;
    }
    // 天空画像の採取RGB
    const float3 sampled =
        EnvironmentMap.SampleLevel(
            MaterialSampler,
            direction,
            0.0f).rgb;
    return lerp(gradient, sampled, saturate(mixEnvironment));
}

// 波状法線で天空・太陽・きらめきとAlphaを合成する(input: World位置・法線とUV)。
float4 ShadeWater(const PixelInput input)
{
    // 表面から視点への単位方向
    const float3 viewDirection = normalize(
        CameraPosition.xyz - input.WorldPosition);
    // 裏面でも反射できるよう、幾何法線を視点側へ向けてから波を合成する。
    // 視点側へ向けた幾何法線
    const float3 facingNormal =
        dot(input.WorldNormal, viewDirection) < 0.0f
            ? -input.WorldNormal
            : input.WorldNormal;
    // 波の傾きを合成した法線
    const float3 normal = WaveNormal(
        input.WorldPosition.xz,
        facingNormal);


    // 視線と波の法線の内積
    const float viewDotNormal =
        saturate(dot(normal, viewDirection));
    // 水のF0を使う反射率
    const float fresnel =
        0.02f + (1.0f - 0.02f)
            * pow(1.0f - viewDotNormal, 5.0f);


    // 視線角で混ぜた浅深の水色
    const float3 waterColor = lerp(
        CustomParameters[3].rgb,
        CustomParameters[2].rgb,
        pow(viewDotNormal, 0.7f));

    // 視線を法線で反射した方向
    const float3 reflected = reflect(-viewDirection, normal);
    // 反射方向の天空RGB
    const float3 sky = SkyColor(
        reflected,
        CustomParameters[1].w);

    // 反射と輝きを合成するRGB
    float3 color = lerp(waterColor, sky, fresnel);


    // 表面から太陽への単位方向
    float3 toSun = normalize(float3(0.35f, 0.75f, 0.4f));
    // 強度込みの太陽RGB
    float3 sunColor = float3(1.0f, 0.96f, 0.88f);
    // 太陽の角半径rad
    float sunAngularRadius = 0.00465f;
    if (LightCounts.x >= 1u)
    {
        // 先頭の平行光
        const DirectionalLight sun = DirectionalLights[0];
        toSun = normalize(-sun.DirectionIntensity.xyz);
        sunColor = sun.Color.rgb * sun.DirectionIntensity.w;
        // 角半径未設定の旧シーンでは0.00465radを使う。
        sunAngularRadius = sun.Color.w > 1.0e-5f
            ? sun.Color.w
            : 0.00465f;
    }

    // 下限0.02の反射粗さ
    const float roughness = max(CustomParameters[1].x, 0.02f);
    // 輝き倍率込みの太陽反射RGB
    const float3 sunTerm = SunSpecular(
        normal,
        viewDirection,
        toSun,
        sunColor,
        sunAngularRadius,
        roughness)
        * max(CustomParameters[1].y, 0.0f);
    color += sunTerm;


    // きらめきの強度倍率
    const float sparkleAmount = max(CustomParameters[1].z, 0.0f);
    if (sparkleAmount > 0.0f)
    {
        // 1時間周期の経過秒
        const float time = TimeParameters.x;
        // 移動後の粒ノイズ採取座標
        const float2 sparkleUV =
            input.WorldPosition.xz
                * max(CustomParameters[0].y, 0.001f) * 6.0f
            + float2(0.17f, -0.31f)
                * (time * CustomParameters[0].w);
        // Worley距離を反転した粒値
        const float cells =
            1.0f - LamaPonWorleyNoise2D(sparkleUV);
        // 法線と太陽視線中間の内積
        const float aligned =
            saturate(dot(normal, normalize(toSun + viewDirection)));
        // 粒と向きによる輝き係数
        const float sparkle =
            pow(cells, 12.0f) * pow(aligned, 24.0f);
        color += sunColor * sparkle * sparkleAmount;
    }

    // 浅い視線角ほど不透明にし、最後に材質Alphaを掛ける。
    // 設定水色のAlpha
    const float baseAlpha = saturate(CustomParameters[2].a);
    // 材質倍率込みの最終Alpha
    const float alpha = saturate(
        lerp(baseAlpha, 1.0f, fresnel) * MaterialColor.a);
    return float4(color, alpha);
}

// 通常Meshへ水面材質を適用する(input: 通常Pixel入力)。
float4 PSMain(PixelInput input) : SV_Target
{
    return ShadeWater(input);
}

// スキン入力を共通構造へ変換して水面材質を適用する(input: スキンPixel入力)。
float4 PSSkinnedMain(SkinnedPixelInput input) : SV_Target
{
    // 共通構造へ変換したPixel入力
    PixelInput pixel;
    pixel.Position = input.Position;
    pixel.WorldPosition = input.WorldPosition.xyz;
    pixel.WorldNormal = input.WorldNormal;
    pixel.TexCoord = input.TexCoord;
    return ShadeWater(pixel);
}
