#pragma once

// 自動露出の順応計算です。D3D11とD3D12のRenderTarget stateが同じ式で
// 露出補正を更新するためのRuntime内部headerで、SDKにはinstallしません。
#include "LamaPon/Graphics/EnvironmentSettings.h"
#include "LamaPon/Graphics/RenderTargetBackendState.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace LamaPon::Detail
{
    // 順応をやめたら状態も捨てます。入れ直したときは、最初に測れた
    // 明るさへそのまま飛ばしたいためです（何秒も前の値から追いつかせると
    // 暗転や白飛びから始まります）。
    inline void ResetAutoExposure(
        RenderTargetBackendState& state) noexcept
    {
        state.m_adaptedLuminance = 0.0f;
        state.m_autoExposureStops = 0.0f;
    }

    // Backendが非同期に読めた前フレームの測定結果で順応を進め、露出の
    // 補正（段数）を返します。未完成ならそのフレームの順応更新だけを
    // 見送ります。
    [[nodiscard]] inline float AdvanceAutoExposure(
        RenderTargetBackendState& state,
        const std::optional<float> measuredLuminance,
        const AutoExposureSettings& settings,
        const float deltaSeconds) noexcept
    {
        const float minimumLuminance = std::max(
            settings.minimumLuminance,
            0.0001f);
        const float maximumLuminance = std::max(
            settings.maximumLuminance,
            minimumLuminance);
        if (!measuredLuminance.has_value())
        {
            return state.m_autoExposureStops;
        }

        const float measured = std::clamp(
            *measuredLuminance,
            minimumLuminance,
            maximumLuminance);
        if (state.m_adaptedLuminance <= 0.0f)
        {
            // 初回は測定値を直接採用し、起動直後の
            // 不要な露出変化を避けます。
            state.m_adaptedLuminance = measured;
        }
        else
        {
            // 明所と暗所で異なる順応速度を適用します。
            const float speed = measured > state.m_adaptedLuminance
                ? std::max(settings.speedToBright, 0.0f)
                : std::max(settings.speedToDark, 0.0f);
            // 指数補間により順応時間をフレームレートから分離し、
            // 大きなdeltaTimeでも行き過ぎを防ぎます。
            const float blend = speed > 0.0f
                ? 1.0f - std::exp(
                    -std::max(deltaSeconds, 0.0f) * speed)
                : 0.0f;
            state.m_adaptedLuminance +=
                (measured - state.m_adaptedLuminance) * blend;
        }
        // 露出は段数（exp2で効く）なのでlog2で渡します。
        state.m_autoExposureStops = std::log2(
            std::max(settings.keyValue, 0.0001f)
            / std::max(state.m_adaptedLuminance, 0.0001f));
        return state.m_autoExposureStops;
    }
}
