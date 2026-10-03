#include "LamaPon/Scene/SceneTransition.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace
{
    using LamaPon::SceneTransitionEasing;
    using LamaPon::SceneTransitionSettings;

    // 円周率
    constexpr float Pi = 3.14159265358979f;

    template <typename Enum>
    struct NamedValue final
    {
        // 名前に対応する列挙値
        Enum value;
        // 保存に使う列挙値の名前
        // 読み込んだ曲線の保存名
        std::string_view name;
    };

    // 曲線と保存名の対応表
    constexpr std::array EasingNames{
        NamedValue<SceneTransitionEasing>{
            SceneTransitionEasing::Linear, "linear" },
        NamedValue<SceneTransitionEasing>{
            SceneTransitionEasing::EaseInQuad, "easeInQuad" },
        NamedValue<SceneTransitionEasing>{
            SceneTransitionEasing::EaseOutQuad, "easeOutQuad" },
        NamedValue<SceneTransitionEasing>{
            SceneTransitionEasing::EaseInOutQuad, "easeInOutQuad" },
        NamedValue<SceneTransitionEasing>{
            SceneTransitionEasing::EaseInCubic, "easeInCubic" },
        NamedValue<SceneTransitionEasing>{
            SceneTransitionEasing::EaseOutCubic, "easeOutCubic" },
        NamedValue<SceneTransitionEasing>{
            SceneTransitionEasing::EaseInOutCubic, "easeInOutCubic" },
        NamedValue<SceneTransitionEasing>{
            SceneTransitionEasing::EaseInOutSine, "easeInOutSine" },
    };
    static_assert(
        EasingNames.size()
        == static_cast<std::size_t>(SceneTransitionEasing::Count));

    // パスをgeneric形式のUTF-8文字列へ変換します(path: 変換するパス)。
    [[nodiscard]] std::string PathToUtf8String(
        const std::filesystem::path& path)
    {
        // generic形式のUTF-8パス
        const std::u8string utf8 = path.generic_u8string();
        return {
            reinterpret_cast<const char*>(utf8.data()),
            utf8.size()
        };
    }

    // UTF-8文字列からパスを復元します(value: UTF-8で表すパス)。
    [[nodiscard]] std::filesystem::path PathFromUtf8String(
        const std::string& value)
    {
        return std::filesystem::path(
            std::u8string(
                reinterpret_cast<const char8_t*>(value.data()),
                reinterpret_cast<const char8_t*>(
                    value.data() + value.size())));
    }

    // 列挙値の名前を返し未知の値なら先頭の名前を返します(names: 空でない対応表, value: 検索する列挙値)。
    template <typename Enum, std::size_t Size>
    [[nodiscard]] std::string_view NameOf(
        const std::array<NamedValue<Enum>, Size>& names,
        const Enum value) noexcept
    {
        // 照合する列挙値と名前
        for (const auto& entry : names)
        {
            if (entry.value == value)
            {
                return entry.name;
            }
        }
        return names.front().name;
    }

    // 名前に対応する列挙値を返します(names: 名前の対応表, name: 検索する名前, fallback: 未発見時の値)。
    template <typename Enum, std::size_t Size>
    [[nodiscard]] Enum ValueOf(
        const std::array<NamedValue<Enum>, Size>& names,
        const std::string_view name,
        const Enum fallback) noexcept
    {
        // 照合する列挙値と名前
        for (const auto& entry : names)
        {
            if (entry.name == name)
            {
                return entry.value;
            }
        }
        return fallback;
    }

    // 有限な値ならそのまま返します(value: 検査する値, fallback: 非有限値の代替値)。
    [[nodiscard]] float FiniteOr(
        const float value,
        const float fallback) noexcept
    {
        return std::isfinite(value) ? value : fallback;
    }

    // 0〜1へ制限し非有限値なら0を返します(value: 制限する値)。
    [[nodiscard]] float Saturate(const float value) noexcept
    {
        return std::clamp(FiniteOr(value, 0.0f), 0.0f, 1.0f);
    }

    // RGBAの各成分を0〜1へ制限し非有限成分は0へ戻します(color: 補正する色)。
    [[nodiscard]] DirectX::XMFLOAT4 SaturateColor(
        const DirectX::XMFLOAT4& color) noexcept
    {
        return {
            Saturate(color.x),
            Saturate(color.y),
            Saturate(color.z),
            Saturate(color.w)
        };
    }

    // 単調な曲線を24回の二分探索で逆算します(easing: 変化曲線, value: 逆算する覆い具合)。
    [[nodiscard]] float InverseEasing(
        const SceneTransitionEasing easing,
        const float value) noexcept
    {
        // 0〜1に制限した逆算目標
        const float target = Saturate(value);
        // 二分探索区間の下端
        float low = 0.0f;
        // 二分探索区間の上端
        float high = 1.0f;
        // 二分探索の反復回数
        for (int iteration{}; iteration < 24; ++iteration)
        {
            // 二分探索区間の中央
            const float middle = (low + high) * 0.5f;
            if (LamaPon::EvaluateSceneTransitionEasing(
                    easing,
                    middle)
                < target)
            {
                low = middle;
            }
            else
            {
                high = middle;
            }
        }
        return (low + high) * 0.5f;
    }

    // 有限な数値を取得します(value: 設定JSON, key: 項目名, fallback: 欠落・型違い・非有限値の代替値)。
    [[nodiscard]] float ReadNumber(
        const nlohmann::json& value,
        const char* key,
        const float fallback)
    {
        // 読み込むJSON項目の位置
        const auto found = value.find(key);
        return found != value.end() && found->is_number()
            ? FiniteOr(found->get<float>(), fallback)
            : fallback;
    }

    // 真偽値を取得します(value: 設定JSON, key: 項目名, fallback: 欠落や型違いの代替値)。
    [[nodiscard]] bool ReadBoolean(
        const nlohmann::json& value,
        const char* key,
        const bool fallback)
    {
        // 読み込むJSON項目の位置
        const auto found = value.find(key);
        return found != value.end() && found->is_boolean()
            ? found->get<bool>()
            : fallback;
    }

    // 四成分の色を取得します(value: 設定JSON, key: 項目名, fallback: 不正な配列や非有限成分の代替値)。
    [[nodiscard]] DirectX::XMFLOAT4 ReadColor(
        const nlohmann::json& value,
        const char* key,
        const DirectX::XMFLOAT4& fallback)
    {
        // 読み込むJSON項目の位置
        const auto found = value.find(key);
        // 各色成分が数値であるか調べます(item: 配列の色成分)。
        if (found == value.end()
            || !found->is_array()
            || found->size() != 4
            || !std::all_of(
                found->begin(),
                found->end(),
                [](const nlohmann::json& item)
                {
                    return item.is_number();
                }))
        {
            return fallback;
        }
        return {
            FiniteOr((*found)[0].get<float>(), fallback.x),
            FiniteOr((*found)[1].get<float>(), fallback.y),
            FiniteOr((*found)[2].get<float>(), fallback.z),
            FiniteOr((*found)[3].get<float>(), fallback.w)
        };
    }

    // 文字列を取得し欠落や型違いなら空を返します(value: 設定JSON, key: 項目名)。
    [[nodiscard]] std::string ReadString(
        const nlohmann::json& value,
        const char* key)
    {
        // 読み込むJSON項目の位置
        const auto found = value.find(key);
        return found != value.end() && found->is_string()
            ? found->get<std::string>()
            : std::string{};
    }
}

namespace LamaPon
{
    SceneTransitionSettings MakeSceneTransition(
        const float durationSeconds,
        const float holdSeconds,
        const SceneTransitionEasing easing)
    {
        // 作成または読み込む設定
        SceneTransitionSettings settings;
        settings.easing = easing;
        settings.coverDuration = durationSeconds;
        settings.holdDuration = holdSeconds;
        settings.revealDuration = durationSeconds;
        return SanitizeSceneTransition(settings);
    }

    bool IsInstantSceneTransition(
        const SceneTransitionSettings& settings) noexcept
    {
        // NaNや負の値もSanitizeでは0になるため、0と同じに扱います。
        return !(settings.coverDuration > 0.0f)
            && !(settings.holdDuration > 0.0f)
            && !(settings.revealDuration > 0.0f);
    }

    SceneTransitionSettings SanitizeSceneTransition(
        const SceneTransitionSettings& settings)
    {
        // 補正に使う既定設定
        const SceneTransitionSettings defaults;
        // 補正後の遷移設定
        SceneTransitionSettings result = settings;
        if (result.easing >= SceneTransitionEasing::Count)
        {
            result.easing = defaults.easing;
        }
        // 遷移の一段階の上限秒数
        constexpr float MaximumSeconds = 30.0f;
        result.coverDuration = std::clamp(
            FiniteOr(result.coverDuration, defaults.coverDuration),
            0.0f,
            MaximumSeconds);
        result.holdDuration = std::clamp(
            FiniteOr(result.holdDuration, defaults.holdDuration),
            0.0f,
            MaximumSeconds);
        result.revealDuration = std::clamp(
            FiniteOr(result.revealDuration, defaults.revealDuration),
            0.0f,
            MaximumSeconds);
        return result;
    }

    float EvaluateSceneTransitionEasing(
        const SceneTransitionEasing easing,
        const float t) noexcept
    {
        // 0〜1に制限した進捗率
        const float x = Saturate(t);
        switch (easing)
        {
        case SceneTransitionEasing::EaseInQuad:
            return x * x;
        case SceneTransitionEasing::EaseOutQuad:
            return 1.0f - (1.0f - x) * (1.0f - x);
        case SceneTransitionEasing::EaseInOutQuad:
            return x < 0.5f
                ? 2.0f * x * x
                : 1.0f - std::pow(-2.0f * x + 2.0f, 2.0f) * 0.5f;
        case SceneTransitionEasing::EaseInCubic:
            return x * x * x;
        case SceneTransitionEasing::EaseOutCubic:
            return 1.0f - std::pow(1.0f - x, 3.0f);
        case SceneTransitionEasing::EaseInOutCubic:
            return x < 0.5f
                ? 4.0f * x * x * x
                : 1.0f - std::pow(-2.0f * x + 2.0f, 3.0f) * 0.5f;
        case SceneTransitionEasing::EaseInOutSine:
            return -(std::cos(Pi * x) - 1.0f) * 0.5f;
        case SceneTransitionEasing::Linear:
        default:
            return x;
        }
    }

    std::string_view SceneTransitionEasingName(
        const SceneTransitionEasing easing) noexcept
    {
        return NameOf(EasingNames, easing);
    }

    SceneTransitionEasing SceneTransitionEasingFromName(
        const std::string_view name) noexcept
    {
        return ValueOf(
            EasingNames,
            name,
            SceneTransitionEasing::EaseInOutCubic);
    }

    // 補正した設定をJSONへ変換します(source: 保存する遷移設定)。
    nlohmann::json SceneTransitionToJson(
        const SceneTransitionSettings& source)
    {
        // 作成または読み込む設定
        const auto settings = SanitizeSceneTransition(source);
        return nlohmann::json{
            {
                "easing",
                std::string(SceneTransitionEasingName(settings.easing))
            },
            { "coverDuration", settings.coverDuration },
            { "holdDuration", settings.holdDuration },
            { "revealDuration", settings.revealDuration },
            { "showLoadingScreen", settings.showLoadingScreen },
            { "blockInput", settings.blockInput },
            { "fadeMusic", settings.fadeMusic },
        };
    }

    SceneTransitionSettings SceneTransitionFromJson(
        const nlohmann::json& value,
        const SceneTransitionSettings& fallback)
    {
        if (!value.is_object())
        {
            return SanitizeSceneTransition(fallback);
        }
        // 作成または読み込む設定
        SceneTransitionSettings settings = fallback;
        // 読み込んだ曲線の保存名
        if (const auto name = ReadString(value, "easing");
            !name.empty())
        {
            settings.easing = ValueOf(
                EasingNames,
                name,
                fallback.easing);
        }
        settings.coverDuration = ReadNumber(
            value, "coverDuration", fallback.coverDuration);
        settings.holdDuration = ReadNumber(
            value, "holdDuration", fallback.holdDuration);
        settings.revealDuration = ReadNumber(
            value, "revealDuration", fallback.revealDuration);
        settings.showLoadingScreen = ReadBoolean(
            value, "showLoadingScreen", fallback.showLoadingScreen);
        settings.blockInput = ReadBoolean(
            value, "blockInput", fallback.blockInput);
        settings.fadeMusic = ReadBoolean(
            value, "fadeMusic", fallback.fadeMusic);
        return SanitizeSceneTransition(settings);
    }

    nlohmann::json SceneLoadingScreenToJson(
        const SceneLoadingScreenSettings& settings)
    {
        // RGBAを四成分のJSON配列へ変換します(value: 保存する色)。
        const auto color = [](const DirectX::XMFLOAT4& value)
        {
            return nlohmann::json::array(
                { value.x, value.y, value.z, value.w });
        };
        return nlohmann::json{
            { "enabled", settings.enabled },
            { "message", settings.message },
            { "backgroundColor", color(settings.backgroundColor) },
            { "barBackgroundColor", color(settings.barBackgroundColor) },
            { "barFillColor", color(settings.barFillColor) },
            { "textColor", color(settings.textColor) },
            { "showPercentage", settings.showPercentage },
            { "hint", settings.hint },
            {
                "backgroundTexture",
                PathToUtf8String(settings.backgroundTexture)
            },
            { "showSpinner", settings.showSpinner },
            { "smoothProgress", settings.smoothProgress },
            {
                "fadeDuration",
                std::clamp(
                    FiniteOr(settings.fadeDuration, 0.2f),
                    0.0f,
                    5.0f)
            },
        };
    }

    SceneLoadingScreenSettings SceneLoadingScreenFromJson(
        const nlohmann::json& value,
        const SceneLoadingScreenSettings& fallback)
    {
        if (!value.is_object())
        {
            return fallback;
        }
        // 作成または読み込む設定
        SceneLoadingScreenSettings settings = fallback;
        settings.enabled =
            ReadBoolean(value, "enabled", fallback.enabled);
        // 読み込むJSON項目の位置
        if (const auto found = value.find("message");
            found != value.end() && found->is_string())
        {
            settings.message = found->get<std::string>();
        }
        settings.backgroundColor = SaturateColor(ReadColor(
            value, "backgroundColor", fallback.backgroundColor));
        settings.barBackgroundColor = SaturateColor(ReadColor(
            value, "barBackgroundColor", fallback.barBackgroundColor));
        settings.barFillColor = SaturateColor(ReadColor(
            value, "barFillColor", fallback.barFillColor));
        settings.textColor = SaturateColor(ReadColor(
            value, "textColor", fallback.textColor));
        settings.showPercentage = ReadBoolean(
            value, "showPercentage", fallback.showPercentage);
        // 読み込むJSON項目の位置
        if (const auto found = value.find("hint");
            found != value.end() && found->is_string())
        {
            settings.hint = found->get<std::string>();
        }
        // 読み込むJSON項目の位置
        if (const auto found = value.find("backgroundTexture");
            found != value.end() && found->is_string())
        {
            settings.backgroundTexture =
                PathFromUtf8String(found->get<std::string>());
        }
        settings.showSpinner = ReadBoolean(
            value, "showSpinner", fallback.showSpinner);
        settings.smoothProgress = ReadBoolean(
            value, "smoothProgress", fallback.smoothProgress);
        settings.fadeDuration = std::clamp(
            ReadNumber(value, "fadeDuration", fallback.fadeDuration),
            0.0f,
            5.0f);
        return settings;
    }

    void SceneTransitionTimeline::Start(
        const SceneTransitionSettings& settings)
    {
        // 切り替える直前の覆い具合
        const float currentCoverage = Coverage();
        // 再開する前の遷移段階
        const auto previousPhase = m_phase;
        m_settings = SanitizeSceneTransition(settings);
        m_heldSeconds = 0.0f;
        m_readyFrames = 0;
        if (previousPhase == SceneTransitionPhase::Covered)
        {
            m_progress = 1.0f;
            return;
        }
        m_phase = SceneTransitionPhase::Covering;
        m_progress = previousPhase == SceneTransitionPhase::Idle
            ? 0.0f
            : InverseEasing(m_settings.easing, currentCoverage);
    }

    SceneTransitionTimelineEvents SceneTransitionTimeline::Advance(
        float deltaSeconds,
        const bool readyToReveal) noexcept
    {
        // 今回発生した段階の通知
        SceneTransitionTimelineEvents events;
        deltaSeconds = std::max(FiniteOr(deltaSeconds, 0.0f), 0.0f);
        // 段階の時間が0なら即座に完了させます(duration: 現在の段階の所要秒数)。
        const auto advance = [this, deltaSeconds](
                const float duration) noexcept
            {
                m_progress = duration > 0.0f
                    ? std::min(m_progress + deltaSeconds / duration, 1.0f)
                    : 1.0f;
            };
        switch (m_phase)
        {
        case SceneTransitionPhase::Covering:
            advance(m_settings.coverDuration);
            if (m_progress >= 1.0f)
            {
                m_phase = SceneTransitionPhase::Covered;
                m_progress = 1.0f;
                m_heldSeconds = 0.0f;
                m_readyFrames = 0;
                events.covered = true;
            }
            break;
        case SceneTransitionPhase::Covered:
        {
            m_heldSeconds += deltaSeconds;
            m_readyFrames = readyToReveal
                ? std::min(m_readyFrames + 1, 1000u)
                : 0u;
            // 時間のある遷移は準備完了後も2回の呼び出しを待ちます。
            // 準備完了を待つ呼び出し回数
            const std::uint32_t requiredFrames =
                IsInstantSceneTransition(m_settings) ? 1u : 2u;
            if (m_readyFrames >= requiredFrames
                && m_heldSeconds >= m_settings.holdDuration)
            {
                m_phase = SceneTransitionPhase::Revealing;
                m_progress = 0.0f;
                events.revealStarted = true;
            }
            break;
        }
        case SceneTransitionPhase::Revealing:
            advance(m_settings.revealDuration);
            if (m_progress >= 1.0f)
            {
                Reset();
                events.finished = true;
            }
            break;
        case SceneTransitionPhase::Idle:
        default:
            break;
        }
        return events;
    }

    void SceneTransitionTimeline::Reveal() noexcept
    {
        if (m_phase == SceneTransitionPhase::Idle
            || m_phase == SceneTransitionPhase::Revealing)
        {
            return;
        }
        // 切り替える直前の覆い具合
        const float currentCoverage = Coverage();
        m_phase = SceneTransitionPhase::Revealing;
        m_progress = InverseEasing(
            m_settings.easing,
            1.0f - currentCoverage);
        m_heldSeconds = 0.0f;
        m_readyFrames = 0;
    }

    void SceneTransitionTimeline::Reset() noexcept
    {
        m_phase = SceneTransitionPhase::Idle;
        m_progress = 0.0f;
        m_heldSeconds = 0.0f;
        m_readyFrames = 0;
    }

    float SceneTransitionTimeline::Coverage() const noexcept
    {
        switch (m_phase)
        {
        case SceneTransitionPhase::Covering:
            return EvaluateSceneTransitionEasing(
                m_settings.easing,
                m_progress);
        case SceneTransitionPhase::Covered:
            return 1.0f;
        case SceneTransitionPhase::Revealing:
            return 1.0f
                - EvaluateSceneTransitionEasing(
                    m_settings.easing,
                    m_progress);
        case SceneTransitionPhase::Idle:
        default:
            return 0.0f;
        }
    }
}
