// LamaPonに組み込まれているシーン遷移（Shader演出）用のシェーダーです。
//
// SceneTransitionEffect::Shaderのとき、エンジンが画面全体を1枚の
// Spriteで覆ってこのピクセルシェーダーを使います。画素ごとに
// 「覆われる順番」（0で最初、1で最後）を模様から計算し、進み具合
// coverageより順番が早い画素だけを覆いの色で塗ります。
//
// 独自のシーン遷移シェーダーを書く場合も、このファイルを複製して
// PatternOrderだけを書き換えるのが簡単です（入力と定数の並びは
// 同じものが渡されます）。
//
// 【入力の並びは変えないこと】
// SpriteBatchの頂点シェーダーは COLOR0 → TEXCOORD0 → SV_Position の
// 順で出力します。ピクセルシェーダーの引数は入力レジスタへ宣言順に
// 割り当てられるため、この順で書かないと1本ずつずれます。
//
// color（COLOR0）       = 覆いの色（premultiplyしない色）
// uv（TEXCOORD0）       = 画面の左上(0,0)～右下(1,1)
// CustomParameters[0]   = coverage(0～1), 境界のぼかし幅, 差し色の幅,
//                         模様の番号
// CustomParameters[1]   = 順番を反転するか(0/1), stagger, divisions, 未使用
// CustomParameters[2]   = 差し色（premultiplyしない色。alpha 0で無効）
// CustomParameters[3]   = 画面の幅, 高さ, 中心x, 中心y（ピクセル）
// CustomParameters[4]   = 向きx, 向きy（単位ベクトル）, 未使用, 未使用
// CustomParameters[5～7]はSprite描画時にエンジンが上書きするため
// 使いません。
// SpriteTexture（t0）   = ルール画像（指定が無ければ白）
//
// 模様の番号はSceneTransitionShaderPatternと同じです。
//   0 = RuleImage, 1 = Dissolve, 2 = Clock, 3 = Spiral,
//   4 = Ripple, 5 = Hexagons, 6 = Heart

Texture2D SpriteTexture : register(t0);
SamplerState SpriteSampler : register(s0);

cbuffer SpriteParameters : register(b0)
{
    float4 CustomParameters[8];
};

static const float TransitionPi = 3.14159265f;
static const float TransitionTau = 6.28318531f;

// 整数演算だけで作る擬似乱数です（LamaPonNoise.hlsliと同じハッシュ）。
uint TransitionHash(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

float TransitionCellRandom(int2 cell)
{
    const uint hashed = TransitionHash(
        asuint(cell.x) * 0x8da6b343u
        ^ TransitionHash(asuint(cell.y) + 0x68e31da4u));
    return (float)(hashed >> 8u) / 16777216.0f;
}

float TransitionValueNoise(float2 position)
{
    const float2 cellOrigin = floor(position);
    const float2 fraction = position - cellOrigin;
    const float2 blend = fraction * fraction * (3.0f - 2.0f * fraction);
    const int2 cell = (int2)cellOrigin;
    const float a = TransitionCellRandom(cell);
    const float b = TransitionCellRandom(cell + int2(1, 0));
    const float c = TransitionCellRandom(cell + int2(0, 1));
    const float d = TransitionCellRandom(cell + int2(1, 1));
    return lerp(lerp(a, b, blend.x), lerp(c, d, blend.x), blend.y);
}

float TransitionFractalNoise(float2 position)
{
    float sum = 0.0f;
    float amplitude = 0.5f;
    float normalization = 0.0f;
    [unroll]
    for (int octave = 0; octave < 4; ++octave)
    {
        sum += amplitude * TransitionValueNoise(position);
        normalization += amplitude;
        position = position * 2.03f + float2(17.1f, 9.7f);
        amplitude *= 0.5f;
    }
    return sum / normalization;
}

// 負の値でも正の余りを返します（HLSLのfmodは被除数の符号に従うため）。
float2 TransitionPositiveModulo(float2 value, float2 divisor)
{
    return value - divisor * floor(value / divisor);
}

// 画面の四隅のうち、中心から最も遠い距離です。
float TransitionFarthestCorner(float2 center, float2 size)
{
    const float2 nearCorner = abs(center);
    const float2 farCorner = abs(size - center);
    const float2 extent = max(nearCorner, farCorner);
    return max(length(extent), 1.0f);
}

// 画面の四隅を向きdirectionへ射影した範囲で、positionの位置を
// 0～1へ正規化します。
float TransitionAlongDirection(
    float2 position,
    float2 size,
    float2 direction)
{
    const float c0 = 0.0f;
    const float c1 = size.x * direction.x;
    const float c2 = size.y * direction.y;
    const float c3 = dot(size, direction);
    const float minimum = min(min(c0, c1), min(c2, c3));
    const float maximum = max(max(c0, c1), max(c2, c3));
    return saturate(
        (dot(position, direction) - minimum)
        / max(maximum - minimum, 1.0f));
}

// Inigo Quilez氏のハートの距離関数（y上向き、先端が原点）です。
float TransitionHeartDistance(float2 position)
{
    position.x = abs(position.x);
    if (position.y + position.x > 1.0f)
    {
        const float2 delta = position - float2(0.25f, 0.75f);
        return sqrt(dot(delta, delta)) - 0.35355339f;
    }
    const float2 top = position - float2(0.0f, 1.0f);
    const float2 diagonal =
        position - 0.5f * max(position.x + position.y, 0.0f);
    return sqrt(min(dot(top, top), dot(diagonal, diagonal)))
        * sign(position.x - position.y);
}

// 画面座標をハートの座標系（中心付近が(0, 0.5)）へ移します。
float TransitionHeartAt(float2 pixel, float2 center, float scale)
{
    const float2 local = float2(
        (pixel.x - center.x) / scale,
        (center.y - pixel.y) / scale + 0.5f);
    return TransitionHeartDistance(local);
}

float PatternOrder(float2 uv)
{
    const float2 size = max(CustomParameters[3].xy, float2(1.0f, 1.0f));
    const float2 center = CustomParameters[3].zw;
    const float2 direction = CustomParameters[4].xy;
    const float stagger = saturate(CustomParameters[1].y);
    const float divisions = max(CustomParameters[1].z, 1.0f);
    const int pattern = (int)round(CustomParameters[0].w);
    const float2 pixel = uv * size;
    const float2 delta = pixel - center;
    const float farthest = TransitionFarthestCorner(center, size);
    const float radius = saturate(length(delta) / farthest);
    // 12時の位置を0として時計回りに増える角度（0～1）です。
    // 画面はy下向きなので、上向きは-yです。
    const float angle =
        frac(atan2(delta.x, -delta.y) / TransitionTau + 1.0f);
    // 勾配を使うSampleは分岐の外で行います（分岐内だとミップ選択の
    // 微分が不定になり、FXCが警告を出すため）。
    const float3 rule = SpriteTexture.Sample(SpriteSampler, uv).rgb;

    if (pattern == 0)
    {
        // RuleImage: 暗い画素から順に覆います。
        return dot(rule, float3(0.299f, 0.587f, 0.114f));
    }
    if (pattern == 1)
    {
        // Dissolve: 細かさはdivisions（画面の横に並ぶノイズの数）です。
        const float cell = size.x / divisions;
        const float noise = TransitionFractalNoise(pixel / cell);
        return saturate((noise - 0.2f) / 0.6f);
    }
    if (pattern == 2)
    {
        // Clock: 左向き系の方向なら反時計回りにします。
        const bool counterClockwise =
            direction.x < -0.001f
            || (abs(direction.x) <= 0.001f && direction.y < 0.0f);
        return counterClockwise ? frac(1.0f - angle) : angle;
    }
    if (pattern == 3)
    {
        // Spiral: 外側から、divisions本の渦の腕に沿って閉じます。
        const float arms = min(divisions, 8.0f);
        const float swirl = frac(angle * arms + radius * 1.5f);
        return saturate((1.0f - radius) * 0.65f + swirl * 0.35f);
    }
    if (pattern == 4)
    {
        // Ripple: 外側から閉じる円に波の揺らぎを重ねます。
        const float wave =
            0.5f + 0.5f * sin(radius * divisions * TransitionPi);
        return saturate((1.0f - radius) * 0.8f + wave * 0.2f);
    }
    if (pattern == 5)
    {
        // Hexagons: 横にdivisions個並ぶ六角形のタイルが、向きに沿って
        // 中心から膨らむように現れます。
        const float cell = size.x / divisions;
        const float2 scaled = pixel / cell;
        const float2 spacing = float2(1.0f, 1.7320508f);
        const float2 halfSpacing = spacing * 0.5f;
        const float2 a =
            TransitionPositiveModulo(scaled, spacing) - halfSpacing;
        const float2 b =
            TransitionPositiveModulo(scaled - halfSpacing, spacing)
            - halfSpacing;
        const float2 local = dot(a, a) < dot(b, b) ? a : b;
        const float2 tileCenter = (scaled - local) * cell;
        const float2 absolute = abs(local);
        // 中心0、辺で1になる六角形の距離です。
        const float hexagon = max(
            dot(absolute, normalize(float2(1.0f, 1.7320508f))),
            absolute.x) / 0.5f;
        const float order =
            TransitionAlongDirection(tileCenter, size, direction);
        return saturate(
            order * stagger + saturate(hexagon) * (1.0f - stagger));
    }
    if (pattern == 6)
    {
        // Heart: ハートの距離関数を、中心で0、最も遠い角で1になるよう
        // 正規化し、外側から閉じます。
        const float scale = max(min(size.x, size.y) * 0.5f, 1.0f);
        const float inside = TransitionHeartAt(center, center, scale);
        const float outside = max(
            max(
                TransitionHeartAt(float2(0.0f, 0.0f), center, scale),
                TransitionHeartAt(float2(size.x, 0.0f), center, scale)),
            max(
                TransitionHeartAt(float2(0.0f, size.y), center, scale),
                TransitionHeartAt(size, center, scale)));
        const float distance = TransitionHeartAt(pixel, center, scale);
        return 1.0f
            - saturate((distance - inside) / max(outside - inside, 0.001f));
    }
    // 未知の番号はDissolveと同じにします。
    const float fallbackCell = size.x / divisions;
    return saturate(
        (TransitionFractalNoise(pixel / fallbackCell) - 0.2f) / 0.6f);
}

float4 PSMain(
    float4 color : COLOR0,
    float2 uv : TEXCOORD0,
    float4 position : SV_Position) : SV_Target
{
    const float coverage = saturate(CustomParameters[0].x);
    const float softness = max(CustomParameters[0].y, 0.0001f);
    const float accentWidth = max(CustomParameters[0].z, 0.0f);
    const float4 accent = CustomParameters[2];

    float order = saturate(PatternOrder(uv));
    if (CustomParameters[1].x >= 0.5f)
    {
        // 通り抜ける演出の開く段階では、先に覆った画素から開けます。
        order = 1.0f - order;
    }

    // coverage 0で何も覆わず、1で全画素（ぼかしと差し色を含む）を
    // 覆い切るように前線の位置を広げます。
    const float front =
        coverage * (1.0f + softness + accentWidth) - accentWidth;
    const float cover = saturate((front - order) / softness);
    const float glow =
        saturate((front + accentWidth - order) / softness);
    const float mainAlpha = cover * color.a;
    const float accentAlpha = (glow - cover) * accent.a;
    const float alpha = mainAlpha + accentAlpha;
    clip(alpha - 0.0001f);
    const float3 rgb =
        (color.rgb * mainAlpha + accent.rgb * accentAlpha)
        / max(alpha, 0.0001f);
    return float4(rgb, saturate(alpha));
}
