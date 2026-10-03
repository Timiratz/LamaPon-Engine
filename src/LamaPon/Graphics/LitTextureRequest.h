#pragma once

#include "LamaPon/Graphics/GraphicsResource.h"
#include "LamaPon/Graphics/LitMaterial.h"

#include <DirectXMath.h>

#include <array>

namespace LamaPon
{
    // 一回のLit描画に使うビューを要求の寿命中保持する。
    // 空のビューは白色・平坦法線の代替を使い、失効・異種・別Backend世代のビューを含む要求は拒否する。
    struct LitTextureRequest final
    {
        // 基本色画像の共有ビュー
        GraphicsViewHandle albedo;
        // 法線画像の共有ビュー
        GraphicsViewHandle normal;
        // 粗さ画像の共有ビュー
        GraphicsViewHandle roughness;
        // 金属度画像の共有ビュー
        GraphicsViewHandle metallic;
        // 遮蔽画像の共有ビュー
        GraphicsViewHandle occlusion;
        // 発光画像の共有ビュー
        GraphicsViewHandle emissive;
        // t7からの追加画像の共有ビュー
        std::array<
            GraphicsViewHandle,
            LitMaterial::CustomTextureCount> customTextures{};
        // 遮蔽を適用する強さ
        float occlusionStrength{ 1.0f };
        // 発光のRGB倍率
        DirectX::XMFLOAT3 emissiveFactor{};
    };
}
