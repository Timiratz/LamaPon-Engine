#pragma once

#include <DirectXMath.h>

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace LamaPon
{
    // シーン遷移は「旧シーンを覆う→覆ったまま新シーンへ切り替える→
    // 開く」の順に進みます。エンジンは覆う絵を描かず、覆い具合
    // （Coverage、0～1）だけを計算します。Coverageは
    // SceneManager::TransitionCoverageで読めるので、ScriptやSpriteで
    // 自分の演出を描きます（例はscene-transition-showcaseパッケージ）。
    //
    // Coverageの変化のしかた（イージング）です。
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

    // シーン遷移1回分の時間と動作です。時間はtimeScaleの影響を
    // 受けない実時間（秒）で、一時停止中のメニューからでも動きます。
    // 既定値は3つの時間がすべて0の「すぐ切り替える遷移」で、従来どおり
    // 読み込み中だけ読み込み画面を表示します。
    struct SceneTransitionSettings final
    {
        // Coverageの変化のしかたです。
        SceneTransitionEasing easing{
            SceneTransitionEasing::EaseInOutCubic
        };
        // 旧シーンを覆い終えるまでの秒数。
        float coverDuration{};
        // 覆い終えてから開き始めるまでの最短秒数。読み込みが
        // 速くても、ここで指定した時間は覆ったままにします。
        float holdDuration{};
        // 新シーンを見せ終えるまでの秒数。
        float revealDuration{};
        // 覆っている間に読み込みが続いていれば、読み込み画面を
        // フェードで重ねます。
        bool showLoadingScreen{ true };
        // 遷移中はUI Buttonのクリックを受け付けません
        // （二重に遷移を要求する事故を防ぎます）。
        bool blockInput{ true };
        // 覆い具合に合わせてMusicバスの音量を下げ、開くときに戻します。
        bool fadeMusic{ true };
    };

    // 非同期読み込み中に重ねる標準の読み込み画面です。遷移で
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
        // 時間のある遷移と組み合わせたときの表示・非表示のフェード秒数。
        float fadeDuration{ 0.2f };
    };

    [[nodiscard]] nlohmann::json SceneLoadingScreenToJson(
        const SceneLoadingScreenSettings& settings);
    [[nodiscard]] SceneLoadingScreenSettings SceneLoadingScreenFromJson(
        const nlohmann::json& value,
        const SceneLoadingScreenSettings& fallback = {});

    // 覆う時間と開く時間の両方にdurationSecondsを使う設定を作ります。
    // 残りの項目は既定値のままです。
    [[nodiscard]] SceneTransitionSettings MakeSceneTransition(
        float durationSeconds,
        float holdSeconds = 0.1f,
        SceneTransitionEasing easing =
            SceneTransitionEasing::EaseInOutCubic);

    // 3つの時間がすべて0（覆わずにすぐ切り替える）ならtrueです。
    [[nodiscard]] bool IsInstantSceneTransition(
        const SceneTransitionSettings& settings) noexcept;

    // 範囲外の値を安全な範囲へ丸めた設定を返します。
    [[nodiscard]] SceneTransitionSettings SanitizeSceneTransition(
        const SceneTransitionSettings& settings);

    // tは0～1。範囲外はクランプします。
    [[nodiscard]] float EvaluateSceneTransitionEasing(
        SceneTransitionEasing easing,
        float t) noexcept;

    [[nodiscard]] std::string_view SceneTransitionEasingName(
        SceneTransitionEasing easing) noexcept;
    [[nodiscard]] SceneTransitionEasing SceneTransitionEasingFromName(
        std::string_view name) noexcept;

    // データアセットなどへ保存する形式です。読み込みでは無い項目に
    // fallbackの値を使い、未知の名前はfallbackへ戻します。知らない
    // キー（演出の見た目など）は無視するため、自分の演出の設定と
    // 同じJSONへまとめて保存できます。
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
}
