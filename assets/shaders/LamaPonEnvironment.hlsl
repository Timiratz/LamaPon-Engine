// 深度→距離・ビュー空間位置・法線の式はここにしかありません。
// 自作のScreenEffectも同じファイルを取り込みます（同じ式を2箇所へ
// 書くと、片方だけ直したときに無言で絵が食い違うため）。
#include "LamaPonScreenDepth.hlsli"

cbuffer SkyBuffer : register(b0)
{
    row_major float4x4 InverseViewProjection;
    float4 CameraPosition;
    float4 TopColor;
    float4 HorizonColor;
    float4 GroundColor;
    // x=キューブマップ使用, y/z/w=予約
    float4 SkyOptions;
    // 空に描く太陽。xyz=太陽へ向かう向き, w=角半径（ラジアン）。
    // 末尾に足しているので、この行を持たない古い環境Shaderも
    // そのまま動きます。
    float4 SunDirection;
    // rgb=太陽の色×強さ, w=0より大きければ描く。
    float4 SunDiskColor;
};

cbuffer BloomBuffer : register(b1)
{
    float2 TexelSize;
    float BloomThreshold;
    float BloomIntensity;
    float BloomRadius;
    float3 BloomPadding;
};

cbuffer LensFlareBuffer : register(b7)
{
    // xy=1/画面サイズ, z=しきい値, w=全体の強さ
    float4 LensFlarePrimary;
    // x=ゴースト, y=ハローの位置, z=色分散, w=筋の強さ
    float4 LensFlareSecondary;
    // x=筋の長さ, y/z/w=予約
    float4 LensFlareTertiary;
    // 筋の多段ぼかし用。x=1回のタップ間隔（UV）, y=方向の本数,
    // z=1本目の角度（ラジアン）, w=1なら最初の回（高輝度の抽出も行う）。
    float4 LensFlareStreakPass;
};

cbuffer PrefilterBuffer : register(b3)
{
    // x=キューブ面(0-5), y=粗さ, z=ソース解像度, w=予約
    float4 PrefilterParameters;
};

cbuffer ColorGradingBuffer : register(b2)
{
    // exposure, contrast, saturation, temperature
    float4 ColorGradePrimary;
    // tint, vignette, grading enabled, 自動露出の補正（段数）。
    // 最後の欄は以前は未使用の詰め物だったので、並びは変わって
    // いません。
    float4 ColorGradeSecondary;
};

cbuffer AmbientOcclusionBuffer : register(b4)
{
    // x=1/幅, y=1/高さ, z=遮蔽を探す半径(ワールド単位), w=強さ(0-1)
    float4 AmbientOcclusionParameters;
    // 深度からビュー空間位置へ戻すための射影の値。
    // x=projection._33, y=projection._43,
    // z=1/projection._11, w=1/projection._22
    float4 AmbientOcclusionProjection;
    // x=サンプル数（品質設定から）, y/z/w=予約
    float4 AmbientOcclusionQuality;
};

// ボリュメトリックライト（光の筋）。
cbuffer VolumetricBuffer : register(b5)
{
    // 深度→ワールド座標の復元に使う逆ビュー射影。
    row_major float4x4 VolumetricInverseViewProjection;
    // xyz=カメラのワールド位置, w=最大距離。
    float4 VolumetricCameraPosition;
    // xyz=光の向き（光源から出る向き）, w=サンプル数。
    float4 VolumetricLightDirection;
    // rgb=光の色×強度, w=前方散乱の強さ。
    float4 VolumetricLightColor;
    // カスケードのビュー射影（Litシェーダーと同じ並び）。
    row_major float4x4 VolumetricCascades[4];
    // x=カスケード数, y=影のバイアス, z=1/シャドウ解像度,
    // w=予約。
    float4 VolumetricShadowParameters;
};

// TAA。
cbuffer TemporalBuffer : register(b6)
{
    // 深度→ワールド座標の復元に使う逆ビュー射影（今のフレーム。
    // ずらしを含んだままの行列でないと深度と噛み合いません）。
    row_major float4x4 TemporalInverseViewProjection;
    // 前フレームのビュー射影（ずらしを含まないもの）。
    row_major float4x4 TemporalPreviousViewProjection;
    // x=履歴を残す比率, y=近傍クランプの緩さ,
    // z=1/画面幅, w=1/画面高さ。
    float4 TemporalParameters;
};

// 被写界深度（DoF）。
cbuffer DepthOfFieldBuffer : register(b8)
{
    // x=ピントの合う距離, y=ピントの合う幅, z=ぼけの強さ,
    // w=ぼけ半径の上限（**フル解像度の**画素数）。
    float4 DepthOfFieldParameters;
    // 深度をビュー空間のZ（カメラからの距離）へ戻すための射影の値。
    // x=projection._33, y=projection._43, z/w=予約。
    float4 DepthOfFieldProjection;
    // x=1/幅, y=1/高さ（**そのパスの**解像度）, z=サンプル数,
    // w=予約。
    float4 DepthOfFieldTexel;
};

// モーションブラー（カメラの動きによるブレ）。
cbuffer MotionBlurBuffer : register(b9)
{
    // 深度→ワールド座標の復元に使う逆ビュー射影（今のフレーム。
    // TAAと同じく**ずらしを含まない**もの。ずらしを含めると
    // 毎フレーム半画素ぶんの偽の速度が出ます）。
    row_major float4x4 MotionBlurInverseViewProjection;
    // 前フレームのビュー射影（ずらしを含まないもの）。
    row_major float4x4 MotionBlurPreviousViewProjection;
    // x=ブレの強さ, y=伸ばす最大の長さ（画素）, z=サンプル数,
    // w=予約。
    float4 MotionBlurParameters;
    // x=1/幅, y=1/高さ, z/w=予約。
    float4 MotionBlurTexel;
};

// 自動露出の明るさ測定パス。
cbuffer LuminanceBuffer : register(b10)
{
    // x=1/幅, y=1/高さ（**測定先の**解像度）, z/w=予約。
    float4 LuminanceTexel;
};

// 深度から形状の境界を検出する画面アウトライン。
cbuffer ScreenOutlineBuffer : register(b11)
{
    // rgb=線の色, w=強さ。
    float4 ScreenOutlineColor;
    // x=太さ（画素）, y=深度しきい値,
    // z=法線しきい値, w=予約。
    float4 ScreenOutlineParameters;
    // x=projection._33, y=projection._43,
    // z=1/projection._11, w=1/projection._22。
    float4 ScreenOutlineProjection;
    // xy=1/画面サイズ, zw=画面サイズ。
    float4 ScreenOutlineTexel;
};

Texture2D SourceTexture : register(t0);
TextureCube SkyCubemap : register(t1);
Texture2D DepthTexture : register(t2);
// カスケードシャドウ（Litシェーダーが使っているものと同じ）。
Texture2DArray VolumetricShadowTexture : register(t3);
// TAA（時間的アンチエイリアス）の履歴＝前フレームの解決済みの絵。
Texture2D TemporalHistoryTexture : register(t4);
// 多段でぼかし終えた筋（1/4解像度）。レンズフレアの合成が読みます。
Texture2D LensFlareStreakTexture : register(t5);
// 被写界深度の作業用（半解像度、rgb=色, a=符号付きCoC）。ぼかしパスは
// 「①が書いた色とCoC」を、合成パスは「②がぼかし終えた絵」を、
// どちらもここから読みます（同時には刺さらないので1枠で足ります）。
Texture2D DepthOfFieldTexture : register(t6);
SamplerState LinearSampler : register(s0);
SamplerComparisonState VolumetricShadowSampler
    : register(s1);

struct ScreenVertex
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

ScreenVertex VSMain(uint vertexId : SV_VertexID)
{
    ScreenVertex output;
    const float2 position = vertexId == 0u
        ? float2(-1.0f, -1.0f)
        : (vertexId == 1u
            ? float2(-1.0f, 3.0f)
            : float2(3.0f, -1.0f));
    output.position = float4(position, 0.0f, 1.0f);
    output.uv = float2(
        position.x * 0.5f + 0.5f,
        0.5f - position.y * 0.5f);
    return output;
}

static const int2 ScreenOutlineDirections[8] = {
    int2(-1, -1),
    int2( 0, -1),
    int2( 1, -1),
    int2(-1,  0),
    int2( 1,  0),
    int2(-1,  1),
    int2( 0,  1),
    int2( 1,  1)
};

int2 ScreenOutlineClampPixel(int2 pixel)
{
    const int2 size = max(
        int2(ScreenOutlineTexel.zw),
        int2(1, 1));
    return clamp(pixel, int2(0, 0), size - 1);
}

float ScreenOutlineSceneDistance(int2 pixel)
{
    const float deviceDepth = DepthTexture.Load(int3(
        ScreenOutlineClampPixel(pixel),
        0)).r;
    return LamaPonSceneDistance(
        deviceDepth,
        ScreenOutlineProjection);
}

float3 ScreenOutlineNormal(int2 pixel)
{
    const int2 size = max(
        int2(ScreenOutlineTexel.zw),
        int2(3, 3));
    // LamaPonReconstructViewNormalは4近傍を読むので、画面端では
    // 1画素内側へ寄せてLoadの範囲外アクセスを避けます。
    const int2 interiorMaximum = max(
        size - 2,
        int2(1, 1));
    const int2 safePixel = clamp(
        pixel,
        int2(1, 1),
        interiorMaximum);
    return LamaPonReconstructViewNormal(
        DepthTexture,
        safePixel,
        ScreenOutlineTexel.xy,
        ScreenOutlineProjection,
        float4(
            ScreenOutlineProjection.z,
            ScreenOutlineProjection.w,
            0.0f,
            0.0f));
}

float4 PSScreenOutline(ScreenVertex input) : SV_Target
{
    const float4 source = SourceTexture.Sample(
        LinearSampler,
        input.uv);
    const int2 screenSize = int2(ScreenOutlineTexel.zw);
    if (screenSize.x < 3 || screenSize.y < 3)
    {
        return source;
    }

    const int2 pixel = int2(input.position.xy);
    const float centerDistance =
        ScreenOutlineSceneDistance(pixel);
    const float3 centerNormal =
        ScreenOutlineNormal(pixel);
    const int radius = clamp(
        (int)ScreenOutlineParameters.x,
        1,
        4);
    const float depthThreshold = max(
        ScreenOutlineParameters.y,
        0.0001f);
    const float normalThreshold = max(
        ScreenOutlineParameters.z,
        0.0001f);
    float depthEdge = 0.0f;
    float normalEdge = 0.0f;

    [unroll]
    for (int index = 0; index < 8; ++index)
    {
        const int2 samplePixel = pixel
            + ScreenOutlineDirections[index] * radius;
        const float sampleDistance =
            ScreenOutlineSceneDistance(samplePixel);
        const bool centerIsSky = centerDistance >= 999999.0f;
        const bool sampleIsSky = sampleDistance >= 999999.0f;
        if (centerIsSky != sampleIsSky)
        {
            depthEdge = 1.0f;
        }
        else if (!centerIsSky)
        {
            const float relativeDifference = abs(
                sampleDistance - centerDistance)
                / max(centerDistance, 0.001f);
            depthEdge = max(
                depthEdge,
                smoothstep(
                    0.35f,
                    1.0f,
                    relativeDifference / depthThreshold));

            const float normalDifference = 1.0f - saturate(dot(
                centerNormal,
                ScreenOutlineNormal(samplePixel)));
            normalEdge = max(
                normalEdge,
                smoothstep(
                    0.35f,
                    1.0f,
                    normalDifference / normalThreshold));
        }
    }

    const float edge = saturate(
        max(depthEdge, normalEdge)
        * saturate(ScreenOutlineColor.a));
    return float4(
        lerp(source.rgb, ScreenOutlineColor.rgb, edge),
        source.a);
}

float4 PSSky(ScreenVertex input) : SV_Target
{
    const float2 clip = float2(
        input.uv.x * 2.0f - 1.0f,
        1.0f - input.uv.y * 2.0f);
    const float4 farPosition = mul(
        float4(clip, 1.0f, 1.0f),
        InverseViewProjection);
    const float3 worldPosition =
        farPosition.xyz / max(abs(farPosition.w), 0.00001f);
    const float3 direction = normalize(
        worldPosition - CameraPosition.xyz);
    if (SkyOptions.x > 0.5f)
    {
        const float3 cubeColor =
            SkyCubemap.SampleLevel(
                LinearSampler,
                direction,
                0.0f).rgb;
        return float4(
            cubeColor * max(TopColor.a, 0.0f),
            1.0f);
    }
    const float above = smoothstep(
        -0.03f, 0.85f, direction.y);
    const float below = smoothstep(
        0.0f, 0.65f, -direction.y);
    float3 color = lerp(
        HorizonColor.rgb,
        TopColor.rgb,
        above);
    color = lerp(color, GroundColor.rgb, below);

    // 空に太陽そのものを描きます（朝昼夜モードのときだけ）。
    if (SunDiskColor.a > 0.0f)
    {
        const float cosine = dot(direction, SunDirection.xyz);
        const float radius = max(SunDirection.w, 0.0001f);
        // 円盤の中は光の色そのもの、縁の外は「にじみ」。
        // 縁を1画素で切ると階段状のギザギザが出るので、円盤の
        // 5%ぶんだけぼかしています。
        const float disk = smoothstep(
            cos(radius * 1.05f),
            cos(radius * 0.95f),
            cosine);
        // 太陽のまわりの空の明るみ。角半径の30倍くらいまで
        // ゆるく広がるようにしています。
        const float glow = pow(
            saturate(
                (cosine - cos(radius * 30.0f))
                / max(1.0f - cos(radius * 30.0f), 0.0001f)),
            4.0f);
        // 太陽が地平線の下にあるときは、にじみだけ弱く残します
        // （日没直後の空が完全に均一にならないように）。
        const float horizonFade = smoothstep(
            -0.12f, 0.02f, SunDirection.y);
        color += SunDiskColor.rgb * glow * 0.35f * horizonFade;
        color = lerp(
            color,
            SunDiskColor.rgb,
            disk * horizonFade);
    }
    return float4(color * max(TopColor.a, 0.0f), 1.0f);
}

float3 BrightColor(float2 uv)
{
    const float3 color =
        SourceTexture.Sample(LinearSampler, uv).rgb;
    const float brightness = max(
        color.r,
        max(color.g, color.b));
    return color * saturate(
        (brightness - BloomThreshold)
        / max(brightness, 0.0001f));
}

float4 PSBloom(ScreenVertex input) : SV_Target
{
    const float4 source =
        SourceTexture.Sample(LinearSampler, input.uv);
    const float2 step = TexelSize * BloomRadius;
    float3 bloom = BrightColor(input.uv) * 0.2f;
    bloom += BrightColor(input.uv + float2(step.x, 0.0f)) * 0.12f;
    bloom += BrightColor(input.uv - float2(step.x, 0.0f)) * 0.12f;
    bloom += BrightColor(input.uv + float2(0.0f, step.y)) * 0.12f;
    bloom += BrightColor(input.uv - float2(0.0f, step.y)) * 0.12f;
    bloom += BrightColor(input.uv + step) * 0.08f;
    bloom += BrightColor(input.uv - step) * 0.08f;
    bloom += BrightColor(input.uv + float2(step.x, -step.y)) * 0.08f;
    bloom += BrightColor(input.uv + float2(-step.x, step.y)) * 0.08f;
    return float4(
        source.rgb + bloom * BloomIntensity,
        source.a);
}

// Screen space lens flare。
//
// 高輝度部分を別テクスチャへ抽出せず、1パスの中でサンプルします。
// 画面中心を光学中心に見立て、中心の反対側へゴーストを置き、
// その周囲にハローと放射状の筋を加えます。Bloomと違って、画面上の
// 明るい点が光学系の反射として複数個に分かれて見える表現です。
float3 LensFlareBright(float2 uv)
{
    if (any(uv < 0.0f) || any(uv > 1.0f))
    {
        return 0.0f;
    }
    const float3 color =
        SourceTexture.Sample(LinearSampler, uv).rgb;
    const float brightness = max(
        color.r,
        max(color.g, color.b));
    const float threshold = max(LensFlarePrimary.z, 0.0f);
    const float gate = smoothstep(
        threshold,
        threshold + max(threshold * 0.35f, 0.25f),
        brightness);
    return color * gate;
}

float3 LensFlareChromaticSample(float2 uv, float2 direction)
{
    const float chromatic =
        saturate(LensFlareSecondary.z) * 0.015f;
    const float2 offset = direction * chromatic;
    const float3 red = LensFlareBright(uv + offset);
    const float3 green = LensFlareBright(uv);
    const float3 blue = LensFlareBright(uv - offset);
    return float3(red.r, green.g, blue.b);
}


// 筋（ストリーク）の多段ぼかし。
//
// 1パスで長い筋を作ろうとすると、タップの間隔が空いて点線になります
// （7タップで画面の半分を伸ばすと、間が数百画素も飛びます）。そこで
// 「4タップだけ進めて書き戻す」を3回繰り返し、毎回タップ間隔を4倍に
// 広げます。1回目は1画素刻み、2回目は4画素刻み、3回目は16画素刻みで、
// 前の回の結果を読むので**隙間が埋まったまま**遠くまで伸びます。
// 12タップで64画素ぶんの連続した筋になる、という理屈です。
float4 PSLensFlareStreak(ScreenVertex input) : SV_Target
{
    const float stride = LensFlareStreakPass.x;
    const int directionCount = clamp(
        (int)LensFlareStreakPass.y,
        1,
        4);
    const float baseAngle = LensFlareStreakPass.z;
    const bool firstPass = LensFlareStreakPass.w > 0.5f;

    float3 total = 0.0f;
    float weightTotal = 0.0f;
    [loop]
    for (int index = 0; index < directionCount; ++index)
    {
        // 方向は等間隔に散らします。2本なら十字ではなく180度
        // 反対まで含めて水平＋垂直、というふうに半円で割ります
        // （筋は両側へ伸ばすので、半円ぶんあれば足ります）。
        const float angle = baseAngle
            + 3.14159265f * (float)index
                / (float)directionCount;
        const float2 axis = float2(cos(angle), sin(angle));
        [unroll]
        for (int tap = -2; tap <= 2; ++tap)
        {
            const float2 offset =
                axis * ((float)tap * stride);
            // 画面外は切り捨てず端で止めます。捨てると端の画素だけ
            // タップ数が減って筋が急に細くなり、しかも重みの合計が
            // 合わなくなって暗くなります。
            const float2 uv = clamp(
                input.uv + offset,
                0.0f,
                1.0f);
            // 遠いタップほど弱めます。これが無いと筋の端が
            // 急に切れて棒に見えます。
            const float weight =
                1.0f - abs((float)tap) * 0.22f;
            // 筋の長さ方向に波長をずらします。中心が白く、
            // 外へ行くほど色が分かれる、あの見え方になります。
            const float dispersion =
                saturate(LensFlareSecondary.z);
            const float shift =
                (float)tap / 2.0f * dispersion;
            float3 sample = firstPass
                ? LensFlareBright(uv)
                : LensFlareStreakTexture.SampleLevel(
                    LinearSampler,
                    uv,
                    0.0f).rgb;
            if (dispersion > 0.0f)
            {
                // 手前側を暖色、奥側を寒色へ寄せます。
                sample *= float3(
                    1.0f + shift,
                    1.0f,
                    1.0f - shift);
            }
            total += sample * weight;
            weightTotal += weight;
        }
    }
    // 重みの合計で割ります。タップ数（5）で割ると、重みの合計が
    // 3.68しかないぶん毎回0.74倍に暗くなり、3回重ねると0.4倍まで
    // 落ちて筋がほとんど見えなくなります。合計で割れば1回ごとの
    // 明るさが保たれ、伸ばしたぶんだけ薄くなる自然な減り方に
    // なります。
    total /= max(weightTotal, 0.0001f);
    return float4(total, 1.0f);
}

float4 PSScreenSpaceLensFlare(ScreenVertex input) : SV_Target
{
    const float4 source =
        SourceTexture.Sample(LinearSampler, input.uv);
    const float2 center = float2(0.5f, 0.5f);
    const float2 fromCenter = input.uv - center;
    const float radius = length(fromCenter);
    const float2 direction = radius > 0.0001f
        ? fromCenter / radius
        : float2(1.0f, 0.0f);

    float3 flare = LensFlareBright(input.uv) * 0.22f;

    // ゴーストは光学中心に対して反対側へ4つ置きます。
    const float dispersal = max(
        LensFlareSecondary.x,
        0.01f);
    [unroll]
    for (int index = 1; index <= 4; ++index)
    {
        const float scale = dispersal * (float)index;
        const float2 ghostUv = center - fromCenter * scale;
        const float ghostWeight = 0.23f - (float)index * 0.025f;
        flare += LensFlareChromaticSample(
            ghostUv,
            direction) * max(ghostWeight, 0.05f);
    }

    // 光学中心を囲むハロー。明るい光源が中心の反対側にあるときだけ
    // 円環が出るので、画面全体が白くなるのを避けられます。
    const float haloRadius = clamp(
        LensFlareSecondary.y,
        0.05f,
        1.5f);
    const float haloDistance = abs(radius - haloRadius);
    const float halo = 1.0f - smoothstep(
        0.015f,
        0.10f + haloRadius * 0.18f,
        haloDistance);
    const float2 haloUv = center - direction * haloRadius;
    flare += LensFlareChromaticSample(
        haloUv,
        direction) * halo * 0.32f;

    // 筋は多段パス（PSLensFlareStreak）が1/4解像度で作り終えた
    // ものを読むだけです。1パスで作ろうとすると点線になります。
    const float3 streak =
        LensFlareStreakTexture.SampleLevel(
            LinearSampler,
            input.uv,
            0.0f).rgb;
    flare += streak * LensFlareSecondary.w;

    return float4(
        source.rgb
            + flare * max(LensFlarePrimary.w, 0.0f),
        source.a);
}

float Luminance(float3 color)
{
    return dot(color, float3(0.299f, 0.587f, 0.114f));
}

float4 PSFXAA(ScreenVertex input) : SV_Target
{
    const float2 texel = TexelSize;
    const float3 center =
        SourceTexture.Sample(LinearSampler, input.uv).rgb;
    const float lumaCenter = Luminance(center);
    const float lumaNorth = Luminance(
        SourceTexture.Sample(
            LinearSampler,
            input.uv + float2(0.0f, -texel.y)).rgb);
    const float lumaSouth = Luminance(
        SourceTexture.Sample(
            LinearSampler,
            input.uv + float2(0.0f, texel.y)).rgb);
    const float lumaWest = Luminance(
        SourceTexture.Sample(
            LinearSampler,
            input.uv + float2(-texel.x, 0.0f)).rgb);
    const float lumaEast = Luminance(
        SourceTexture.Sample(
            LinearSampler,
            input.uv + float2(texel.x, 0.0f)).rgb);
    const float lumaMinimum = min(
        lumaCenter,
        min(min(lumaNorth, lumaSouth), min(lumaWest, lumaEast)));
    const float lumaMaximum = max(
        lumaCenter,
        max(max(lumaNorth, lumaSouth), max(lumaWest, lumaEast)));
    if (lumaMaximum - lumaMinimum < 0.0312f)
    {
        return float4(center, 1.0f);
    }

    float2 direction = float2(
        -(lumaNorth - lumaSouth),
        lumaWest - lumaEast);
    const float reduction = max(
        (lumaNorth + lumaSouth + lumaWest + lumaEast)
            * 0.03125f,
        0.0078125f);
    const float inverseMinimum =
        1.0f / (min(abs(direction.x), abs(direction.y)) + reduction);
    direction = clamp(
        direction * inverseMinimum,
        -8.0f,
        8.0f) * texel;

    const float3 first =
        0.5f * (
            SourceTexture.Sample(
                LinearSampler,
                input.uv + direction * (1.0f / 3.0f - 0.5f)).rgb
            + SourceTexture.Sample(
                LinearSampler,
                input.uv + direction * (2.0f / 3.0f - 0.5f)).rgb);
    const float3 second =
        first * 0.5f
        + 0.25f * (
            SourceTexture.Sample(
                LinearSampler,
                input.uv + direction * -0.5f).rgb
            + SourceTexture.Sample(
                LinearSampler,
                input.uv + direction * 0.5f).rgb);
    const float secondLuma = Luminance(second);
    return float4(
        secondLuma < lumaMinimum || secondLuma > lumaMaximum
            ? first
            : second,
        1.0f);
}

float3 ACESFilm(float3 color)
{
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate(
        (color * (a * color + b))
        / (color * (c * color + d) + e));
}

float4 PSToneMap(ScreenVertex input) : SV_Target
{
    float3 color = max(
        SourceTexture.Sample(LinearSampler, input.uv).rgb,
        0.0f);
    const float gradingEnabled = saturate(ColorGradeSecondary.z);
    const float exposure = lerp(
        0.0f,
        ColorGradePrimary.x,
        gradingEnabled);
    const float temperature = lerp(
        0.0f,
        ColorGradePrimary.w,
        gradingEnabled);
    const float tint = lerp(
        0.0f,
        ColorGradeSecondary.x,
        gradingEnabled);
    const float3 whiteBalance = max(float3(
        1.0f + temperature * 0.16f - tint * 0.05f,
        1.0f + tint * 0.10f,
        1.0f - temperature * 0.16f - tint * 0.05f),
        0.05f);
    // 自動露出の補正（段数）。CPU側が測った平均輝度から決めた値が
    // 入ってきます（自動露出が無効なら0）。**カラー調整のオン/オフとは
    // 独立に効かせます。** 露出はカメラの挙動であって色の作り込みでは
    // ないので、「カラー調整を切ったら真っ白になった」という驚きを
    // 作らないためです。
    const float autoExposure = ColorGradeSecondary.w;
    color *= exp2(exposure + autoExposure) * whiteBalance;
    color = ACESFilm(color);

    const float luminance = Luminance(color);
    const float saturation = lerp(
        1.0f,
        max(ColorGradePrimary.z, 0.0f),
        gradingEnabled);
    color = lerp(luminance.xxx, color, saturation);
    const float contrast = lerp(
        1.0f,
        max(ColorGradePrimary.y, 0.0f),
        gradingEnabled);
    color = (color - 0.5f) * contrast + 0.5f;

    const float2 centered = input.uv * 2.0f - 1.0f;
    const float vignetteShape = saturate(
        1.0f - dot(centered, centered) * 0.42f);
    const float vignette = lerp(
        0.0f,
        saturate(ColorGradeSecondary.y),
        gradingEnabled);
    color *= lerp(1.0f, vignetteShape, vignette);
    return float4(saturate(color), 1.0f);
}

float4 PSCopy(ScreenVertex input) : SV_Target
{
    return SourceTexture.Sample(
        LinearSampler,
        input.uv);
}

// ---- SSRのHi-Z深度ピラミッド ----
// 深度バッファを「カメラからの距離」へ直したものを頂点に、各ミップが
// 「4テクセルの最小値（いちばん手前）」を持つピラミッドを作ります。
// SSRのレイは「この区画の最も手前より、レイの区間全体が手前」なら
// 区画ごと飛ばせるので、何も無い空間を大股で越えられます。

// ミップ0: 生の深度→ビュー空間の距離。
// PrefilterParameters.x=projection._33, y=projection._43。
float4 PSReflectionDepthLinearize(
    ScreenVertex input) : SV_Target
{
    const int2 pixel = int2(input.position.xy);
    const float deviceDepth =
        SourceTexture.Load(int3(pixel, 0)).r;
    const float denominator =
        deviceDepth + PrefilterParameters.x;
    // 遠平面（何も描かれていない空）は0除算になるので、十分遠い値に
    // します。ピラミッドはminを取るので、空はどのミップでも
    // 「遮るものが無い」として扱われます。
    if (denominator > -1e-6f)
    {
        return 1e6f;
    }
    return PrefilterParameters.y / denominator;
}

// ミップN+1: 親ミップの2x2の最小値。
// PrefilterParameters.xy = 親ミップの大きさ。
//
// 親の辺が奇数のときは、端の1列（1行）がどの子にも入らず
// こぼれます。こぼれた列は端の子が余分に読んで拾います——
// minのピラミッドで拾い漏れがあると「本当はそこに物があるのに
// 無いことになっている」区画ができ、レイが物を突き抜けます。
float4 PSReflectionDepthDownsample(
    ScreenVertex input) : SV_Target
{
    const int2 parentSize = int2(PrefilterParameters.xy);
    const int2 parent = int2(input.position.xy) * 2;
    const int2 last = parentSize - 1;
    const float a = SourceTexture.Load(
        int3(min(parent, last), 0)).r;
    const float b = SourceTexture.Load(
        int3(min(parent + int2(1, 0), last), 0)).r;
    const float c = SourceTexture.Load(
        int3(min(parent + int2(0, 1), last), 0)).r;
    const float d = SourceTexture.Load(
        int3(min(parent + int2(1, 1), last), 0)).r;
    float nearest = min(min(a, b), min(c, d));
    const bool oddWidth = (parentSize.x & 1) != 0;
    const bool oddHeight = (parentSize.y & 1) != 0;
    if (oddWidth)
    {
        nearest = min(nearest, SourceTexture.Load(
            int3(min(parent + int2(2, 0), last), 0)).r);
        nearest = min(nearest, SourceTexture.Load(
            int3(min(parent + int2(2, 1), last), 0)).r);
    }
    if (oddHeight)
    {
        nearest = min(nearest, SourceTexture.Load(
            int3(min(parent + int2(0, 2), last), 0)).r);
        nearest = min(nearest, SourceTexture.Load(
            int3(min(parent + int2(1, 2), last), 0)).r);
    }
    if (oddWidth && oddHeight)
    {
        nearest = min(nearest, SourceTexture.Load(
            int3(min(parent + int2(2, 2), last), 0)).r);
    }
    return nearest;
}

// ---- SSAO（遮蔽による陰り） ----
// 深度バッファだけから、物が接している隙間や角を暗くします。法線
// バッファを持たない前方レンダリングでも動くよう、法線は深度から
// 復元した位置の傾きで求めます。深度から作ったビュー空間位置の
// 周りをいくつか調べ、手前に物があるほど暗くします。

// 深度（0-1）からビュー空間のZ（カメラからの距離）へ戻します。
// エンジンのカメラは右手系（XMMatrixPerspectiveFovRH）なので、
// depth = -_33 + _43 / 距離 という関係になります。したがって
// 距離 = _43 / (depth + _33) で、分母はA（=_33、負の値）を
// 「引く」のではなく「足す」のが正しい形です。
// 左手系の式（depth - A）にすると分母が常に正になり、下の
// ガードが全ピクセルで成立してSSAOが完全に無効化されます。
// AmbientOcclusionProjectionの並びは共有実装の期待と同じです
// （xy=_33/_43、zw=1/_11・1/_22）。式はLamaPonScreenDepth.hlsliに
// あります。
float AmbientOcclusionViewDepth(float depth)
{
    return LamaPonSceneDistance(
        depth,
        AmbientOcclusionProjection);
}

// UVと深度からビュー空間位置を復元します。
float3 AmbientOcclusionViewPosition(float2 uv, float depth)
{
    return LamaPonViewPositionFromDepth(
        uv,
        depth,
        AmbientOcclusionProjection,
        float4(
            AmbientOcclusionProjection.z,
            AmbientOcclusionProjection.w,
            0.0f,
            0.0f));
}

// UVの位置の深度を読んでビュー空間位置へ戻します。
float3 AmbientOcclusionViewPositionAt(float2 uv)
{
    const float depth = DepthTexture.Sample(
        LinearSampler,
        uv).r;
    return AmbientOcclusionViewPosition(uv, depth);
}

// 深度から法線を再構成します。
//
// ddx/ddyの傾きを使うと三角形単位の面法線になるため、曲面が
// カクカクした面の集まりに見え、さらにポリゴンの境界では別の面を
// またいだ傾きが出て誤った遮蔽が発生します。
//
// そこで上下左右の深度を見て、中心との段差が小さい側（同じ面が
// 続いている側）を軸ごとに選んでから外積を取ります。輪郭では
// 手前と奥をまたがないので、シルエット沿いの破綻が起きません。
// 隣が空（深度1で距離が1e6になる）の場合も段差が巨大になるため、
// 自動的に反対側が選ばれます。
//
// 座標系はx=右、y=上、z=奥（左手系）なので、cross(縦, 横)が
// カメラ向きの-zになります（従来のcross(ddx, ddy)と同じ向き）。
float3 AmbientOcclusionReconstructNormal(
    float2 uv,
    float3 origin,
    float2 texelSize)
{
    const float2 offsetX = float2(texelSize.x, 0.0f);
    const float2 offsetY = float2(0.0f, texelSize.y);

    const float3 left = AmbientOcclusionViewPositionAt(uv - offsetX);
    const float3 right = AmbientOcclusionViewPositionAt(uv + offsetX);
    // 画面のyは下向きなので、uvを引いた側が画面上side（ビュー空間の+y）。
    const float3 up = AmbientOcclusionViewPositionAt(uv - offsetY);
    const float3 down = AmbientOcclusionViewPositionAt(uv + offsetY);

    // 段差の小さい側を選んで外積を取る部分は共有実装です。
    return LamaPonNormalFromNeighbours(
        origin, left, right, up, down);
}

// 画素ごとに向きを変える擬似乱数（縞模様を目立たなくします）。
float AmbientOcclusionNoise(float2 pixel)
{
    return frac(
        52.9829189f
        * frac(dot(pixel, float2(0.06711056f, 0.00583715f))));
}

// 遮蔽量だけを求めるパス（半解像度のRチャンネルへ出力）。
// 1.0=遮蔽なし、0.0=完全に遮蔽。色を暗くするのは後段のApplyです。
// 3パスに分けているのは、途中でブラーをかけてサンプル数由来の
// ザラつきを消すためです。
float4 PSAmbientOcclusion(ScreenVertex input) : SV_Target
{
    const float depth = DepthTexture.Sample(
        LinearSampler,
        input.uv).r;
    // 空（深度1）は遮蔽しません。
    if (depth >= 0.999999f)
    {
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    const float3 origin = AmbientOcclusionViewPosition(
        input.uv,
        depth);

    const float2 texelSize = AmbientOcclusionParameters.xy;
    const float radius = AmbientOcclusionParameters.z;
    const float strength = AmbientOcclusionParameters.w;

    // 上下左右の深度から法線を再構成します（ddx/ddyの面法線より
    // 曲面と輪郭に強い）。
    const float3 normal = AmbientOcclusionReconstructNormal(
        input.uv,
        origin,
        texelSize);

    // 画面上での探索半径。近くのものほど大きく広がります。
    const float projectedRadius =
        radius / max(origin.z, 0.001f);
    const float rotation = AmbientOcclusionNoise(
        input.uv / max(texelSize.x, 1e-6f)
            * float2(1.0f, texelSize.x / max(texelSize.y, 1e-6f)))
        * 6.2831853f;

    // サンプル数は品質設定から渡されます（8〜24）。
    const int SampleCount = clamp(
        int(AmbientOcclusionQuality.x),
        4,
        32);
    float occlusion = 0.0f;
    for (int index = 0; index < SampleCount; ++index)
    {
        // 黄金角でらせん状に配置し、少ない回数でも偏らせません。
        const float fraction =
            (float(index) + 0.5f) / float(SampleCount);
        const float angle = rotation + fraction * 18.849556f;
        const float distance = sqrt(fraction);
        const float2 offset = float2(
            cos(angle),
            sin(angle)) * distance * projectedRadius * 0.5f;
        const float2 sampleUv = input.uv + offset;
        if (any(sampleUv < 0.0f) || any(sampleUv > 1.0f))
        {
            continue;
        }

        const float sampleDepth = DepthTexture.Sample(
            LinearSampler,
            sampleUv).r;
        if (sampleDepth >= 0.999999f)
        {
            continue;
        }
        const float3 samplePosition =
            AmbientOcclusionViewPosition(
                sampleUv,
                sampleDepth);
        float3 difference = samplePosition - origin;
        const float length2 = dot(difference, difference);
        if (length2 < 1e-8f)
        {
            continue;
        }
        difference *= rsqrt(length2);
        const float sampleDistance = sqrt(length2);
        // 法線より手前側にある分だけ遮蔽と見なします。自己遮蔽を
        // 避けるため、わずかな傾き（bias）は無視します。
        const float facing = saturate(
            dot(normal, difference) - 0.06f);
        // 半径より遠いものは効かせません（別の物体で暗くならない）。
        const float falloff = saturate(
            1.0f - sampleDistance / max(radius, 0.001f));
        occlusion += facing * falloff;
    }

    occlusion = saturate(
        occlusion / float(SampleCount) * 2.4f * strength);
    // 遮蔽率ではなく「残る明るさ」を書きます。ブラーで平均しても
    // 意味が変わらず、Apply側は掛け算するだけで済みます。
    const float visibility = 1.0f - occlusion;
    return float4(visibility, visibility, visibility, 1.0f);
}

// 深度を見るブラー（バイラテラル）。少ないサンプル数と画素ごとの
// 回転で出るザラつきを均します。単純なブラーだと物の輪郭を越えて
// にじみ、手前の物の縁に陰りが漏れるため、中心と深度が近い画素だけ
// を混ぜます。
float4 PSAmbientOcclusionBlur(ScreenVertex input) : SV_Target
{
    const float2 texelSize = AmbientOcclusionParameters.xy;
    const float centerDepth = AmbientOcclusionViewDepth(
        DepthTexture.Sample(LinearSampler, input.uv).r);

    // 深度差の許容量。遠くほど絶対差が大きくなるので距離に比例させます。
    // 床のように視線に対して浅い角度の面では、隣の画素でも深度が
    // それなりに変わります。ここを厳しくすると全部の隣接画素が
    // 却下されて中心だけが残り、ブラーが効かなくなります
    // （実際に2%の二値判定にしたところ、接地部の陰りが点々のまま
    // 残りました）。少し広めに取り、二値ではなく連続的に落とします。
    const float depthScale =
        max(centerDepth * 0.08f, 0.05f);

    float total = 0.0f;
    float weightSum = 0.0f;
    // 4x4（中心が境界に来るオフセット）で16タップ。半解像度なので
    // フル解像度換算では広い範囲を均せます。
    [unroll]
    for (int y = -2; y <= 1; ++y)
    {
        [unroll]
        for (int x = -2; x <= 1; ++x)
        {
            const float2 offset = float2(
                (float(x) + 0.5f) * texelSize.x,
                (float(y) + 0.5f) * texelSize.y);
            const float2 sampleUv = input.uv + offset;
            const float sampleDepth = AmbientOcclusionViewDepth(
                DepthTexture.Sample(
                    LinearSampler,
                    sampleUv).r);
            // 深度が離れるほど滑らかに軽くします。物の輪郭を
            // 越えたタップはほぼ0になり、陰りが漏れません。
            const float depthRatio =
                (sampleDepth - centerDepth) / depthScale;
            const float weight =
                1.0f / (1.0f + depthRatio * depthRatio);
            total += SourceTexture.Sample(
                LinearSampler,
                sampleUv).r * weight;
            weightSum += weight;
        }
    }

    const float visibility = weightSum > 0.0f
        ? total / weightSum
        : SourceTexture.Sample(LinearSampler, input.uv).r;
    return float4(visibility, visibility, visibility, 1.0f);
}

// 求めた遮蔽をカラーへ掛けます。AOは半解像度なので、ここで
// バイリニア補間されながら拡大されます。
// ---- 被写界深度（DoF） ----
//
// 3パスです。
//   ①半解像度へ「色」と「符号付きCoC」を書き出す
//   ②半解像度で円形にぼかす
//   ③フル解像度で、CoCの大きさに応じて元の絵と混ぜる
//
// 半解像度でぼかすのは、同じ見た目のぼけを1/4のコストで作れる
// からです。ぼけた絵に細部は残らないので、解像度を落としても
// 失うものがありません。逆に③をフル解像度で行うのは必須で、
// ピントが合っている面をここで元の絵から取り直します。

// 深度（0-1）からビュー空間のZ（カメラからの距離）へ戻します。
// 式の根拠はAmbientOcclusionViewDepthと同じ（右手系なので分母は
// depth + _33）です。SSAOと同じ計算ですが、DoFはSSAOを切っていても
// 動く必要があるため、専用の定数バッファから読みます。
float DepthOfFieldViewDepth(float depth)
{
    const float projectionA = DepthOfFieldProjection.x;
    const float projectionB = DepthOfFieldProjection.y;
    const float denominator = depth + projectionA;
    // 遠平面（depth≒-A）では0除算になるので十分遠い値を返します。
    // 空はここへ来て「無限遠」として扱われ、最大までぼけます
    // （10mにピントを合わせたカメラで空がぼけるのと同じです）。
    if (denominator > -1e-6f)
    {
        return 1e6f;
    }
    return projectionB / denominator;
}

// 符号付きCoC（ぼけの大きさ）。-1〜+1で、負が前ぼけ（焦点面より
// 手前）、正が後ぼけ（奥）です。1.0でぼけ半径の上限に当たります。
//
// 薄レンズのCoCは |1/焦点距離 - 1/被写体距離| に比例します。基準を
// ピントの合う帯の端 r に取ると |1/r - 1/d| * r = |1 - r/d| となり、
// 割り算1回で済みます。この形は本物のレンズと同じ振る舞いをします。
//   ・奥はどこまで行っても1で飽和する（無限遠のぼけには上限がある）
//   ・手前は距離が半分になるたびに倍で増える（近いものは急にぼける）
// 距離の差をそのまま使うと、この非対称さが出ずに「奥だけ延々と
// ぼける」不自然な絵になります。
//
// 符号を持たせているのは、②のにじみの向きを決めるためです。深度を
// もう1枚読まずに前後を判定できます。
float DepthOfFieldSignedCircleOfConfusion(float viewDepth)
{
    const float focus = max(DepthOfFieldParameters.x, 0.01f);
    const float halfRange =
        max(DepthOfFieldParameters.y, 0.0f) * 0.5f;
    const float nearEdge = max(focus - halfRange, 0.01f);
    const float farEdge = focus + halfRange;

    float reference;
    // 前ぼけを負、後ぼけを正にする符号。intrinsicのsign()と名前が
    // ぶつからないよう別名にしています。
    float direction;
    if (viewDepth < nearEdge)
    {
        reference = nearEdge;
        direction = -1.0f;
    }
    else if (viewDepth > farEdge)
    {
        reference = farEdge;
        direction = 1.0f;
    }
    else
    {
        // ピントの合っている帯の中。完全に鋭いままにします。
        return 0.0f;
    }

    const float relative = abs(
        1.0f - reference / max(viewDepth, 0.001f));
    return direction
        * saturate(relative * max(DepthOfFieldParameters.z, 0.0f));
}

// ①半解像度へ色とCoCを書き出します。
//
// 半解像度の画素の中心はフル解像度の4画素のちょうど角に当たるので、
// 色はバイリニア1回でその4画素の平均になります（追加のタップは
// 要りません）。
//
// 一方CoCは平均してはいけません。輪郭をまたいだ深度の平均は「手前と
// 奥の間のどこか」という存在しない距離になり、それがたまたま焦点面に
// 当たると輪郭沿いだけCoCが0になります。すると奥のぼけた背景に、
// 手前の物の形をした鋭い輪が残ります。そこで4画素を個別に読み、
// **絶対値が最大のCoC**を採ります。こちらへ寄せた場合の誤差は
// 「手前の物の縁が半画素ぶん余分にぼける」ですが、その画素は③で
// フル解像度の鋭い色に戻されるため、画面には出てきません。
float4 PSDepthOfFieldPrepare(ScreenVertex input) : SV_Target
{
    const float3 color = SourceTexture.SampleLevel(
        LinearSampler,
        input.uv,
        0.0f).rgb;

    // 半解像度の画素(i,j)はフル解像度の(2i,2j)から2x2に対応します。
    const int2 basePixel = int2(input.position.xy) * 2;
    float signedCoc = 0.0f;
    [unroll]
    for (int y = 0; y < 2; ++y)
    {
        [unroll]
        for (int x = 0; x < 2; ++x)
        {
            const float depth = DepthTexture.Load(
                int3(basePixel + int2(x, y), 0)).r;
            const float candidate =
                DepthOfFieldSignedCircleOfConfusion(
                    DepthOfFieldViewDepth(depth));
            if (abs(candidate) > abs(signedCoc))
            {
                signedCoc = candidate;
            }
        }
    }
    return float4(color, signedCoc);
}

// ②半解像度で円形にぼかします。
//
// サンプル点は黄金角の螺旋で置きます。リング数とリングごとの点数を
// 決め打ちするやり方と違い、サンプル数を何個にしても円の中へ均等に
// 散ってくれるので、品質設定で本数を変えられます。
float4 PSDepthOfFieldBlur(ScreenVertex input) : SV_Target
{
    const float2 texel = DepthOfFieldTexel.xy;
    const int sampleCount = clamp(
        (int)DepthOfFieldTexel.z,
        4,
        64);
    // 設定はフル解像度の画素数なので、半解像度の画素へ直します。
    const float maximumRadius =
        max(DepthOfFieldParameters.w, 0.0f) * 0.5f;

    const float4 center = DepthOfFieldTexture.SampleLevel(
        LinearSampler,
        input.uv,
        0.0f);
    const float centerCoc = abs(center.a);

    // 中心は必ず重み1で入れます。こうすると重みの合計が必ず1以上に
    // なるので、0除算よけのεが要りません（εを置くと、ぼけていない
    // 画素でだけ効いて色が変わります）。
    float3 total = center.rgb;
    float totalWeight = 1.0f;

    // 螺旋の向きを画素ごとに回します。固定のままだと、点光源のぼけに
    // 螺旋の腕がそのまま模様として浮きます。SSAOと同じ画素ごとの
    // 擬似乱数（縞を散らす目的も同じ）を使います。
    const float rotation =
        AmbientOcclusionNoise(input.position.xy) * 6.28318531f;

    [loop]
    for (int index = 0; index < sampleCount; ++index)
    {
        // 黄金角。連続する点が最も離れて並びます。
        const float angle =
            (float)index * 2.39996323f + rotation;
        // 半径をsqrtで取ると、点が円の面積に対して均等に散ります
        // （そのまま比例させると中心へ密集します）。
        const float radius = sqrt(
            ((float)index + 0.5f) / (float)sampleCount)
            * maximumRadius;
        const float2 uv = clamp(
            input.uv
                + float2(cos(angle), sin(angle))
                    * radius * texel,
            0.0f,
            1.0f);
        const float4 tap = DepthOfFieldTexture.SampleLevel(
            LinearSampler,
            uv,
            0.0f);

        // にじみの向き。手前のもの（CoCが負）は奥へ自由にはみ出します
        // ——本物のぼけと同じで、ピントの合った被写体の前にある枝は
        // 被写体に覆いかぶさります。逆に、奥のものが鋭い手前へ
        // はみ出すのは間違いで、そのままにすると輪郭の外側に薄い霧を
        // まとった「切り抜き」のような絵になります。そちら向きは
        // 中心のぼけ具合までに抑えます。
        const float tapCoc = abs(tap.a);
        const float spread = tap.a < 0.0f
            ? tapCoc
            : min(tapCoc, centerCoc);
        // 1画素ぶんの柔らかい縁を付けます。段差のままだとぼけの
        // 輪郭が硬く、点光源が「輪」に見えます。
        const float weight = saturate(
            spread * maximumRadius - radius + 1.0f);
        total += tap.rgb * weight;
        totalWeight += weight;
    }
    // CoCはそのまま持ち回します（③はフル解像度で読み直すので
    // 使いませんが、デバッグでこのテクスチャを覗いたときに
    // 意味のある値が入っている方が追いやすいためです）。
    return float4(total / totalWeight, center.a);
}

// ③フル解像度で合成します。
float4 PSDepthOfFieldComposite(ScreenVertex input) : SV_Target
{
    const float4 sharp = SourceTexture.Sample(
        LinearSampler,
        input.uv);
    const float depth = DepthTexture.Sample(
        LinearSampler,
        input.uv).r;
    const float coc = abs(
        DepthOfFieldSignedCircleOfConfusion(
            DepthOfFieldViewDepth(depth)));
    const float3 blurred = DepthOfFieldTexture.SampleLevel(
        LinearSampler,
        input.uv,
        0.0f).rgb;

    // ぼけ半径がフル解像度の1画素に達したところで、完全にぼかした絵へ
    // 移ります。1画素未満のぼけは見えないので、そこは元の絵をそのまま
    // 使います。ここを常に混ぜてしまうと、ピントが合っている面まで
    // 半解像度の絵が入って全体が甘くなります。
    const float mixAmount = saturate(
        coc * max(DepthOfFieldParameters.w, 0.0f));
    return float4(
        lerp(sharp.rgb, blurred, mixAmount),
        sharp.a);
}

// ---- モーションブラー（カメラの動きによるブレ） ----
//
// TAAの再投影とまったく同じ計算で「この画素が前フレームどこに写って
// いたか」を求め、今の位置との差（＝画面上の移動量）に沿ってサンプル
// して平均します。新しいバッファは要りません（深度と行列2本だけ）。
//
// 物体ごとの速度は持っていないので、ブレるのはカメラが動いたぶんだけ
// です。速度バッファを足すと自作ShaderがMRTへ書けなくなるため、TAAと
// 同じ理由で避けています。
float4 PSMotionBlur(ScreenVertex input) : SV_Target
{
    const float4 source =
        SourceTexture.Sample(LinearSampler, input.uv);
    // 空（深度1）も通します。無限遠に近い点はカメラの平行移動では
    // ほとんど動かず、回転では大きく動くので、「走っても空はブレない
    // が、振り向くとブレる」が式のうえで自動的に出ます。
    const float depth = DepthTexture.Sample(
        LinearSampler,
        input.uv).r;

    const float2 clip = float2(
        input.uv.x * 2.0f - 1.0f,
        1.0f - input.uv.y * 2.0f);
    const float4 worldHomogeneous = mul(
        float4(clip, depth, 1.0f),
        MotionBlurInverseViewProjection);
    if (worldHomogeneous.w <= 0.0001f)
    {
        return source;
    }
    const float3 worldPosition =
        worldHomogeneous.xyz / worldHomogeneous.w;

    const float4 previousClip = mul(
        float4(worldPosition, 1.0f),
        MotionBlurPreviousViewProjection);
    if (previousClip.w <= 0.0001f)
    {
        // 前フレームはカメラの後ろにあった画素。伸ばす向きが決まらない
        // のでブレさせません。
        return source;
    }
    const float2 previousUv = float2(
        previousClip.x / previousClip.w * 0.5f + 0.5f,
        0.5f - previousClip.y / previousClip.w * 0.5f);

    float2 velocity = (input.uv - previousUv)
        * max(MotionBlurParameters.x, 0.0f);
    // 長さは画素数で見ます。UVのままだと縦横で尺度が違い、上限が
    // 画面の縦横比で変わってしまいます。
    const float2 velocityPixels =
        velocity / max(MotionBlurTexel.xy, 1e-6f);
    const float lengthPixels = length(velocityPixels);
    const float limitPixels = max(MotionBlurParameters.y, 0.0f);
    // 半画素も動いていないなら何もしません。動いていない画面が
    // サンプルの丸め誤差でわずかに甘くなるのを防ぎます。
    if (lengthPixels < 0.5f || limitPixels <= 0.0f)
    {
        return source;
    }
    if (lengthPixels > limitPixels)
    {
        // 上限で切ります。切らないとカメラを素早く振ったときに画面
        // 全体が溶けます。
        velocity *= limitPixels / lengthPixels;
    }

    const int sampleCount = clamp(
        (int)MotionBlurParameters.z,
        2,
        32);
    // 中心を必ず重み1で入れておくと、重みの合計が必ず1以上になるので
    // 0除算よけのεが要りません（εは効いてしまうと色を変えます）。
    float3 total = source.rgb;
    float totalWeight = 1.0f;
    [loop]
    for (int index = 0; index < sampleCount; ++index)
    {
        // シャッターの中心を今の位置に取り、前後へ半分ずつ伸ばします。
        // 片側だけへ伸ばすと、物が進行方向へずれて見えます。
        const float offset =
            ((float)index + 0.5f) / (float)sampleCount - 0.5f;
        const float2 uv = clamp(
            input.uv + velocity * offset,
            0.0f,
            1.0f);
        total += SourceTexture.SampleLevel(
            LinearSampler,
            uv,
            0.0f).rgb;
        totalWeight += 1.0f;
    }
    return float4(total / totalWeight, source.a);
}

// ---- 自動露出の明るさ測定 ----
//
// トーンマップ前のHDRから輝度の**対数**を書き出します。以降の平均は
// GenerateMipsに任せ、いちばん小さいミップ（1x1）をCPUが読みます。
//
// 対数で平均するのは、明るさの感じ方が比で決まるためです。線形の平均を
// 取ると、画面の隅にある1個の明るい光源が平均を支配して、暗い部屋が
// もっと暗くなります（対数なら「何段明るいか」の平均になります）。
//
// 出力先は1/4解像度で、1画素がフル解像度の4x4を覆います。バイリニアの
// 1タップはちょうど2x2の平均になるので、2x2の位置へ4タップ置けば
// 16画素の正確な平均になります。**取りこぼしを作らないのが要点**で、
// 間引くと細かい明滅がそのまま露出のちらつきになります。
float4 PSLuminance(ScreenVertex input) : SV_Target
{
    // 1/4解像度の1テクセルの1/4＝フル解像度の1テクセルぶん。
    const float2 offset = LuminanceTexel.xy * 0.25f;
    float3 total = 0.0f;
    total += SourceTexture.SampleLevel(
        LinearSampler,
        input.uv + float2(-offset.x, -offset.y),
        0.0f).rgb;
    total += SourceTexture.SampleLevel(
        LinearSampler,
        input.uv + float2(offset.x, -offset.y),
        0.0f).rgb;
    total += SourceTexture.SampleLevel(
        LinearSampler,
        input.uv + float2(-offset.x, offset.y),
        0.0f).rgb;
    total += SourceTexture.SampleLevel(
        LinearSampler,
        input.uv + float2(offset.x, offset.y),
        0.0f).rgb;
    const float average = Luminance(max(total * 0.25f, 0.0f));
    // 真っ黒（log(0)=-inf）を避けます。1e-4は「実際には一度も
    // 効かない」十分小さい値で、0.02（設定の下限の既定）より
    // 200倍以上暗いところにあります。
    return log(max(average, 1e-4f)).xxxx;
}

// ---- ボリュメトリックライト（光の筋 / god ray） ----
//
// カメラから各ピクセルへ向かうレイに沿って進み、「その点に光が
// 届いているか」をシャドウマップで判定して足し込みます。光が
// 届いている区間が長いほど明るくなり、遮られた区間は暗いまま
// なので、遮蔽物の影が空気中に筋として現れます。
//
// 既存のカスケードシャドウをそのまま引くので、新しいバッファは
// 作っていません。影付きの平行光源が必要です。

static const float LamaPonVolumetricPi = 3.14159265f;

// Henyey-Greenstein位相関数。空気中の粒子が光をどの方向へ散らすか
// のモデルで、光源の方を向いたときだけ明るくなる指向性を作ります。
// これが無いと画面全体が均一に白くなって「霧」に見えます。
float VolumetricPhase(float cosineAngle, float scattering)
{
    const float g = clamp(scattering, 0.0f, 0.95f);
    const float gSquared = g * g;
    const float denominator =
        1.0f + gSquared - 2.0f * g * cosineAngle;
    return (1.0f - gSquared)
        / (4.0f * LamaPonVolumetricPi
            * pow(max(denominator, 0.0001f), 1.5f));
}

// ワールド座標が光に照らされているかをカスケードシャドウで判定。
float VolumetricShadowAt(float3 worldPosition)
{
    const int cascadeCount =
        (int)VolumetricShadowParameters.x;
    // 手前のカスケードから順に、範囲へ収まるものを使います。
    [loop]
    for (int cascade = 0; cascade < 4; ++cascade)
    {
        if (cascade >= cascadeCount)
        {
            break;
        }
        const float4 lightPosition = mul(
            float4(worldPosition, 1.0f),
            VolumetricCascades[cascade]);
        if (lightPosition.w <= 0.0001f)
        {
            continue;
        }
        const float3 projected =
            lightPosition.xyz / lightPosition.w;
        const float2 shadowUv =
            projected.xy * float2(0.5f, -0.5f) + 0.5f;
        if (shadowUv.x < 0.0f || shadowUv.x > 1.0f
            || shadowUv.y < 0.0f || shadowUv.y > 1.0f
            || projected.z <= 0.0f
            || projected.z >= 1.0f)
        {
            continue;
        }
        return VolumetricShadowTexture.SampleCmpLevelZero(
            VolumetricShadowSampler,
            float3(shadowUv, cascade),
            projected.z
                - VolumetricShadowParameters.y);
    }
    // どのカスケードにも入らない遠方は「照らされている」扱い。
    return 1.0f;
}

float4 PSVolumetricLight(ScreenVertex input) : SV_Target
{
    const float4 sceneColor =
        SourceTexture.Sample(LinearSampler, input.uv);

    // 深度からワールド座標を復元し、カメラからの距離を求めます。
    const float depth =
        DepthTexture.Sample(LinearSampler, input.uv).r;
    const float2 clip = float2(
        input.uv.x * 2.0f - 1.0f,
        1.0f - input.uv.y * 2.0f);
    const float4 worldHomogeneous = mul(
        float4(clip, depth, 1.0f),
        VolumetricInverseViewProjection);
    if (worldHomogeneous.w <= 0.0001f)
    {
        return sceneColor;
    }
    const float3 worldPosition =
        worldHomogeneous.xyz / worldHomogeneous.w;

    const float3 cameraPosition =
        VolumetricCameraPosition.xyz;
    const float3 toPixel = worldPosition - cameraPosition;
    const float pixelDistance = length(toPixel);
    if (pixelDistance <= 0.0001f)
    {
        return sceneColor;
    }
    const float3 rayDirection = toPixel / pixelDistance;
    // 空を見ているところ（深度1）も最大距離まで進めます。
    const float marchDistance = min(
        pixelDistance,
        VolumetricCameraPosition.w);

    const int sampleCount =
        (int)max(VolumetricLightDirection.w, 1.0f);
    const float stepLength =
        marchDistance / (float)sampleCount;

    // 開始位置をピクセルごとにずらして、少ないサンプル数でも
    // 縞（バンディング）が出にくくします。ここはUVではなく
    // ピクセル座標で計算します。UVは0〜1しか動かないので、
    // 隣のピクセルとの差が小さすぎてディザにならないからです。
    const float dither = frac(
        52.9829189f
        * frac(dot(
            input.position.xy,
            float2(0.06711056f, 0.00583715f))));
    float travelled =
        stepLength * (0.5f + dither * 0.5f);

    // 光の向きは「光源から出る向き」なので、散乱の判定には
    // 視線との角度で -direction を使います。
    const float cosineAngle = dot(
        rayDirection,
        -normalize(VolumetricLightDirection.xyz));
    const float phase = VolumetricPhase(
        cosineAngle,
        VolumetricLightColor.w);

    float accumulated = 0.0f;
    [loop]
    for (int step = 0; step < sampleCount; ++step)
    {
        const float3 samplePosition =
            cameraPosition + rayDirection * travelled;
        accumulated +=
            VolumetricShadowAt(samplePosition);
        travelled += stepLength;
    }
    // 距離で正規化して、遠くを見たときだけ極端に明るくなるのを
    // 防ぎます。
    const float visibility =
        accumulated / (float)sampleCount;

    const float3 scatter =
        VolumetricLightColor.rgb
        * visibility
        * phase
        * (marchDistance
            / max(VolumetricCameraPosition.w, 0.0001f));
    return float4(
        sceneColor.rgb + max(scatter, 0.0f),
        sceneColor.a);
}

// TAA（時間的アンチエイリアス）の解決。
//
// 今のフレームと前フレームの結果を混ぜます。前フレームのどこを読むかは
// 「深度からワールド座標を戻し、前フレームの行列で射影し直す」ことで
// 求めます（再投影）。カメラが動いても同じ場所を見続けられます。
//
// ただし物体ごとの速度は持っていないので、動く物の上では再投影が
// 外れます。そこで今のフレームの近傍9マスの色の範囲を作り、履歴を
// その範囲へ押し込みます（近傍クランプ）。範囲から大きく外れた履歴＝
// 別のものを指している履歴なので、押し込むことで残像が消えます。
float4 PSTemporalAntiAliasing(ScreenVertex input)
    : SV_Target
{
    const float3 current =
        SourceTexture.Sample(LinearSampler, input.uv).rgb;

    // 深度が最遠（空・何も描かれていない）でも打ち切りません。
    // 無限遠の点として再投影すれば正しく前フレームへ写るからです。
    // ここで打ち切ると輪郭の空側だけ混ざらず、エッジの片側しか
    // 均されません（2026-08-05に段差が半分しか減らない原因でした）。
    const float depth =
        DepthTexture.Sample(LinearSampler, input.uv).r;

    const float2 clip = float2(
        input.uv.x * 2.0f - 1.0f,
        1.0f - input.uv.y * 2.0f);
    const float4 worldHomogeneous = mul(
        float4(clip, depth, 1.0f),
        TemporalInverseViewProjection);
    if (worldHomogeneous.w <= 0.0001f)
    {
        return float4(current, 1.0f);
    }
    const float3 worldPosition =
        worldHomogeneous.xyz / worldHomogeneous.w;

    const float4 previousClip = mul(
        float4(worldPosition, 1.0f),
        TemporalPreviousViewProjection);
    if (previousClip.w <= 0.0001f)
    {
        return float4(current, 1.0f);
    }
    const float3 previousProjected =
        previousClip.xyz / previousClip.w;
    const float2 previousUv = float2(
        previousProjected.x * 0.5f + 0.5f,
        0.5f - previousProjected.y * 0.5f);
    // 前フレームでは画面の外だった場所は履歴がありません。
    if (previousUv.x < 0.0f || previousUv.x > 1.0f
        || previousUv.y < 0.0f || previousUv.y > 1.0f)
    {
        return float4(current, 1.0f);
    }

    // 近傍9マスから色の範囲を作ります。
    const float2 texel = TemporalParameters.zw;
    float3 minimumColor = current;
    float3 maximumColor = current;
    [unroll]
    for (int offsetY = -1; offsetY <= 1; ++offsetY)
    {
        [unroll]
        for (int offsetX = -1; offsetX <= 1; ++offsetX)
        {
            if (offsetX == 0 && offsetY == 0)
            {
                continue;
            }
            const float3 neighbour =
                SourceTexture.Sample(
                    LinearSampler,
                    input.uv
                        + float2(
                            (float)offsetX * texel.x,
                            (float)offsetY * texel.y)).rgb;
            minimumColor = min(minimumColor, neighbour);
            maximumColor = max(maximumColor, neighbour);
        }
    }
    // 緩さを掛けて範囲を広げます。0にすると履歴がほぼ捨てられ、
    // アンチエイリアスも効かなくなります。
    const float tolerance = max(
        TemporalParameters.y,
        0.0f);
    const float3 middle =
        (minimumColor + maximumColor) * 0.5f;
    const float3 extent =
        (maximumColor - minimumColor) * 0.5f * tolerance;
    minimumColor = middle - extent;
    maximumColor = middle + extent;

    const float3 history =
        TemporalHistoryTexture.Sample(
            LinearSampler,
            previousUv).rgb;
    const float3 clampedHistory = clamp(
        history,
        minimumColor,
        maximumColor);

    const float weight = saturate(TemporalParameters.x);
    return float4(
        lerp(current, clampedHistory, weight),
        1.0f);
}

// 左右反転コピー。リフレクションプローブのボックス射影が使います。
// エンジンの右手系で描いた面画像を、D3Dの（左手系の）キューブ面
// レイアウトへ合わせるには左右の鏡像が必要ですが、射影行列で
// 反転すると巻き方向が逆になりカリングが崩れるため、描いた後に
// このパスで反転します。
float4 PSCopyMirrorX(ScreenVertex input) : SV_Target
{
    return SourceTexture.Sample(
        LinearSampler,
        float2(1.0f - input.uv.x, input.uv.y));
}

// ---- IBL事前フィルタ（スカイキューブマップ設定時に1回だけ実行） ----

// キューブ面のUVから方向ベクトルを作ります（D3D11の面順）。
float3 CubeDirection(uint face, float2 uv)
{
    const float2 st = float2(
        uv.x * 2.0f - 1.0f,
        1.0f - uv.y * 2.0f);
    if (face == 0u)
    {
        return normalize(float3(1.0f, st.y, -st.x));
    }
    if (face == 1u)
    {
        return normalize(float3(-1.0f, st.y, st.x));
    }
    if (face == 2u)
    {
        return normalize(float3(st.x, 1.0f, -st.y));
    }
    if (face == 3u)
    {
        return normalize(float3(st.x, -1.0f, st.y));
    }
    if (face == 4u)
    {
        return normalize(float3(st.x, st.y, 1.0f));
    }
    return normalize(float3(-st.x, st.y, -1.0f));
}

float RadicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u)
        | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u)
        | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u)
        | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u)
        | ((bits & 0xFF00FF00u) >> 8u);
    return (float)bits * 2.3283064365386963e-10f;
}

float2 Hammersley(uint index, uint count)
{
    return float2(
        (float)index / (float)count,
        RadicalInverseVdC(index));
}

// GGX分布に沿ったハーフベクトルの重点サンプリング。
float3 ImportanceSampleGGX(
    float2 xi,
    float roughness,
    float3 normal)
{
    const float alpha = roughness * roughness;
    const float phi = 6.2831853f * xi.x;
    const float cosTheta = sqrt(
        (1.0f - xi.y)
        / (1.0f + (alpha * alpha - 1.0f) * xi.y));
    const float sinTheta =
        sqrt(1.0f - cosTheta * cosTheta);
    const float3 halfVector = float3(
        sinTheta * cos(phi),
        sinTheta * sin(phi),
        cosTheta);
    const float3 up =
        abs(normal.z) < 0.999f
            ? float3(0.0f, 0.0f, 1.0f)
            : float3(1.0f, 0.0f, 0.0f);
    const float3 tangent =
        normalize(cross(up, normal));
    const float3 bitangent = cross(normal, tangent);
    return normalize(
        tangent * halfVector.x
        + bitangent * halfVector.y
        + normal * halfVector.z);
}

// スペキュラ事前畳み込み：ミップごとに粗さを上げてGGX畳み込み。
float4 PSPrefilterEnvironment(
    ScreenVertex input) : SV_Target
{
    const uint face = (uint)PrefilterParameters.x;
    const float roughness = PrefilterParameters.y;
    const float sourceResolution =
        max(PrefilterParameters.z, 1.0f);
    const float3 normal =
        CubeDirection(face, input.uv);

    if (roughness <= 0.001f)
    {
        return float4(
            SkyCubemap.SampleLevel(
                LinearSampler,
                normal,
                0.0f).rgb,
            1.0f);
    }

    const uint SampleCount = 64u;
    const float saTexel =
        4.0f * 3.14159265f
        / (6.0f * sourceResolution
            * sourceResolution);
    float3 color = 0.0f.xxx;
    float weight = 0.0f;
    [loop]
    for (uint index = 0u; index < SampleCount; ++index)
    {
        const float3 halfVector = ImportanceSampleGGX(
            Hammersley(index, SampleCount),
            roughness,
            normal);
        const float3 lightDirection = normalize(
            2.0f * dot(normal, halfVector) * halfVector
            - normal);
        const float normalDotLight =
            saturate(dot(normal, lightDirection));
        if (normalDotLight <= 0.0f)
        {
            continue;
        }
        // pdfからソースミップを選び、ちらつきを抑えます。
        const float normalDotHalf =
            saturate(dot(normal, halfVector));
        const float alpha = roughness * roughness;
        const float denominator =
            normalDotHalf * normalDotHalf
                * (alpha * alpha - 1.0f)
            + 1.0f;
        const float distribution =
            alpha * alpha
            / (3.14159265f
                * denominator * denominator);
        const float pdf =
            distribution * normalDotHalf
                / (4.0f * max(normalDotHalf, 0.0001f))
            + 0.0001f;
        const float saSample =
            1.0f / ((float)SampleCount * pdf);
        const float mip =
            0.5f * log2(saSample / saTexel);
        color +=
            SkyCubemap.SampleLevel(
                LinearSampler,
                lightDirection,
                max(mip, 0.0f)).rgb
            * normalDotLight;
        weight += normalDotLight;
    }
    return float4(color / max(weight, 0.0001f), 1.0f);
}

// 拡散用の放射照度マップ：半球コサイン畳み込み。
float4 PSIrradiance(ScreenVertex input) : SV_Target
{
    const uint face = (uint)PrefilterParameters.x;
    const float3 normal =
        CubeDirection(face, input.uv);
    const float3 up =
        abs(normal.z) < 0.999f
            ? float3(0.0f, 0.0f, 1.0f)
            : float3(1.0f, 0.0f, 0.0f);
    const float3 tangent =
        normalize(cross(up, normal));
    const float3 bitangent = cross(normal, tangent);

    float3 irradiance = 0.0f.xxx;
    float weight = 0.0f;
    const float PhiStep = 6.2831853f / 32.0f;
    const float ThetaStep = 1.5707963f / 8.0f;
    [loop]
    for (uint phiIndex = 0u; phiIndex < 32u; ++phiIndex)
    {
        const float phi = (float)phiIndex * PhiStep;
        [loop]
        for (uint thetaIndex = 0u;
            thetaIndex < 8u;
            ++thetaIndex)
        {
            const float theta =
                ((float)thetaIndex + 0.5f) * ThetaStep;
            const float3 direction =
                tangent * (sin(theta) * cos(phi))
                + bitangent * (sin(theta) * sin(phi))
                + normal * cos(theta);
            const float sampleWeight =
                cos(theta) * sin(theta);
            irradiance +=
                SkyCubemap.SampleLevel(
                    LinearSampler,
                    direction,
                    2.0f).rgb
                * sampleWeight;
            weight += sampleWeight;
        }
    }
    return float4(
        irradiance / max(weight, 0.0001f),
        1.0f);
}
