#pragma once

#include "LamaPon/Graphics/GraphicsResource.h"

#include <cstdint>
#include <functional>

namespace LamaPon
{
    // Sky IBLとReflection Probeが共有するAPI非依存の畳み込み結果です。
    // 2本は同じBackend世代で一組として生成・更新されます。
    struct PrefilteredEnvironmentViews final
    {
        GraphicsViewHandle specular;
        GraphicsViewHandle irradiance;
        float specularMaximumMip{};

        [[nodiscard]] bool IsValid() const noexcept
        {
            return specular && irradiance;
        }
    };

    // Probeの各cube面を描く同期callbackです。描画先の切り替えは
    // GraphicsDevice側が行い、呼び出し側はその面のSceneだけを描きます。
    using EnvironmentProbeFaceRenderer =
        std::function<void(std::uint32_t face)>;
}
