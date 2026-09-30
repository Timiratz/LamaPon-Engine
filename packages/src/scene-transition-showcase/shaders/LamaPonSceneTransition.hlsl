// scene-transition-showcaseパッケージのシーン遷移シェーダーです。
//
// エンジンは遷移の覆い具合（0～1）だけを計算し、画面を覆う絵は
// 描きません。このパッケージのSceneTransition.Overlayが、画面全体へ
// 引き伸ばした1枚のSpriteにこのシェーダーを使い、覆い具合と見た目を
// CustomParametersで渡します。画素ごとに「覆われているか」を計算し、
// 覆いの色（と縁の差し色）で塗ります。
//
// 独自の遷移シェーダーを書く場合も、このファイルを複製して書き換える
// のが簡単です（プリセットの「独自シェーダー」へ指定すると、同じ
// CustomParametersの並びを受け取ります）。
//
// 【入力の並びは変えないこと】
// SpriteBatchの頂点シェーダーは COLOR0 → TEXCOORD0 → SV_Position の
// 順で出力します。ピクセルシェーダーの引数は入力レジスタへ宣言順に
// 割り当てられるため、この順で書かないと1本ずつずれます。
//
// uv（TEXCOORD0）       = Spriteの左上(0,0)～右下(1,1)
// CustomParameters[0]   = coverage(0～1), ぼかし幅, 差し色の幅, 演出の番号
// CustomParameters[1]   = 順番を反転するか(0/1), stagger, divisions,
//                         模様の番号
// CustomParameters[2]   = 差し色（premultiplyしない色。alpha 0で無効）
// CustomParameters[3]   = 中心x, 中心y（0～1）, 向きx, 向きy（単位ベクトル）
// CustomParameters[4]   = 未使用
// CustomParameters[5]   = 覆いの色（Spriteの色。エンジンが入れます）
// CustomParameters[6]   = Spriteの左上x, y, 幅, 高さ（エンジンが入れます）
// CustomParameters[7]   = UIの幅, 高さ, テクスチャの幅, 高さ
// SpriteTexture（t0）   = ルール画像（指定が無ければ白）
//
// ぼかし幅と差し色の幅は、形の演出では画面の短辺に対する比率、
// Shader演出では覆う順番（0～1）に対する幅です。
//
// 演出の番号（SceneTransitionAssets.hのEffectと同じ）
//   0 = なし, 1 = Fade, 2 = Wipe, 3 = Iris, 4 = Diamond, 5 = Blinds,
//   6 = Tiles, 7 = DiamondTiles, 8 = Dots, 9 = Shutter, 10 = Shader
// 模様の番号（ShaderPatternと同じ。演出がShaderのときに使います）
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
static const float TransitionSqrt2 = 1.41421356f;
static const float TransitionInverseSqrt2 = 0.70710678f;
// 差し色を使う分割型の演出では、差し色が先に伸び、少し遅れて
// 覆いの色が追いかけます。
static const float TransitionAccentLead = 0.25f;
// 「確実に内側／外側」を表す距離です。
static const float TransitionFar = 1.0e6f;

// ---------------------------------------------------------------
// 共通の部品
// ---------------------------------------------------------------

// 境界からの符号付き距離d（正なら内側、UIの単位）を、1画素ぶんの
// 幅aaでなめらかにした塗りの割合です。
float TransitionEdge(float d, float aa)
{
    return saturate(d / aa + 0.5f);
}

// 画面の四隅を向きdirectionへ射影した範囲です（x = 最小, y = 最大）。
float2 TransitionProjectedRange(float2 size, float2 direction)
{
    const float c1 = size.x * direction.x;
    const float c2 = size.y * direction.y;
    const float c3 = dot(size, direction);
    return float2(
        min(min(0.0f, c1), min(c2, c3)),
        max(max(0.0f, c1), max(c2, c3)));
}

// 順番orderの要素が、全体の進み具合progressのときにどこまで
// 進んでいるかです。staggerが大きいほど1つずつ順に動きます。
float TransitionStaggered(float progress, float order, float stagger)
{
    const float spread = clamp(stagger, 0.0f, 0.95f);
    if (spread <= 0.0f)
    {
        return saturate(progress);
    }
    return saturate((progress - saturate(order) * spread) / (1.0f - spread));
}

// 分割型の演出の進み具合です（x = 差し色, y = 覆いの色）。
float2 TransitionLayered(float coverage, bool hasAccent)
{
    if (!hasAccent)
    {
        return float2(0.0f, coverage);
    }
    const float scaled = coverage * (1.0f + TransitionAccentLead);
    return float2(
        min(scaled, 1.0f),
        max(scaled - TransitionAccentLead, 0.0f));
}

// 区間[start, end]が、中心x・幅aaの1画素をどれだけ覆うかです。
// 帯や升目の境界で隣の区間の値と足し合わせると、継ぎ目に線が
// 出ずに合計がちょうど1になります。
float TransitionInterval(float start, float end, float x, float aa)
{
    if (end <= start)
    {
        return 0.0f;
    }
    return saturate((end - x) / aa + 0.5f) - saturate((start - x) / aa + 0.5f);
}

// 端から順に覆う演出（Wipe／Iris／Diamond／Shutter）の2層の塗りです。
// tは覆われる順番の位置（0で最初、spanで最後、UIの単位）です。
// 戻り値のxが差し色の層、yが覆いの色の層です。
float2 TransitionEdgeLayers(
    float t,
    float span,
    float coverage,
    float accentWidth,
    float softness,
    float aa)
{
    // coverage 0で何も覆わず、1で差し色とぼかしを含めて覆い切るように
    // 前線を広げます。
    const float front =
        coverage * (span + accentWidth + softness) - (accentWidth + softness);
    const float inside = front - t;
    const float outer = inside + accentWidth;
    const float outerAlpha = softness > aa
        ? saturate((outer + softness) / softness)
        : TransitionEdge(outer, aa);
    if (accentWidth > 0.0f)
    {
        return float2(outerAlpha, TransitionEdge(inside, aa));
    }
    return float2(0.0f, outerAlpha);
}

// ---------------------------------------------------------------
// Shader演出の模様
// ---------------------------------------------------------------

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
    const float2 extent = max(abs(center), abs(size - center));
    return max(length(extent), 1.0f);
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

// 画素ごとの「覆われる順番」（0で最初、1で最後）です。
float TransitionPatternOrder(
    int pattern,
    float2 pixel,
    float2 size,
    float2 center,
    float2 direction,
    float stagger,
    float divisions,
    float3 rule)
{
    const float2 delta = pixel - center;
    const float farthest = TransitionFarthestCorner(center, size);
    const float radius = saturate(length(delta) / farthest);
    // 12時の位置を0として時計回りに増える角度（0～1）です。
    // 画面はy下向きなので、上向きは-yです。
    const float angle =
        frac(atan2(delta.x, -delta.y) / TransitionTau + 1.0f);

    if (pattern == 0)
    {
        // RuleImage: 暗い画素から順に覆います。
        return dot(rule, float3(0.299f, 0.587f, 0.114f));
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
        const float2 range = TransitionProjectedRange(size, direction);
        const float order = saturate(
            (dot(tileCenter, direction) - range.x)
            / max(range.y - range.x, 1.0f));
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
    // Dissolve（未知の番号も同じ）: 細かさはdivisions（画面の横に並ぶ
    // ノイズの数）です。
    const float noiseCell = size.x / divisions;
    return saturate(
        (TransitionFractalNoise(pixel / noiseCell) - 0.2f) / 0.6f);
}

// ---------------------------------------------------------------
// 本体
// ---------------------------------------------------------------

float4 PSMain(
    float4 color : COLOR0,
    float2 uv : TEXCOORD0,
    float4 position : SV_Position) : SV_Target
{
    const float coverage = saturate(CustomParameters[0].x);
    const float softnessRatio = max(CustomParameters[0].y, 0.0f);
    const float accentRatio = max(CustomParameters[0].z, 0.0f);
    const int effect = (int)round(CustomParameters[0].w);
    const bool reverse = CustomParameters[1].x >= 0.5f;
    const float stagger = saturate(CustomParameters[1].y);
    const float divisions = max(CustomParameters[1].z, 1.0f);
    const int pattern = (int)round(CustomParameters[1].w);
    const float4 accent = CustomParameters[2];
    const float2 focus = CustomParameters[3].xy;
    const float2 direction = CustomParameters[3].zw;
    // COLOR0はpremultiply済みなので、元の色はCustomParameters[5]から読みます。
    const float4 coverColor = CustomParameters[5];
    const float2 size = max(CustomParameters[6].zw, float2(1.0f, 1.0f));

    const float2 pixel = uv * size;
    // 1画素がUIの単位でどれだけの幅かです。図形の縁をなめらかにします。
    // 微分は分岐の外で求めます（分岐内だと微分が不定になるため）。
    const float2 pixelStep = fwidth(pixel);
    const float aa = max(max(pixelStep.x, pixelStep.y), 0.0001f);
    const float3 rule = SpriteTexture.Sample(SpriteSampler, uv).rgb;

    const float shortSide = min(size.x, size.y);
    const bool hasAccent = accent.a > 0.0f && accentRatio > 0.0f;
    const float accentWidth = hasAccent ? accentRatio * shortSide : 0.0f;
    const float softness = softnessRatio * shortSide;
    const float2 center = focus * size;
    const float2 range = TransitionProjectedRange(size, direction);
    const float along = dot(pixel, direction) - range.x;
    const float span = max(range.y - range.x, 1.0f);

    // x = 差し色の層、y = 覆いの色の層の塗りの割合です。
    float2 layers = float2(0.0f, 0.0f);
    if (effect == 0)
    {
        layers = float2(0.0f, 0.0f);
    }
    else if (effect == 2)
    {
        // Wipe: 画面の端から伸びます。
        layers = TransitionEdgeLayers(
            along, span, coverage, accentWidth, softness, aa);
    }
    else if (effect == 9)
    {
        // Shutter: 両側から中央へ閉じます。左右の覆いは重ならないので、
        // 足し合わせると中央で合わさる瞬間にも線が残りません。
        const float halfSpan = span * 0.5f;
        layers = saturate(
            TransitionEdgeLayers(
                along, halfSpan, coverage, accentWidth, softness, aa)
            + TransitionEdgeLayers(
                span - along, halfSpan, coverage, accentWidth, softness, aa));
    }
    else if (effect == 3 || effect == 4)
    {
        // Iris／Diamond: 最も遠い角から中心へ向かって閉じます。
        // ひし形はマンハッタン距離を辺に垂直な距離へ直して使います。
        const float2 delta = abs(pixel - center);
        const float2 extent = max(center, size - center);
        const float distance = effect == 4
            ? (delta.x + delta.y) * TransitionInverseSqrt2
            : length(delta);
        const float farthest = max(
            effect == 4
                ? (extent.x + extent.y) * TransitionInverseSqrt2
                : length(extent),
            1.0f);
        layers = TransitionEdgeLayers(
            farthest - distance,
            farthest,
            coverage,
            accentWidth,
            softness,
            aa);
    }
    else if (effect == 5)
    {
        // Blinds: divisions本の帯が順に伸びます。隣の帯の伸びた部分も
        // 同じ画素へ足し合わせ、帯の継ぎ目をなめらかにします。
        const float stripe = span / divisions;
        const float index = floor(along / stripe);
        const float2 progress = TransitionLayered(coverage, hasAccent);
        [unroll]
        for (int neighbor = -1; neighbor <= 1; ++neighbor)
        {
            const float stripeIndex = index + (float)neighbor;
            if (stripeIndex < 0.0f || stripeIndex > divisions - 1.0f)
            {
                continue;
            }
            const float order = divisions > 1.0f
                ? stripeIndex / (divisions - 1.0f)
                : 0.0f;
            const float start = stripeIndex * stripe;
            const float accentEnd = start
                + stripe * TransitionStaggered(progress.x, order, stagger);
            const float mainEnd = start
                + stripe * TransitionStaggered(progress.y, order, stagger);
            layers += float2(
                hasAccent ? TransitionInterval(start, accentEnd, along, aa) : 0.0f,
                TransitionInterval(start, mainEnd, along, aa));
        }
        layers = saturate(layers);
    }
    else if (effect == 6 || effect == 7 || effect == 8)
    {
        // Tiles／DiamondTiles／Dots: 横にdivisions個並ぶ升目の図形が、
        // 向きに沿って順に膨らみます。図形は隣の升目へはみ出すので、
        // 周囲の升目も調べます。
        const float cell = size.x / divisions;
        const float2 ownCell = floor(pixel / cell);
        const float2 progress = TransitionLayered(coverage, hasAccent);
        // 四角は重ならないので塗りを足し合わせ、ひし形とドットは
        // 最も内側にある図形までの距離で塗ります。
        float2 squareCoverage = float2(0.0f, 0.0f);
        float2 distance = float2(-TransitionFar, -TransitionFar);
        [unroll]
        for (int y = -1; y <= 1; ++y)
        {
            [unroll]
            for (int x = -1; x <= 1; ++x)
            {
                const float2 tileCenter =
                    (ownCell + float2((float)x, (float)y) + 0.5f) * cell;
                const float2 offset = pixel - tileCenter;
                const float2 local = abs(offset);
                const float order = saturate(
                    (dot(tileCenter, direction) - range.x) / span);
                const float2 piece = float2(
                    TransitionStaggered(progress.x, order, stagger),
                    TransitionStaggered(progress.y, order, stagger));
                if (effect == 6)
                {
                    // 隣の升目と1だけ重ね、継ぎ目の隙間を防ぎます。
                    const float2 halfSize = piece * (cell + 1.0f) * 0.5f;
                    squareCoverage += float2(
                        TransitionInterval(-halfSize.x, halfSize.x, offset.x, aa)
                            * TransitionInterval(-halfSize.x, halfSize.x, offset.y, aa),
                        TransitionInterval(-halfSize.y, halfSize.y, offset.x, aa)
                            * TransitionInterval(-halfSize.y, halfSize.y, offset.y, aa));
                    continue;
                }
                float2 pieceDistance;
                if (effect == 7)
                {
                    // 45度回した正方形。辺に垂直な距離で比べます。
                    const float2 radius = piece * (cell + TransitionInverseSqrt2);
                    pieceDistance =
                        (radius - (local.x + local.y)) * TransitionInverseSqrt2;
                }
                else
                {
                    const float2 radius =
                        piece * (cell * TransitionInverseSqrt2 + 0.5f);
                    pieceDistance = radius - length(local);
                }
                // 進み具合0の図形は描きません。自分の升目の図形は進み具合1で
                // 升目を必ず覆い切るので、境界のなめらかさで角を透かしません。
                const bool ownPiece = x == 0 && y == 0;
                pieceDistance = float2(
                    piece.x <= 0.0f
                        ? -TransitionFar
                        : (ownPiece && piece.x >= 1.0f
                            ? TransitionFar
                            : pieceDistance.x),
                    piece.y <= 0.0f
                        ? -TransitionFar
                        : (ownPiece && piece.y >= 1.0f
                            ? TransitionFar
                            : pieceDistance.y));
                distance = max(distance, pieceDistance);
            }
        }
        if (effect == 6)
        {
            layers = saturate(float2(
                hasAccent ? squareCoverage.x : 0.0f,
                squareCoverage.y));
        }
        else
        {
            layers = float2(
                hasAccent ? TransitionEdge(distance.x, aa) : 0.0f,
                TransitionEdge(distance.y, aa));
        }
    }
    else if (effect == 10)
    {
        // Shader: 模様の「覆われる順番」が早い画素から覆います。
        float order = saturate(TransitionPatternOrder(
            pattern, pixel, size, center, direction, stagger, divisions, rule));
        if (reverse)
        {
            // 通り抜ける演出の開く段階では、先に覆った画素から開けます。
            order = 1.0f - order;
        }
        const float patternSoftness = max(softnessRatio, 0.0001f);
        const float patternAccent = hasAccent ? accentRatio : 0.0f;
        const float front = coverage * (1.0f + patternSoftness + patternAccent)
            - patternAccent;
        layers = float2(
            hasAccent
                ? saturate((front + patternAccent - order) / patternSoftness)
                : 0.0f,
            saturate((front - order) / patternSoftness));
    }
    else
    {
        // Fade（未知の番号も同じ）: 全体を少しずつ覆います。
        layers = float2(0.0f, coverage);
    }

    if (effect != 0 && coverage >= 0.9999f)
    {
        // 覆い切った状態では、縁のなめらかさで角が透けないようにします。
        layers = float2(0.0f, 1.0f);
    }

    // 覆いの色を差し色の上へ重ねます。
    const float mainAlpha = layers.y * coverColor.a;
    const float accentAlpha = layers.x * accent.a * (1.0f - mainAlpha);
    const float alpha = mainAlpha + accentAlpha;
    clip(alpha - 0.0001f);
    const float3 rgb =
        (coverColor.rgb * mainAlpha + accent.rgb * accentAlpha)
        / max(alpha, 0.0001f);
    return float4(rgb, saturate(alpha));
}
