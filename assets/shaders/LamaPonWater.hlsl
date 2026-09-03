// 水面の見本Shaderです。
//
// ノイズで波を作り、その波の傾き（法線）で「空の映り込み」と
// 「太陽の反射」を計算します。平らな板メッシュへマテリアルとして
// 割り当てるだけで動きます。スクリプトは要りません。
//
// 中で何をしているか（上から順に）:
//   ① ノイズを2枚重ねて波の高さを作る（大きなうねり＋細かいさざ波）
//   ② 高さの傾きから法線を出す（隣を測って引き算するだけ）
//   ③ 見る角度で「映り込みの強さ」を決める（フレネル。水面を
//      真上から見ると透けて、浅い角度で見ると鏡になる現象）
//   ④ 空の色を映す＋太陽をGGXで反射する
//
// ★太陽の向きと色はシーンのDirectional Lightから読んでいます
//   （下のLightingBufferの宣言）。ライトを回すと反射も動きます。
//   太陽の「角度の大きさ」も一緒に読むので、ハイライトは点ではなく
//   本物と同じ0.53度の円盤の大きさになります。

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
    float4 MaterialTextureParameters;
    float4 EmissiveParameters;
    // x=経過秒（エンジンが入れます）。これで波が勝手に動きます。
    float4 TimeParameters;
};

// シーンのライトです。エンジンがb1へ入れているので、自作Shaderからも
// 「必要なところまで宣言して、そこで止める」だけで読めます。
// （並びはLamaPonLit.hlslのLightingBufferと同じにしてください）
struct DirectionalLight
{
    // xyz=光が進む向き, w=強さ。
    float4 DirectionIntensity;
    // rgb=色, w=太陽の角半径（ラジアン）。
    float4 Color;
};

cbuffer LightingBuffer : register(b1)
{
    float4 Ambient;
    uint4 LightCounts;
    DirectionalLight DirectionalLights[4];
};

cbuffer BoneBuffer : register(b2)
{
    float4x3 BoneTransforms[72];
};

TextureCube EnvironmentMap : register(t3);
SamplerState MaterialSampler : register(s0);

static const float WaterPi = 3.14159265f;

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

PixelInput BuildPixelInput(
    const float3 position,
    const float3 normal,
    const float2 texCoord)
{
    PixelInput output;
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

PixelInput VSMain(VertexInput input)
{
    return BuildPixelInput(
        input.Position,
        input.Normal,
        input.TexCoord);
}

// スキン付きモデルへ割り当てられたときのための入口です。水面を骨で
// 動かすことはまずありませんが、これが無いとコンパイルに失敗して
// 「なぜ映らないのか分からない」状態になるので用意しています。
PixelInput VSSkinnedMain(SkinnedVertexInput input)
{
    float4x3 skinning = 0.0f;
    [unroll]
    for (uint index = 0u; index < 4u; ++index)
    {
        const uint bone = min(input.BlendIndices[index], 71u);
        skinning +=
            BoneTransforms[bone] * input.BlendWeights[index];
    }
    return BuildPixelInput(
        mul(float4(input.Position, 1.0f), skinning),
        normalize(mul(input.Normal, (float3x3)skinning)),
        input.TexCoord);
}

// ---- ① 波の高さ ----
// ワールドのXZで測ります（UVではなく）。板を並べても模様が途切れず、
// 板を大きくしても波の大きさが変わりません。
float WaveHeight(const float2 position)
{
    const float time = TimeParameters.x;
    const float speed = CustomParameters[0].w;
    const float swellScale = max(CustomParameters[0].x, 0.001f);
    const float rippleScale = max(CustomParameters[0].y, 0.001f);

    // 大きなうねりと細かいさざ波を、別々の向きへ流します。
    // 同じ向きだと模様がそのまま平行移動して見えて、水に見えません。
    const float swell = LamaPonFractalPerlin2D(
        position * swellScale
            + float2(0.42f, 0.18f) * (time * speed),
        3,
        2.0f,
        0.5f);
    const float ripple = LamaPonFractalPerlin2D(
        position * rippleScale
            + float2(-0.23f, 0.51f) * (time * speed * 1.7f),
        2,
        2.0f,
        0.5f);
    return swell * 0.75f + ripple * 0.25f;
}

// ---- ② 高さの傾きから法線を出す ----
// 隣り合う4点の高さを測って引き算するだけです（中心差分）。
// ノイズの式を微分しなくてよいので、波の作り方を変えても
// ここは書き換えずに済みます。
float3 WaveNormal(
    const float2 position,
    const float3 geometricNormal)
{
    // 測る間隔はさざ波1つぶんの5%です。ここが粗いと山と谷を
    // またいでしまい、傾きが平均化されて水面が平らになります
    // （35%にしていたとき、波はあるのに鏡のような一枚板に
    // 見えました。絵で見るまで気付けない類の間違いです）。
    const float step =
        0.05f / max(CustomParameters[0].y, 0.001f);
    const float heightRight =
        WaveHeight(position + float2(step, 0.0f));
    const float heightLeft =
        WaveHeight(position - float2(step, 0.0f));
    const float heightForward =
        WaveHeight(position + float2(0.0f, step));
    const float heightBack =
        WaveHeight(position - float2(0.0f, step));

    const float amplitude = max(CustomParameters[0].z, 0.0f);
    const float2 gradient = float2(
        (heightRight - heightLeft) / (2.0f * step),
        (heightForward - heightBack) / (2.0f * step))
        * amplitude;

    // Y上向きで作った法線を、板の実際の向きへ載せ替えます。
    // 板が水平（法線が+Y）なら right=+X／forward=+Z になり、
    // ワールドのXZとぴったり一致します。
    const float3 up = normalize(geometricNormal);
    const float3 helper = abs(up.y) > 0.99f
        ? float3(0.0f, 0.0f, 1.0f)
        : float3(0.0f, 1.0f, 0.0f);
    const float3 right = normalize(cross(up, helper));
    // 3本目は必ず right × up です。cross(up, right) と書くと
    // 行列式が−1（鏡像）になり、波が裏返って光が逆から来ます。
    const float3 forward = cross(right, up);

    return normalize(
        right * (-gradient.x)
        + up
        + forward * (-gradient.y));
}

// ---- ④ 太陽の反射（GGX＋太陽の角度の大きさ） ----
// 太陽は点ではなく、空に0.53度の円盤として見えています。点として
// 扱うと、つるつるの水面ではハイライトが1画素の点になってしまい、
// 「きらきら光る帯」になりません。
//
// やり方は代表点法です。反射の向きが太陽の円盤から外れているときだけ、
// 円盤の縁の一番近い点へ寄せます。そのぶん明るくなりすぎるので、
// 広がった割合で割って戻します（エネルギー保存）。
float3 SunSpecular(
    const float3 normal,
    const float3 viewDirection,
    const float3 toSun,
    const float3 sunColor,
    const float sunAngularRadius,
    const float roughness)
{
    const float alpha = max(roughness * roughness, 1.0e-4f);

    // 反射ベクトルを太陽の円盤へ寄せた「代表点」。
    const float3 reflected =
        reflect(-viewDirection, normal);
    const float sunDotReflected = dot(toSun, reflected);
    const float diskCosine = cos(sunAngularRadius);
    float3 representative = reflected;
    if (sunDotReflected < diskCosine)
    {
        const float3 sideways =
            reflected - sunDotReflected * toSun;
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

    const float normalDotLight = saturate(dot(normal, toSun));
    if (normalDotLight <= 0.0f)
    {
        return 0.0f.xxx;
    }
    const float normalDotView =
        max(dot(normal, viewDirection), 1.0e-4f);
    const float3 halfVector =
        normalize(representative + viewDirection);
    const float normalDotHalf =
        saturate(dot(normal, halfVector));
    const float viewDotHalf =
        saturate(dot(viewDirection, halfVector));

    // GGXの法線分布。
    const float alphaSquared = alpha * alpha;
    const float denominator =
        normalDotHalf * normalDotHalf * (alphaSquared - 1.0f)
        + 1.0f;
    // 下限は「絶対に効かない」ほど小さくします。1.0e-4だと鋭い
    // ハイライトほど分母が小さくなって常に下限へ張り付き、太陽の
    // 反射が消えます（LamaPonLit.hlslでも同じ間違いがありました）。
    const float distribution = alphaSquared
        / max(WaterPi * denominator * denominator, 1.0e-12f);

    // 代表点へ寄せたぶんのエネルギー補正。円盤の広がりを粗さへ
    // 足した値との比で割ります。
    const float widened =
        saturate(alpha + sin(sunAngularRadius) * 0.5f);
    const float energy =
        (alpha / max(widened, 1.0e-4f))
        * (alpha / max(widened, 1.0e-4f));

    // 幾何減衰（Smith-Schlick）とフレネル（水のF0は0.02）。
    const float k = alpha * 0.5f + 1.0e-4f;
    const float geometry =
        (normalDotView / (normalDotView * (1.0f - k) + k))
        * (normalDotLight
            / (normalDotLight * (1.0f - k) + k));
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

// 空の色。Skyboxが無くても水面が黒くならないよう、上（天頂）と
// 地平の2色から作ります。Skyboxやリフレクションプローブがある
// シーンでは「空の映り込み」を上げると実際の空が混ざります。
float3 SkyColor(const float3 direction, const float mixEnvironment)
{
    const float upward = saturate(direction.y * 0.5f + 0.5f);
    const float3 gradient = lerp(
        CustomParameters[5].rgb,
        CustomParameters[4].rgb,
        pow(upward, 0.6f));
    if (mixEnvironment <= 0.0f)
    {
        return gradient;
    }
    const float3 sampled =
        EnvironmentMap.SampleLevel(
            MaterialSampler,
            direction,
            0.0f).rgb;
    return lerp(gradient, sampled, saturate(mixEnvironment));
}

float4 ShadeWater(const PixelInput input)
{
    const float3 viewDirection = normalize(
        CameraPosition.xyz - input.WorldPosition);
    // 両面描画（cull: none）なので、板の裏を見ていることがあります。
    // 法線が向こう向きのままだと、空の映り込みは下向きを引いて
    // ずっと地平の色になり、太陽は「水平線の下」と判定されて
    // 反射が丸ごと消えます（真っ平らな水色の板に見えます）。
    // 見ている側へ向け直してから波を乗せます。
    const float3 facingNormal =
        dot(input.WorldNormal, viewDirection) < 0.0f
            ? -input.WorldNormal
            : input.WorldNormal;
    const float3 normal = WaveNormal(
        input.WorldPosition.xz,
        facingNormal);

    // ---- ③ フレネル ----
    // 真上から覗くと水の中が見え、浅い角度では空が映る、という
    // 見え方の切り替わりです。水のF0（正面から見た反射率）は0.02。
    const float viewDotNormal =
        saturate(dot(normal, viewDirection));
    const float fresnel =
        0.02f + (1.0f - 0.02f)
            * pow(1.0f - viewDotNormal, 5.0f);

    // 水の色。浅い角度ほど「深い色」に見えるようにして、
    // 奥行きを感じさせます（本物は水を通る距離が長くなるため）。
    const float3 waterColor = lerp(
        CustomParameters[3].rgb,
        CustomParameters[2].rgb,
        pow(viewDotNormal, 0.7f));

    const float3 reflected = reflect(-viewDirection, normal);
    const float3 sky = SkyColor(
        reflected,
        CustomParameters[1].w);

    float3 color = lerp(waterColor, sky, fresnel);

    // 太陽。シーンに方向光があればそれを、無ければ斜め上から。
    float3 toSun = normalize(float3(0.35f, 0.75f, 0.4f));
    float3 sunColor = float3(1.0f, 0.96f, 0.88f);
    float sunAngularRadius = 0.00465f;
    if (LightCounts.x >= 1u)
    {
        const DirectionalLight sun = DirectionalLights[0];
        toSun = normalize(-sun.DirectionIntensity.xyz);
        sunColor = sun.Color.rgb * sun.DirectionIntensity.w;
        // 角半径が入っていない古いシーンでは0が来るので、
        // そのときは本物の太陽（0.53度＝角半径0.00465）にします。
        sunAngularRadius = sun.Color.w > 1.0e-5f
            ? sun.Color.w
            : 0.00465f;
    }
    // ここで言う「ざらつき」は、画素より細かくて描き切れない
    // さざ波の代わりです。0に近づけて完全な鏡にすると、太陽の
    // 0.53度は反射の散らばり（波の傾きぶん±40度ほど）に対して
    // 狭すぎて、光の帯が数画素へ落ちるか丸ごと消えます。
    // 少しざらつかせるのが正解で、これで「きらきら光る帯」に
    // なります。
    const float roughness = max(CustomParameters[1].x, 0.02f);
    const float3 sunTerm = SunSpecular(
        normal,
        viewDirection,
        toSun,
        sunColor,
        sunAngularRadius,
        roughness)
        * max(CustomParameters[1].y, 0.0f);
    color += sunTerm;

    // きらめき。細かい波の面が太陽を向いた瞬間だけ光る、水面特有の
    // ちらちらです。Worleyノイズの粒を「小さな面の向き」に見立てて、
    // 太陽へ向いている粒だけを光らせます。
    const float sparkleAmount = max(CustomParameters[1].z, 0.0f);
    if (sparkleAmount > 0.0f)
    {
        const float time = TimeParameters.x;
        const float2 sparkleUV =
            input.WorldPosition.xz
                * max(CustomParameters[0].y, 0.001f) * 6.0f
            + float2(0.17f, -0.31f)
                * (time * CustomParameters[0].w);
        const float cells =
            1.0f - LamaPonWorleyNoise2D(sparkleUV);
        const float aligned =
            saturate(dot(normal, normalize(toSun + viewDirection)));
        const float sparkle =
            pow(cells, 12.0f) * pow(aligned, 24.0f);
        color += sunColor * sparkle * sparkleAmount;
    }

    // 不透明度もフレネルで動かします。真上から見ると底が透けて、
    // 浅い角度では空を映して見通せなくなります。
    const float baseAlpha = saturate(CustomParameters[2].a);
    const float alpha = saturate(
        lerp(baseAlpha, 1.0f, fresnel) * MaterialColor.a);
    return float4(color, alpha);
}

float4 PSMain(PixelInput input) : SV_Target
{
    return ShadeWater(input);
}

float4 PSSkinnedMain(SkinnedPixelInput input) : SV_Target
{
    PixelInput pixel;
    pixel.Position = input.Position;
    pixel.WorldPosition = input.WorldPosition.xyz;
    pixel.WorldNormal = input.WorldNormal;
    pixel.TexCoord = input.TexCoord;
    return ShadeWater(pixel);
}
