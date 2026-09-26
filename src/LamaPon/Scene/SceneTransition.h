#pragma once

#include <DirectXMath.h>

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    // 画面を覆う（旧シーンを隠す）／開く（新シーンを見せる）演出の形です。
    // Shader以外は単色の矩形と組み込みの円テクスチャだけで組み立てるため、
    // D3D11／D3D12のどちらでも追加のシェーダーなしで動きます。
    // Shaderは全画面を1枚のSpriteで覆い、画素ごとに覆う順番を
    // ピクセルシェーダーで決めます。
    enum class SceneTransitionEffect : std::uint8_t
    {
        // 覆いを描かずに切り替えます（従来どおり読み込み画面だけ）。
        None,
        // 単色で徐々に覆います。
        Fade,
        // 画面の端から単色が伸びます（斜めも可）。
        Wipe,
        // 円が閉じて／開いて切り替えます。
        Iris,
        // ひし形の穴が閉じて／開いて切り替えます。
        Diamond,
        // ブラインドのように帯が順に伸びます。
        Blinds,
        // 四角いタイルが順に現れます。
        Tiles,
        // 45度回したタイルが順に現れます。
        DiamondTiles,
        // 丸いドットが膨らんで覆います。
        Dots,
        // 両側から扉のように閉じます。
        Shutter,
        // ピクセルシェーダーの模様で覆います（shaderPattern、
        // ruleTexture、shaderを参照）。
        Shader,
        Count
    };

    // Shader演出の組み込み模様です。どれも「画素ごとの覆われる順番
    // （0で最初、1で最後）」を計算し、順番が早い画素から覆います。
    enum class SceneTransitionShaderPattern : std::uint8_t
    {
        // ルール画像の暗い画素から順に覆います（ノベルゲームで定番の
        // 方式）。画像が無いときはDissolveになります。
        RuleImage,
        // ノイズでまだらに溶けるように覆います。
        Dissolve,
        // 時計の針のように12時の位置から回って覆います。
        Clock,
        // 渦を巻きながら中心へ閉じます。
        Spiral,
        // 波紋のように揺らぎながら外側から閉じます。
        Ripple,
        // 六角形のタイルが順に現れます。
        Hexagons,
        // ハート形の穴が閉じます。
        Heart,
        Count
    };

    // Shader演出が既定で使う組み込みシェーダー（assets相対）です。
    // 新規プロジェクトと既存プロジェクトの更新で配布されます。
    inline constexpr std::string_view SceneTransitionBuiltInShader =
        "shaders/LamaPonSceneTransition.hlsl";

    // 覆いが進む向きです。Blinds／Tiles／Dotsでは現れる順番、
    // Shutterでは閉じる軸（左右または上下、斜め）に使います。
    enum class SceneTransitionDirection : std::uint8_t
    {
        LeftToRight,
        RightToLeft,
        TopToBottom,
        BottomToTop,
        TopLeftToBottomRight,
        TopRightToBottomLeft,
        BottomLeftToTopRight,
        BottomRightToTopLeft,
        Count
    };

    enum class SceneTransitionEasing : std::uint8_t
    {
        Linear,
        EaseInQuad,
        EaseOutQuad,
        EaseInOutQuad,
        EaseInCubic,
        EaseOutCubic,
        EaseInOutCubic,
        EaseInOutSine,
        Count
    };

    // シーン遷移1回分の見た目と時間です。時間はtimeScaleの影響を
    // 受けない実時間（秒）で、一時停止中のメニューからでも動きます。
    struct SceneTransitionSettings final
    {
        SceneTransitionEffect effect{
            SceneTransitionEffect::None
        };
        SceneTransitionDirection direction{
            SceneTransitionDirection::LeftToRight
        };
        SceneTransitionEasing easing{
            SceneTransitionEasing::EaseInOutCubic
        };
        // 旧シーンを覆い終えるまでの秒数。
        float coverDuration{ 0.4f };
        // 覆い終えてから開き始めるまでの最短秒数。読み込みが
        // 速くても、ここで指定した時間は覆ったままにします。
        float holdDuration{ 0.1f };
        // 新シーンを見せ終えるまでの秒数。
        float revealDuration{ 0.4f };
        // 覆いの色。alphaを1未満にすると旧シーンが透けます。
        DirectX::XMFLOAT4 color{ 0.0f, 0.0f, 0.0f, 1.0f };
        // 覆いの縁へ入れる差し色です。alphaが0なら使いません。
        // Wipe／Shutterでは先端の帯、Iris／Diamondでは穴の縁、
        // Tiles系では市松模様の片側に使います。
        DirectX::XMFLOAT4 accentColor{ 1.0f, 1.0f, 1.0f, 0.0f };
        // 差し色の帯の太さ（画面の短辺に対する比率）。
        float accentWidth{ 0.03f };
        // Wipe／Shutterの境界をぼかす幅（画面の短辺に対する比率）。
        float softness{ 0.0f };
        // Blindsの帯の本数、Tiles系の横方向の個数。
        std::uint32_t divisions{ 10 };
        // Blinds／Tiles系の時間差。0で一斉、1に近いほど1枚ずつ
        // 順番に現れます。
        float stagger{ 0.5f };
        // Iris／Diamondの中心（画面に対する比率。左上が0,0）。
        // プレイヤーの画面位置を入れると、そこへ向かって閉じます。
        DirectX::XMFLOAT2 focus{ 0.5f, 0.5f };
        // trueなら、開くときも覆ったときと同じ向きへ抜けます
        // （覆いが画面を通り過ぎる見た目）。falseなら巻き戻すように
        // 来た方向へ戻ります。
        bool passThrough{ true };
        // 覆っている間に読み込みが続いていれば、読み込み画面を
        // フェードで重ねます。
        bool showLoadingScreen{ true };
        // 遷移中はUI Buttonのクリックを受け付けません
        // （二重に遷移を要求する事故を防ぎます）。
        bool blockInput{ true };
        // 覆いに合わせてMusicバスの音量を下げ、開くときに戻します。
        bool fadeMusic{ true };
        // Shader演出の模様です。
        SceneTransitionShaderPattern shaderPattern{
            SceneTransitionShaderPattern::Dissolve
        };
        // Shader演出のt0へ渡す画像（assets相対）。RuleImageでは
        // 覆う順番を決めるグレースケール画像として使います。
        std::filesystem::path ruleTexture;
        // 独自のピクセルシェーダー（assets相対の.hlsl）。空なら
        // SceneTransitionBuiltInShaderを使います。独自シェーダーも
        // 組み込みと同じCustomParametersの並びを受け取ります。
        std::filesystem::path shader;
    };

    // 非同期読み込み中に重ねる標準の読み込み画面です。遷移演出が
    // 画面を覆っている間に読み込みが続いていれば、その上へ重ねます。
    struct SceneLoadingScreenSettings final
    {
        bool enabled{ true };
        std::string message{ "シーンを読み込んでいます..." };
        DirectX::XMFLOAT4 backgroundColor{
            0.02f, 0.03f, 0.06f, 0.94f
        };
        DirectX::XMFLOAT4 barBackgroundColor{
            0.12f, 0.16f, 0.24f, 1.0f
        };
        DirectX::XMFLOAT4 barFillColor{
            0.12f, 0.48f, 0.9f, 1.0f
        };
        DirectX::XMFLOAT4 textColor{
            0.92f, 0.96f, 1.0f, 1.0f
        };
        bool showPercentage{ true };
        // ここから下は後から追加した項目で、既定値では従来と同じ
        // 見た目になります。
        // メッセージの下に小さく表示する補足（操作のヒントなど）。
        std::string hint;
        // 背景へ画面全体を覆うように表示する画像（assets相対）。
        // 空なら背景色だけを塗ります。
        std::filesystem::path backgroundTexture;
        // 読み込み中を示す回転インジケーターを表示します。
        bool showSpinner{};
        // 進捗バーを実際の進捗へ滑らかに追いつかせます。
        bool smoothProgress{ true };
        // 遷移演出と組み合わせたときの表示・非表示のフェード秒数。
        float fadeDuration{ 0.2f };
    };

    [[nodiscard]] nlohmann::json SceneLoadingScreenToJson(
        const SceneLoadingScreenSettings& settings);
    [[nodiscard]] SceneLoadingScreenSettings SceneLoadingScreenFromJson(
        const nlohmann::json& value,
        const SceneLoadingScreenSettings& fallback = {});

    // 演出と色だけを指定した設定を作ります。覆う時間と開く時間の
    // 両方にdurationSecondsを使い、残りは既定値のままです。
    [[nodiscard]] SceneTransitionSettings MakeSceneTransition(
        SceneTransitionEffect effect,
        float durationSeconds = 0.4f,
        const DirectX::XMFLOAT4& color = { 0.0f, 0.0f, 0.0f, 1.0f });

    // 範囲外の値を安全な範囲へ丸めた設定を返します。
    [[nodiscard]] SceneTransitionSettings SanitizeSceneTransition(
        const SceneTransitionSettings& settings);

    // tは0～1。範囲外はクランプします。
    [[nodiscard]] float EvaluateSceneTransitionEasing(
        SceneTransitionEasing easing,
        float t) noexcept;

    [[nodiscard]] std::string_view SceneTransitionEffectName(
        SceneTransitionEffect effect) noexcept;
    [[nodiscard]] SceneTransitionEffect SceneTransitionEffectFromName(
        std::string_view name) noexcept;
    [[nodiscard]] std::string_view SceneTransitionDirectionName(
        SceneTransitionDirection direction) noexcept;
    [[nodiscard]] SceneTransitionDirection
        SceneTransitionDirectionFromName(
            std::string_view name) noexcept;
    [[nodiscard]] std::string_view SceneTransitionEasingName(
        SceneTransitionEasing easing) noexcept;
    [[nodiscard]] SceneTransitionEasing SceneTransitionEasingFromName(
        std::string_view name) noexcept;
    [[nodiscard]] std::string_view SceneTransitionShaderPatternName(
        SceneTransitionShaderPattern pattern) noexcept;
    [[nodiscard]] SceneTransitionShaderPattern
        SceneTransitionShaderPatternFromName(
            std::string_view name) noexcept;

    // Scene・Project Settingsへ保存する形式です。読み込みでは
    // 無い項目にfallbackの値を使い、未知の名前はfallbackへ戻します。
    [[nodiscard]] nlohmann::json SceneTransitionToJson(
        const SceneTransitionSettings& settings);
    [[nodiscard]] SceneTransitionSettings SceneTransitionFromJson(
        const nlohmann::json& value,
        const SceneTransitionSettings& fallback = {});

    enum class SceneTransitionPhase : std::uint8_t
    {
        Idle,
        // 旧シーンを覆っている途中です。
        Covering,
        // 画面を覆い終え、読み込みの完了と最短保持時間を待っています。
        Covered,
        // 新シーンを見せている途中です。
        Revealing
    };

    // Advanceの1回で起きた出来事です。
    struct SceneTransitionTimelineEvents final
    {
        bool covered{};
        bool revealStarted{};
        bool finished{};
    };

    // 覆う→保持→開くの進行だけを持つ、描画やシーンに依存しない
    // 時間軸です。SceneManagerが毎フレーム実時間で進めます。
    class SceneTransitionTimeline final
    {
    public:
        // 開いている途中から始めた場合は、今の覆い具合から
        // 続けて覆い直します（見た目が飛びません）。
        void Start(const SceneTransitionSettings& settings);
        // readyToRevealは「新シーンの有効化が済み、開いてよい」状態です。
        // 1回の呼び出しで進む段階は1つだけなので、覆い終えたフレームに
        // 呼び出し側がシーンを有効化してから開き始められます。
        SceneTransitionTimelineEvents Advance(
            float deltaSeconds,
            bool readyToReveal) noexcept;
        // 覆っている途中でも、今の覆い具合からすぐ開き始めます
        // （読み込みのキャンセルや失敗時に使います）。
        void Reveal() noexcept;
        // 演出を打ち切って何も覆っていない状態へ戻します。
        void Reset() noexcept;

        [[nodiscard]] SceneTransitionPhase Phase() const noexcept
        {
            return m_phase;
        }
        [[nodiscard]] const SceneTransitionSettings&
            Settings() const noexcept
        {
            return m_settings;
        }
        [[nodiscard]] bool IsActive() const noexcept
        {
            return m_phase != SceneTransitionPhase::Idle;
        }
        [[nodiscard]] bool IsFullyCovered() const noexcept
        {
            return m_phase == SceneTransitionPhase::Covered;
        }
        // 0で何も覆っていない、1で全面を覆っている状態です。
        // イージング適用後の値です。
        [[nodiscard]] float Coverage() const noexcept;
        // 今の段階の進み具合（イージング前、0～1）。
        [[nodiscard]] float PhaseProgress() const noexcept
        {
            return m_progress;
        }

    private:
        SceneTransitionSettings m_settings;
        SceneTransitionPhase m_phase{ SceneTransitionPhase::Idle };
        float m_progress{};
        float m_heldSeconds{};
        std::uint32_t m_readyFrames{};
    };

    enum class SceneTransitionShape : std::uint8_t
    {
        // 単色の矩形。
        Rectangle,
        // 内接円だけを塗るbuiltin/circle。
        Circle,
        // 内接円の外側だけを塗るbuiltin/iris。
        InverseCircle
    };

    // builtin/circleとbuiltin/irisの円の半径は、テクスチャの一辺に
    // 対してこの比率です（256pxのうち半径112px）。
    inline constexpr float SceneTransitionCircleRadiusRatio =
        112.0f / 256.0f;

    // 描画する1枚分です。x,y,width,heightは回転前の矩形（ピクセル）で、
    // rotationは矩形の中心を軸にした回転（ラジアン）です。
    // colorはpremultiplyしていない色です。
    struct SceneTransitionQuad final
    {
        float x{};
        float y{};
        float width{};
        float height{};
        float rotation{};
        SceneTransitionShape shape{ SceneTransitionShape::Rectangle };
        DirectX::XMFLOAT4 color{};
    };

    // Shader演出のSprite passへ渡すCustomParametersです。
    // シェーダー側は[0]～[4]だけを読みます（[5]～[7]はSprite描画時に
    // エンジンが上書きするため使いません）。
    //   [0] = coverage, 境界のぼかし幅, 差し色の幅, 模様の番号
    //   [1] = 開くときに順番を反転するか(0/1), stagger, divisions, 0
    //   [2] = 差し色（premultiplyしない色）
    //   [3] = 画面の幅, 高さ, 中心x, 中心y（ピクセル）
    //   [4] = 向きx, 向きy, 0, 0
    // 覆いの色はSpriteのtint（COLOR0）で渡します。
    struct SceneTransitionShaderFrame final
    {
        std::array<DirectX::XMFLOAT4, 8> parameters{};
        DirectX::XMFLOAT4 tint{};
        // RuleImageで画像が無い場合はDissolveへ置き換えた後の模様です。
        SceneTransitionShaderPattern pattern{
            SceneTransitionShaderPattern::Dissolve
        };
    };

    [[nodiscard]] SceneTransitionShaderFrame
        BuildSceneTransitionShaderFrame(
            const SceneTransitionSettings& settings,
            float coverage,
            bool revealing,
            float width,
            float height,
            bool hasRuleTexture);

    // coverageはCoverage()の値、revealingは開く途中かどうかです。
    // quadsは消去してから描く順に詰めます。Shader演出は画素単位の
    // 模様をGPUで描くため、ここではシェーダーを使えないときの
    // 代わりとしてFadeと同じ矩形を返します。
    void BuildSceneTransitionQuads(
        const SceneTransitionSettings& settings,
        float coverage,
        bool revealing,
        float width,
        float height,
        std::vector<SceneTransitionQuad>& quads);
}
