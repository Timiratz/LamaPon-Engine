// SceneTransition.Overlay用の全画面Sprite Shaderです。
// Engineはcoverageを計算し、このShaderは各画素の覆い形状と色を描きます。
// 頂点Shaderの出力順COLOR0, TEXCOORD0, SV_Positionを維持してください。
// Pixel Shader入力は宣言順で割り当てられ、並びが違うと値がずれます。
// uv: Sprite左上(0,0)～右下(1,1)。
// CustomParameters[0]: coverage, softness, accentWidth, effect。
// CustomParameters[1]: reverse, stagger, divisions, pattern。
// CustomParameters[2]: 差し色RGBA（非premultiply、alpha 0で無効）。
// CustomParameters[3]: focus.xy（0～1）, direction.zw（単位ベクトル）。
// CustomParameters[4]: 未使用。
// CustomParameters[5]: 覆い色（Sprite color、Engine設定）。
// CustomParameters[6]: Sprite矩形（左上xy、幅高さ、Engine設定）。
// CustomParameters[7]: UIとTextureの幅・高さ。SpriteTexture(t0): Rule画像。
// softnessとaccentWidthは形状演出では短辺比、Shader演出ではcoverage比です。
// effect番号: 0 None, 1 Fade, 2 Wipe, 3 Iris, 4 Diamond, 5 Blinds, 6 Tiles, 7 DiamondTiles, 8 Dots, 9 Shutter, 10 Shader。
// pattern番号: 0 RuleImage, 1 Dissolve, 2 Clock, 3 Spiral, 4 Ripple, 5 Hexagons, 6 Heart。

// Sprite画像とRule画像を読み込みます。
Texture2D SpriteTexture : register(t0);
// Sprite画像のサンプラー状態
SamplerState SpriteSampler : register(s0);

// エンジンから受け取る遷移設定
cbuffer SpriteParameters : register(b0)
{
    // 演出設定とSprite情報
    float4 CustomParameters[8];
};

// 円周率
static const float TransitionPi = 3.14159265f;
// 円周率の2倍
static const float TransitionTau = 6.28318531f;
// 斜め距離の換算係数
static const float TransitionSqrt2 = 1.41421356f;
// 斜め距離の逆換算係数
static const float TransitionInverseSqrt2 = 0.70710678f;
// 差し色を使う分割型の演出では、差し色が先に伸び、少し遅れて覆いの色が追いかけます。
static const float TransitionAccentLead = 0.25f;
// 「確実に内側／外側」を表す距離です。
static const float TransitionFar = 1.0e6f;

// 境界からの符号付き距離d（正なら内側、UIの単位）を、1画素ぶんの幅aaでなめらかにした塗りの割合です。
// 1画素幅で境界距離を塗り率へ変換します(d: 境界距離, aa: 1画素幅)。
float TransitionEdge(float d, float aa)
{
    return saturate(d / aa + 0.5f);
}

// 画面の四隅を向きdirectionへ射影した範囲です（x = 最小, y = 最大）。
// 画面四隅の射影範囲を返します(size: 描画寸法, direction: 射影方向)。
float2 TransitionProjectedRange(float2 size, float2 direction)
{
    // 横幅の射影値
    const float c1 = size.x * direction.x;
    // 高さの射影値
    const float c2 = size.y * direction.y;
    // 対角の射影値
    const float c3 = dot(size, direction);
    return float2(
        min(min(0.0f, c1), min(c2, c3)),
        max(max(0.0f, c1), max(c2, c3)));
}

// 分割要素の進み具合を返します(progress: 全体進捗, order: 順番, stagger: 遅延率)。
float TransitionStaggered(float progress, float order, float stagger)
{
    // 0～0.95に制限した遅延率
    const float spread = clamp(stagger, 0.0f, 0.95f);
    // 遅延なしなら全体進捗を使います。
    if (spread <= 0.0f)
    {
        return saturate(progress);
    }
    return saturate((progress - saturate(order) * spread) / (1.0f - spread));
}

// 差し色と覆いの進み具合を返します(coverage: 全体進捗, hasAccent: 差し色の有無)。
float2 TransitionLayered(float coverage, bool hasAccent)
{
    // 差し色がない場合は覆いだけ進めます。
    if (!hasAccent)
    {
        return float2(0.0f, coverage);
    }
    // 差し色の先行分を含む進捗
    const float scaled = coverage * (1.0f + TransitionAccentLead);
    return float2(
        min(scaled, 1.0f),
        max(scaled - TransitionAccentLead, 0.0f));
}

// 1画素と区間の重なり率を返します(start: 区間始点, end: 区間終点, x: 画素中心, aa: 画素幅)。
float TransitionInterval(float start, float end, float x, float aa)
{
    // 空区間は覆いません。
    if (end <= start)
    {
        return 0.0f;
    }
    return saturate((end - x) / aa + 0.5f) - saturate((start - x) / aa + 0.5f);
}

// Shape演出の2層塗りを返します(t: 進行位置, span: 全長, coverage: 進捗, accentWidth: 差し色幅, softness: ぼかし幅, aa: 画素幅)。
float2 TransitionEdgeLayers(
    float t,
    float span,
    float coverage,
    float accentWidth,
    float softness,
    float aa)
{
    // 進捗に合わせたShape前線
    const float front =
        coverage * (span + accentWidth + softness) - (accentWidth + softness);
    // 画素位置から覆い色境界までの距離
    const float inside = front - t;
    // 差し色境界までの距離
    const float outer = inside + accentWidth;
    // ぼかし幅を反映した差し色の塗り率
    const float outerAlpha = softness > aa
        ? saturate((outer + softness) / softness)
        : TransitionEdge(outer, aa);
    // 差し色帯を描く場合は両層の塗り率を返します。
    if (accentWidth > 0.0f)
    {
        return float2(outerAlpha, TransitionEdge(inside, aa));
    }
    return float2(0.0f, outerAlpha);
}

// 整数ハッシュを計算します(value: 入力値)。
uint TransitionHash(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

// 格子セルごとの再現可能な乱数を返します(cell: 格子座標)。
float TransitionCellRandom(int2 cell)
{
    // セル座標を混合したハッシュ値
    const uint hashed = TransitionHash(
        asuint(cell.x) * 0x8da6b343u
        ^ TransitionHash(asuint(cell.y) + 0x68e31da4u));
    return (float)(hashed >> 8u) / 16777216.0f;
}

// 格子点から2D value noiseを補間します(position: ノイズ座標)。
float TransitionValueNoise(float2 position)
{
    // 座標が属する格子セルの原点
    const float2 cellOrigin = floor(position);
    // セル内の小数座標
    const float2 fraction = position - cellOrigin;
    // 滑らかな補間係数
    const float2 blend = fraction * fraction * (3.0f - 2.0f * fraction);
    // 整数格子座標
    const int2 cell = (int2)cellOrigin;
    // 左上セル値
    const float a = TransitionCellRandom(cell);
    // 右上セル値
    const float b = TransitionCellRandom(cell + int2(1, 0));
    // 左下セル値
    const float c = TransitionCellRandom(cell + int2(0, 1));
    // 右下セル値
    const float d = TransitionCellRandom(cell + int2(1, 1));
    return lerp(lerp(a, b, blend.x), lerp(c, d, blend.x), blend.y);
}

// 4段階のFractal noiseを合成します(position: ノイズ座標)。
float TransitionFractalNoise(float2 position)
{
    // 各Octaveの加重ノイズ合計
    float sum = 0.0f;
    // 現在のOctaveの重み
    float amplitude = 0.5f;
    // 重み合計による正規化値
    float normalization = 0.0f;
    [unroll]
    // 低周波から高周波へ4段階加算します(octave: 周波数段階)。
    for (int octave = 0; octave < 4; ++octave)
    {
        sum += amplitude * TransitionValueNoise(position);
        normalization += amplitude;
        position = position * 2.03f + float2(17.1f, 9.7f);
        amplitude *= 0.5f;
    }
    return sum / normalization;
}

// 各成分の正の剰余を返します(value: 座標, divisor: 周期)。
float2 TransitionPositiveModulo(float2 value, float2 divisor)
{
    return value - divisor * floor(value / divisor);
}

// 画面四隅までの最大距離を返します(center: 基準位置, size: 画面寸法)。
float TransitionFarthestCorner(float2 center, float2 size)
{
    // 中心から各軸で遠い方の距離
    const float2 extent = max(abs(center), abs(size - center));
    return max(length(extent), 1.0f);
}

// Heart輪郭までの符号付き距離を返します(position: y上向き座標)。
float TransitionHeartDistance(float2 position)
{
    position.x = abs(position.x);
    // 右上の円弧領域を判定します。
    if (position.y + position.x > 1.0f)
    {
        // 円弧中心からの差
        const float2 delta = position - float2(0.25f, 0.75f);
        return sqrt(dot(delta, delta)) - 0.35355339f;
    }
    // 上部先端からの差
    const float2 top = position - float2(0.0f, 1.0f);
    // Heart中央の斜線からの差
    const float2 diagonal =
        position - 0.5f * max(position.x + position.y, 0.0f);
    return sqrt(min(dot(top, top), dot(diagonal, diagonal)))
        * sign(position.x - position.y);
}

// 画面座標をHeart用座標へ変換します(pixel: 画素位置, center: 中心, scale: 大きさ)。
float TransitionHeartAt(float2 pixel, float2 center, float scale)
{
    // Heart距離関数へ渡す局所座標
    const float2 local = float2(
        (pixel.x - center.x) / scale,
        (center.y - pixel.y) / scale + 0.5f);
    return TransitionHeartDistance(local);
}

// Pattern上の覆われる順番を返します(pattern: 模様番号, pixel: 画素位置, size: 画面寸法, center: 中心, direction: 向き, stagger: 時間差, divisions: 分割数, rule: Rule画像色)。
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
    // Pattern中心からの画素差
    const float2 delta = pixel - center;
    // 中心から画面角までの最大距離
    const float farthest = TransitionFarthestCorner(center, size);
    // 画面角までの正規化距離
    const float radius = saturate(length(delta) / farthest);
    // 12時起点で時計回りに進む正規化角度
    const float angle =
        frac(atan2(delta.x, -delta.y) / TransitionTau + 1.0f);

    // Rule画像の輝度を進行順として使います。
    if (pattern == 0)
    {
        return dot(rule, float3(0.299f, 0.587f, 0.114f));
    }
    // Clockの回転方向を向きから選びます。
    if (pattern == 2)
    {
        // 反時計回りに進む向きか
        const bool counterClockwise =
            direction.x < -0.001f
            || (abs(direction.x) <= 0.001f && direction.y < 0.0f);
        return counterClockwise ? frac(1.0f - angle) : angle;
    }
    // 外側から渦巻き状に覆います。
    if (pattern == 3)
    {
        // 最大8本の渦の腕
        const float arms = min(divisions, 8.0f);
        // 半径と角度から求めた渦位相
        const float swirl = frac(angle * arms + radius * 1.5f);
        return saturate((1.0f - radius) * 0.65f + swirl * 0.35f);
    }
    // 外側から波紋状に覆います。
    if (pattern == 4)
    {
        // 半径に応じた波の振幅
        const float wave =
            0.5f + 0.5f * sin(radius * divisions * TransitionPi);
        return saturate((1.0f - radius) * 0.8f + wave * 0.2f);
    }
    // 画面中心から方向順に六角形を膨らませます。
    if (pattern == 5)
    {
        // 六角形タイルのセル幅
        const float cell = size.x / divisions;
        // 画素位置のセル座標
        const float2 scaled = pixel / cell;
        // 六角形格子の間隔
        const float2 spacing = float2(1.0f, 1.7320508f);
        // 格子間隔の半分
        const float2 halfSpacing = spacing * 0.5f;
        // 1つ目の格子候補からの位置
        const float2 a =
            TransitionPositiveModulo(scaled, spacing) - halfSpacing;
        // 2つ目の格子候補からの位置
        const float2 b =
            TransitionPositiveModulo(scaled - halfSpacing, spacing)
            - halfSpacing;
        // 最寄り六角形格子内の位置
        const float2 local = dot(a, a) < dot(b, b) ? a : b;
        // 六角形格子の中心
        const float2 tileCenter = (scaled - local) * cell;
        // 中心からの軸別距離
        const float2 absolute = abs(local);
        // 六角形中心0・辺1の正規化距離
        const float hexagon = max(
            dot(absolute, normalize(float2(1.0f, 1.7320508f))),
            absolute.x) / 0.5f;
        // 向き方向の画面範囲
        const float2 range = TransitionProjectedRange(size, direction);
        // タイル中心の出現順
        const float order = saturate(
            (dot(tileCenter, direction) - range.x)
            / max(range.y - range.x, 1.0f));
        return saturate(
            order * stagger + saturate(hexagon) * (1.0f - stagger));
    }
    // Heart形状の距離から外側順の進捗を求めます。
    if (pattern == 6)
    {
        // 画面短辺に合わせたHeart座標スケール
        const float scale = max(min(size.x, size.y) * 0.5f, 1.0f);
        // Heart中心での符号付き距離
        const float inside = TransitionHeartAt(center, center, scale);
        // 画面四隅での最大距離
        const float outside = max(
            max(
                TransitionHeartAt(float2(0.0f, 0.0f), center, scale),
                TransitionHeartAt(float2(size.x, 0.0f), center, scale)),
            max(
                TransitionHeartAt(float2(0.0f, size.y), center, scale),
                TransitionHeartAt(size, center, scale)));
        // 現在画素でのHeart距離
        const float distance = TransitionHeartAt(pixel, center, scale);
        return 1.0f
            - saturate((distance - inside) / max(outside - inside, 0.001f));
    }
    // Dissolve用のNoiseセル幅（未知Patternもこの分岐を使います）。
    const float noiseCell = size.x / divisions;
    return saturate(
        (TransitionFractalNoise(pixel / noiseCell) - 0.2f) / 0.6f);
}

// Sprite領域へ遷移の覆いを描きます(color: Sprite色, uv: Sprite内UV, position: 画面位置)。
float4 PSMain(
    float4 color : COLOR0,
    float2 uv : TEXCOORD0,
    float4 position : SV_Position) : SV_Target
{
    // 全体の遷移進捗
    const float coverage = saturate(CustomParameters[0].x);
    // 形状境界のぼかし比率
    const float softnessRatio = max(CustomParameters[0].y, 0.0f);
    // 差し色帯の幅比率
    const float accentRatio = max(CustomParameters[0].z, 0.0f);
    // 遷移演出番号
    const int effect = (int)round(CustomParameters[0].w);
    // 開閉を逆向きにするか
    const bool reverse = CustomParameters[1].x >= 0.5f;
    // 分割要素の時間差
    const float stagger = saturate(CustomParameters[1].y);
    // 横方向の分割数
    const float divisions = max(CustomParameters[1].z, 1.0f);
    // Shader演出の模様番号
    const int pattern = (int)round(CustomParameters[1].w);
    // 差し色と不透明度
    const float4 accent = CustomParameters[2];
    // 演出の中心位置比率
    const float2 focus = CustomParameters[3].xy;
    // 演出の進行方向
    const float2 direction = CustomParameters[3].zw;
    // COLOR0はpremultiply済みなので、元の色はCustomParameters[5]から読みます。
    const float4 coverColor = CustomParameters[5];
    // Spriteの描画寸法
    const float2 size = max(CustomParameters[6].zw, float2(1.0f, 1.0f));

    // UVをSprite内の画素位置へ変換
    const float2 pixel = uv * size;
    // 画素位置の変化量を分岐前に測ります。
    const float2 pixelStep = fwidth(pixel);
    // アンチエイリアスに使う画素幅
    const float aa = max(max(pixelStep.x, pixelStep.y), 0.0001f);
    // Shader演出用Rule画像の色
    const float3 rule = SpriteTexture.Sample(SpriteSampler, uv).rgb;

    // Spriteの短辺寸法
    const float shortSide = min(size.x, size.y);
    // 差し色帯を有効にするか
    const bool hasAccent = accent.a > 0.0f && accentRatio > 0.0f;
    // 差し色帯の画素幅
    const float accentWidth = hasAccent ? accentRatio * shortSide : 0.0f;
    // 境界のぼかし幅
    const float softness = softnessRatio * shortSide;
    // 演出の中心画素
    const float2 center = focus * size;
    // 進行方向へ投影した画面範囲
    const float2 range = TransitionProjectedRange(size, direction);
    // 始点から現在画素までの距離
    const float along = dot(pixel, direction) - range.x;
    // 進行方向の画面幅
    const float span = max(range.y - range.x, 1.0f);

    // x = 差し色の層、y = 覆いの色の層の塗りの割合です。
    float2 layers = float2(0.0f, 0.0f);
    // None演出では覆いを描きません。
    if (effect == 0)
    {
        layers = float2(0.0f, 0.0f);
    }
    // Wipeは画面端から覆います。
    else if (effect == 2)
    {
        layers = TransitionEdgeLayers(
            along, span, coverage, accentWidth, softness, aa);
    }
    // Shutterは画面の両端から中央へ閉じます。
    else if (effect == 9)
    {
        // 中央で重なる瞬間に隙間を作らない半幅
        const float halfSpan = span * 0.5f;
        layers = saturate(
            TransitionEdgeLayers(
                along, halfSpan, coverage, accentWidth, softness, aa)
            + TransitionEdgeLayers(
                span - along, halfSpan, coverage, accentWidth, softness, aa));
    }
    // IrisとDiamondは遠い角から中心へ閉じます。
    else if (effect == 3 || effect == 4)
    {
        // 中心から画素までの軸別距離
        const float2 delta = abs(pixel - center);
        // 中心から最遠角までの軸別距離
        const float2 extent = max(center, size - center);
        // 円または辺に垂直なひし形距離
        const float distance = effect == 4
            ? (delta.x + delta.y) * TransitionInverseSqrt2
            : length(delta);
        // 演出中心から最遠角までの距離
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
    // Blindsは分割帯を順に伸ばします。
    else if (effect == 5)
    {
        // 1本あたりの帯幅
        const float stripe = span / divisions;
        // 現在画素が属する帯番号
        const float index = floor(along / stripe);
        // 差し色と覆い色の進捗
        const float2 progress = TransitionLayered(coverage, hasAccent);
        [unroll]
        // 境界を滑らかにするため隣接帯を含めて描きます。
        for (int neighbor = -1; neighbor <= 1; ++neighbor)
        {
            // 現在調べている帯番号
            const float stripeIndex = index + (float)neighbor;
            // 画面外の帯は処理しません。
            if (stripeIndex < 0.0f || stripeIndex > divisions - 1.0f)
            {
                continue;
            }
            // 帯ごとの出現順序
            const float order = divisions > 1.0f
                ? stripeIndex / (divisions - 1.0f)
                : 0.0f;
            // 帯の開始位置
            const float start = stripeIndex * stripe;
            // 差し色の帯端
            const float accentEnd = start
                + stripe * TransitionStaggered(progress.x, order, stagger);
            // 覆い色の帯端
            const float mainEnd = start
                + stripe * TransitionStaggered(progress.y, order, stagger);
            layers += float2(
                hasAccent ? TransitionInterval(start, accentEnd, along, aa) : 0.0f,
                TransitionInterval(start, mainEnd, along, aa));
        }
        layers = saturate(layers);
    }
    // Tiles系は各セルの図形を進行方向へ広げます。
    else if (effect == 6 || effect == 7 || effect == 8)
    {
        // セルの横幅
        const float cell = size.x / divisions;
        // 現在画素が属するセル
        const float2 ownCell = floor(pixel / cell);
        // 差し色と覆い色の進捗
        const float2 progress = TransitionLayered(coverage, hasAccent);
        // 四角形セルの塗り率合計
        float2 squareCoverage = float2(0.0f, 0.0f);
        // ひし形／円の最大内側距離
        float2 distance = float2(-TransitionFar, -TransitionFar);
        [unroll]
        // 図形がはみ出す隣接セルまで調べます。
        for (int y = -1; y <= 1; ++y)
        {
            [unroll]
            // 現在行の隣接セルを調べます。
            for (int x = -1; x <= 1; ++x)
            {
                // 隣接セルの中心画素
                const float2 tileCenter =
                    (ownCell + float2((float)x, (float)y) + 0.5f) * cell;
                // 中心から現在画素までの差
                const float2 offset = pixel - tileCenter;
                // 各軸の中心距離
                const float2 local = abs(offset);
                // セル中心の出現順
                const float order = saturate(
                    (dot(tileCenter, direction) - range.x) / span);
                // 差し色と覆い色の図形サイズ
                const float2 piece = float2(
                    TransitionStaggered(progress.x, order, stagger),
                    TransitionStaggered(progress.y, order, stagger));
                // 四角形セルは塗り率を合算します。
                if (effect == 6)
                {
                    // 1画素重ねてセル境界の隙間を防ぎます。
                    const float2 halfSize = piece * (cell + 1.0f) * 0.5f;
                    squareCoverage += float2(
                        TransitionInterval(-halfSize.x, halfSize.x, offset.x, aa)
                            * TransitionInterval(-halfSize.x, halfSize.x, offset.y, aa),
                        TransitionInterval(-halfSize.y, halfSize.y, offset.x, aa)
                            * TransitionInterval(-halfSize.y, halfSize.y, offset.y, aa));
                    // 四角形処理済みなので距離計算を飛ばします。
                    continue;
                }
                // 図形境界から画素までの距離
                float2 pieceDistance;
                // DiamondTilesは辺に垂直な距離で測ります。
                if (effect == 7)
                {
                    // 差し色と覆い色のひし形半径
                    const float2 radius = piece * (cell + TransitionInverseSqrt2);
                    pieceDistance =
                        (radius - (local.x + local.y)) * TransitionInverseSqrt2;
                }
                // Dotsは円の半径で距離を測ります。
                else
                {
                    // 差し色と覆い色の円半径
                    const float2 radius =
                        piece * (cell * TransitionInverseSqrt2 + 0.5f);
                    pieceDistance = radius - length(local);
                }
                // 現在セル自身の図形か
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
                // 重なった図形のうち最も内側の距離を残します。
                distance = max(distance, pieceDistance);
            }
        }
        // 四角形セルの塗り率を出力します。
        if (effect == 6)
        {
            layers = saturate(float2(
                hasAccent ? squareCoverage.x : 0.0f,
                squareCoverage.y));
        }
        // DiamondTilesとDotsは境界距離を塗り率へ変換します。
        else
        {
            layers = float2(
                hasAccent ? TransitionEdge(distance.x, aa) : 0.0f,
                TransitionEdge(distance.y, aa));
        }
    }
    // Shader演出は各画素の模様順で覆います。
    else if (effect == 10)
    {
        // 模様内でこの画素が覆われる順番
        float order = saturate(TransitionPatternOrder(
            pattern, pixel, size, center, direction, stagger, divisions, rule));
        // 逆再生時は覆われる順番を反転します。
        if (reverse)
        {
            order = 1.0f - order;
        }
        // 模様境界の最小ぼかし幅
        const float patternSoftness = max(softnessRatio, 0.0001f);
        // 模様の差し色幅
        const float patternAccent = hasAccent ? accentRatio : 0.0f;
        // 差し色前端の模様進捗
        const float front = coverage * (1.0f + patternSoftness + patternAccent)
            - patternAccent;
        layers = float2(
            hasAccent
                ? saturate((front + patternAccent - order) / patternSoftness)
                : 0.0f,
            saturate((front - order) / patternSoftness));
    }
    // 未知の演出番号はFadeとして扱います。
    else
    {
        layers = float2(0.0f, coverage);
    }

    // 完了時は全画面を確実に覆います。
    if (effect != 0 && coverage >= 0.9999f)
    {
        layers = float2(0.0f, 1.0f);
    }

    // 覆い色の合成後アルファ
    const float mainAlpha = layers.y * coverColor.a;
    // 覆い色を考慮した差し色アルファ
    const float accentAlpha = layers.x * accent.a * (1.0f - mainAlpha);
    // 合成後の全体アルファ
    const float alpha = mainAlpha + accentAlpha;
    // 透明画素を描画対象から外します。
    clip(alpha - 0.0001f);
    // 合成アルファで割った最終RGB
    const float3 rgb =
        (coverColor.rgb * mainAlpha + accent.rgb * accentAlpha)
        / max(alpha, 0.0001f);
    return float4(rgb, saturate(alpha));
}
