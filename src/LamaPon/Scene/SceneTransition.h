#pragma once

#include <DirectXMath.h>

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace LamaPon
{
    // 覆う・保持・開く順に進み、描画側はSceneManagerのCoverageを使って覆いを描きます。
    enum class SceneTransitionEasing : std::uint8_t
    {
        // 一定速度の変化
        Linear,
        // 二次曲線による加速
        EaseInQuad,
        // 二次曲線による減速
        EaseOutQuad,
        // 二次曲線による加減速
        EaseInOutQuad,
        // 三次曲線による加速
        EaseInCubic,
        // 三次曲線による減速
        EaseOutCubic,
        // 三次曲線による加減速
        EaseInOutCubic,
        // 正弦曲線による加減速
        EaseInOutSine,
        // イージング種別の総数
        Count
    };

    // 時間はtimeScaleの影響を受けない秒数で、既定値では読み込み画面のみ表示して即時に切り替えます。
    struct SceneTransitionSettings final
    {
        // 覆い具合の変化曲線
        SceneTransitionEasing easing{
            SceneTransitionEasing::EaseInOutCubic
        };
        // 旧シーンを覆う秒数
        float coverDuration{};
        // 全面を覆う最短保持秒数
        float holdDuration{};
        // 新シーンを開く秒数
        float revealDuration{};
        // 読込中に標準画面を重ねる指定
        bool showLoadingScreen{ true };
        // 遷移中のUIクリック禁止
        bool blockInput{ true };
        // 覆いに連動する音楽減衰指定
        bool fadeMusic{ true };
    };

    // 遷移で覆っている間に読み込みが続く場合に重ねる標準画面の設定です。
    struct SceneLoadingScreenSettings final
    {
        // 読み込み画面の表示指定
        bool enabled{ true };
        // 読み込み画面の主メッセージ
        std::string message{ "シーンを読み込んでいます..." };
        // 画面背景のRGBA色
        DirectX::XMFLOAT4 backgroundColor{
            0.02f, 0.03f, 0.06f, 0.94f
        };
        // 進捗バー背景のRGBA色
        DirectX::XMFLOAT4 barBackgroundColor{
            0.12f, 0.16f, 0.24f, 1.0f
        };
        // 進捗バー進捗部分のRGBA色
        DirectX::XMFLOAT4 barFillColor{
            0.12f, 0.48f, 0.9f, 1.0f
        };
        // 文字のRGBA色
        DirectX::XMFLOAT4 textColor{
            0.92f, 0.96f, 1.0f, 1.0f
        };
        // 進捗率の数値表示指定
        bool showPercentage{ true };
        // 主メッセージ下の補足文
        std::string hint;
        // 背景画像のassets相対パス
        std::filesystem::path backgroundTexture;
        // 回転インジケーター表示指定
        bool showSpinner{};
        // 表示進捗を滑らかに追従する指定
        bool smoothProgress{ true };
        // 標準画面をフェードする秒数
        float fadeDuration{ 0.2f };
    };

    // 読み込み画面の設定をJSONへ変換します(settings: 保存する設定)。
    [[nodiscard]] nlohmann::json SceneLoadingScreenToJson(
        const SceneLoadingScreenSettings& settings);
    // 既知の設定を読み込み色とフェード時間を制限します(value: 設定JSON, fallback: 欠落や型違いの代替値)。
    // オブジェクト以外のJSONではfallbackをそのまま返します。
    [[nodiscard]] SceneLoadingScreenSettings SceneLoadingScreenFromJson(
        const nlohmann::json& value,
        const SceneLoadingScreenSettings& fallback = {});

    // 同じ秒数で覆いと開きを行う設定を作ります(durationSeconds: 覆いと開きの秒数, holdSeconds: 最短保持秒数, easing: 変化曲線)。
    [[nodiscard]] SceneTransitionSettings MakeSceneTransition(
        float durationSeconds,
        float holdSeconds = 0.1f,
        SceneTransitionEasing easing =
            SceneTransitionEasing::EaseInOutCubic);

    // 三つの時間がいずれも正数でない場合にtrueを返します(settings: 判定する設定)。
    [[nodiscard]] bool IsInstantSceneTransition(
        const SceneTransitionSettings& settings) noexcept;

    // 時間を0〜30秒に制限し非有限値と不正な曲線を既定値へ戻します(settings: 補正する設定)。
    [[nodiscard]] SceneTransitionSettings SanitizeSceneTransition(
        const SceneTransitionSettings& settings);

    // 曲線を適用した進捗を返します(easing: 変化曲線, t: 0〜1に制限し非有限値は0とする進捗)。
    [[nodiscard]] float EvaluateSceneTransitionEasing(
        SceneTransitionEasing easing,
        float t) noexcept;

    // 曲線の保存名を返し、不明な値ならlinearを返します(easing: 変化曲線)。
    [[nodiscard]] std::string_view SceneTransitionEasingName(
        SceneTransitionEasing easing) noexcept;
    // 保存名に対応する曲線を返し、未知の名前ならEaseInOutCubicを返します(name: 曲線の保存名)。
    [[nodiscard]] SceneTransitionEasing SceneTransitionEasingFromName(
        std::string_view name) noexcept;

    // 補正した遷移設定をJSONへ変換します(settings: 保存する設定)。
    [[nodiscard]] nlohmann::json SceneTransitionToJson(
        const SceneTransitionSettings& settings);
    // 既知の遷移設定を読み込み補正します(value: 設定JSON, fallback: 欠落・型違い・未知の曲線名の代替値)。
    [[nodiscard]] SceneTransitionSettings SceneTransitionFromJson(
        const nlohmann::json& value,
        const SceneTransitionSettings& fallback = {});

    enum class SceneTransitionPhase : std::uint8_t
    {
        // 覆いのない待機状態
        Idle,
        // 旧シーンを覆う段階
        Covering,
        // 読み込みと最短保持時間の待機
        Covered,
        // 新シーンを開く段階
        Revealing
    };

    // Advanceの1回で起きた出来事です。
    struct SceneTransitionTimelineEvents final
    {
        // 全面を覆い終えた通知
        bool covered{};
        // 新シーンを開き始めた通知
        bool revealStarted{};
        // 覆いを解除し終えた通知
        bool finished{};
    };

    // SceneManagerが実時間で進める、描画やシーンから独立した遷移の時間軸です。
    class SceneTransitionTimeline final
    {
    public:
        // 現在の覆い具合を維持して遷移を開始します(settings: 時間と変化曲線の設定)。
        void Start(const SceneTransitionSettings& settings);
        // 一段階まで時間軸を進め発生したイベントを返します(deltaSeconds: 非有限・負値を0とする実時間秒数, readyToReveal: 新シーンを開ける状態)。
        // 保持時間に加えて、即時遷移は準備完了1回、時間のある遷移は連続2回の呼び出しを待ちます。
        SceneTransitionTimelineEvents Advance(
            float deltaSeconds,
            bool readyToReveal) noexcept;
        // 現在の覆い具合を維持して即座に開き始めます。
        void Reveal() noexcept;
        // 遷移を打ち切り、覆いのない待機状態へ戻します。
        void Reset() noexcept;

        // 現在の遷移段階を返します。
        [[nodiscard]] SceneTransitionPhase Phase() const noexcept
        {
            return m_phase;
        }
        // 適用中の補正済み遷移設定への参照を返します。
        [[nodiscard]] const SceneTransitionSettings&
            Settings() const noexcept
        {
            return m_settings;
        }
        // 待機状態以外の遷移段階か返します。
        [[nodiscard]] bool IsActive() const noexcept
        {
            return m_phase != SceneTransitionPhase::Idle;
        }
        // 覆い終えて保持している段階か返します。
        [[nodiscard]] bool IsFullyCovered() const noexcept
        {
            return m_phase == SceneTransitionPhase::Covered;
        }
        // 曲線を適用した覆い具合を返し、0は覆いなし、1は全面の覆いを表します。
        [[nodiscard]] float Coverage() const noexcept;
        // 現在の段階の曲線適用前の進捗率を返します。
        [[nodiscard]] float PhaseProgress() const noexcept
        {
            return m_progress;
        }

    private:
        // 適用中の遷移設定
        SceneTransitionSettings m_settings;
        // 現在の遷移段階
        SceneTransitionPhase m_phase{ SceneTransitionPhase::Idle };
        // 現在の段階の進捗率
        float m_progress{};
        // 全面を覆ってからの経過秒数
        float m_heldSeconds{};
        // 連続して準備完了した呼び出し数
        std::uint32_t m_readyFrames{};
    };
}
