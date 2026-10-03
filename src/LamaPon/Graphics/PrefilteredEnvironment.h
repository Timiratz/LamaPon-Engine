#pragma once

#include "LamaPon/Graphics/GraphicsResource.h"

#include <cstdint>
#include <functional>

namespace LamaPon
{
    // 同じバックエンド世代で一組として生成・更新する環境の畳込結果。
    struct PrefilteredEnvironmentViews final
    {
        // 鏡面反射用のキューブビュー
        GraphicsViewHandle specular;
        // 拡散照明用のキューブビュー
        GraphicsViewHandle irradiance;
        // 鏡面反射で使う最大ミップ
        float specularMaximumMip{};

        // 鏡面と拡散の両ビューが存在するか判定する。
        [[nodiscard]] bool IsValid() const noexcept
        {
            return specular && irradiance;
        }
    };

    // キューブ面のシーンを同期描画する(face: 面番号)。
    // 描画先の切替はGraphicsDeviceが行う。
    using EnvironmentProbeFaceRenderer =
        std::function<void(std::uint32_t face)>;
}
