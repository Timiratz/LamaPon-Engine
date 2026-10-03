#pragma once

#include "LamaPon/Graphics/GraphicsResource.h"

#include <DirectXMath.h>

namespace LamaPon
{
    // 一つの物体へ適用する最大二個の反射プローブの環境情報。
    // 主プローブが無効ならSky環境を使い、射影箱は半径の三軸が全て正のときだけ適用する。
    struct ReflectionProbeEnvironment final
    {
        // 主プローブの鏡面ビュー
        GraphicsViewHandle specular;
        // 主プローブの拡散ビュー
        GraphicsViewHandle irradiance;
        // 主プローブの最大ミップ
        float specularMaximumMip{};
        // 主プローブの照明強度
        float intensity{ 1.0f };
        // 主プローブの射影箱の中心
        DirectX::XMFLOAT3 boxCenter{};
        // 主プローブの射影箱の半径
        DirectX::XMFLOAT3 boxExtents{};

        // 副プローブの鏡面ビュー
        GraphicsViewHandle secondarySpecular;
        // 副プローブの拡散ビュー
        GraphicsViewHandle secondaryIrradiance;
        // 副プローブの最大ミップ
        float secondarySpecularMaximumMip{};
        // 副プローブの射影箱の中心
        DirectX::XMFLOAT3 secondaryBoxCenter{};
        // 副プローブの射影箱の半径
        DirectX::XMFLOAT3 secondaryBoxExtents{};
        // 副プローブの比率、0は主のみ
        float secondaryWeight{};

        // 主プローブの鏡面と拡散の両ビューが存在するか判定する。
        [[nodiscard]] bool IsValid() const noexcept
        {
            return specular && irradiance;
        }
        // 副プローブの両ビューが存在し混合比率が正か判定する。
        [[nodiscard]] bool IsBlended() const noexcept
        {
            return secondarySpecular
                && secondaryIrradiance
                && secondaryWeight > 0.0f;
        }
    };
}
