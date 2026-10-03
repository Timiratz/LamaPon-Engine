#pragma once

#include "LamaPon/Graphics/EnvironmentSettings.h"
#include "LamaPon/Graphics/RenderTargetBackendState.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace LamaPon::Detail
{
    // 初回測定を直接採用できるよう順応状態を初期化する(state: 更新する描画先の状態)。
    inline void ResetAutoExposure(
        RenderTargetBackendState& state) noexcept
    {
        state.m_adaptedLuminance = 0.0f;
        state.m_autoExposureStops = 0.0f;
    }

    // 前フレームの測定から順応を更新して露出段数を返す(state: 更新する描画先の状態, measuredLuminance: 任意の測定輝度, settings: 順応の設定, deltaSeconds: 経過秒)。
    // 未測定なら現在値を返し、初回は測定値を直接採用する。
    // 測定輝度・設定・経過秒には有限値を渡す。
    [[nodiscard]] inline float AdvanceAutoExposure(
        RenderTargetBackendState& state,
        const std::optional<float> measuredLuminance,
        const AutoExposureSettings& settings,
        const float deltaSeconds) noexcept
    {
        // 下限を保証した測定輝度
        const float minimumLuminance = std::max(
            settings.minimumLuminance,
            0.0001f);
        // 下限以上の測定輝度上限
        const float maximumLuminance = std::max(
            settings.maximumLuminance,
            minimumLuminance);
        if (!measuredLuminance.has_value())
        {
            return state.m_autoExposureStops;
        }

        // 範囲に収めた測定輝度
        const float measured = std::clamp(
            *measuredLuminance,
            minimumLuminance,
            maximumLuminance);
        if (state.m_adaptedLuminance <= 0.0f)
        {
            // 初回は測定値を直接使い、起動時の露出変化を避ける。
            state.m_adaptedLuminance = measured;
        }
        else
        {

            // 明暗に応じた順応速度
            const float speed = measured > state.m_adaptedLuminance
                ? std::max(settings.speedToBright, 0.0f)
                : std::max(settings.speedToDark, 0.0f);
            // 指数補間でフレーム間隔による行き過ぎを防ぐ。
            // 経過時間による順応の補間率
            const float blend = speed > 0.0f
                ? 1.0f - std::exp(
                    -std::max(deltaSeconds, 0.0f) * speed)
                : 0.0f;
            state.m_adaptedLuminance +=
                (measured - state.m_adaptedLuminance) * blend;
        }
        // 露出倍率はlog2で段数へ変換する。
        state.m_autoExposureStops = std::log2(
            std::max(settings.keyValue, 0.0001f)
            / std::max(state.m_adaptedLuminance, 0.0001f));
        return state.m_autoExposureStops;
    }
}
