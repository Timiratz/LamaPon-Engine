#pragma once

#include "LamaPon/Graphics/GraphicsResource.h"
#include "LamaPon/Graphics/LitMaterial.h"

#include <DirectXMath.h>

#include <array>

namespace LamaPon
{
    // 1回のLit描画で使うAPI非依存texture requestです。各handleは
    // requestの寿命中、参照先resourceを強所有します。emptyは未指定を
    // 表し、LitEffect固有のwhite / flat-normal fallbackが適用されます。
    // stale / 異種 / 別Backend世代のviewを含むrequestは拒否されます。
    struct LitTextureRequest final
    {
        GraphicsViewHandle albedo;
        GraphicsViewHandle normal;
        GraphicsViewHandle roughness;
        GraphicsViewHandle metallic;
        GraphicsViewHandle occlusion;
        GraphicsViewHandle emissive;
        std::array<
            GraphicsViewHandle,
            LitMaterial::CustomTextureCount> customTextures{};
        float occlusionStrength{ 1.0f };
        DirectX::XMFLOAT3 emissiveFactor{};
    };
}
