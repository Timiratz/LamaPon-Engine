#pragma once

#include "LamaPon/Graphics/GraphicsResource.h"

#include <DirectXMath.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace LamaPon
{
    // 画面へ同時に効かせられるLight2Dの数。b1の定数バッファに載せる
    // ので、CustomParameters（b0・8本しかなく、自作Shaderの持ち物）を
    // 圧迫しません。
    inline constexpr std::size_t MaximumSprite2DLights = 16;

    struct Sprite2DLight final
    {
        // xy=画面ピクセル座標, z=届く半径, w=強さ。
        DirectX::XMFLOAT4 positionRadiusIntensity{};
        // rgb=色, w=予約。
        DirectX::XMFLOAT4 color{};
    };

    // 組み込みの2D照明Shader（LamaPonSpriteLit.hlsl）が読む灯り一覧。
    // 自作Shaderは宣言しなければ何の影響も受けません。
    struct Sprite2DLighting final
    {
        // x=灯数, yzw=予約。
        DirectX::XMUINT4 counts{};
        std::array<Sprite2DLight, MaximumSprite2DLights>
            lights{};
    };

    struct SpriteSourceRectangle final
    {
        std::int32_t left{};
        std::int32_t top{};
        std::int32_t right{};
        std::int32_t bottom{};
    };

    enum class SpriteFlip : std::uint8_t
    {
        None,
        Horizontal,
        Vertical,
        Both
    };

    enum class SpriteBlendMode : std::uint8_t
    {
        NonPremultiplied,
        AlphaBlend,
        Additive,
        Opaque
    };

    // 1枚のSpriteを描くAPI非依存requestです。textureがemptyなら、
    // 将来の描画経路ではwhite textureへfallbackします。tintは呼び出し側の
    // 値をそのまま使い、暗黙のpremultiplyは行いません。
    struct SpriteDrawRequest final
    {
        GraphicsViewHandle texture;
        DirectX::XMFLOAT2 position{};
        bool hasSourceRectangle{};
        SpriteSourceRectangle sourceRectangle{};
        DirectX::XMFLOAT4 tint{ 1.0f, 1.0f, 1.0f, 1.0f };
        float rotation{};
        DirectX::XMFLOAT2 origin{};
        DirectX::XMFLOAT2 scale{ 1.0f, 1.0f };
        SpriteFlip flip{ SpriteFlip::None };
        float layerDepth{};
    };

    struct SpritePassDescription final
    {
        SpriteBlendMode blend{
            SpriteBlendMode::NonPremultiplied };
        std::filesystem::path pixelShader;
        std::array<DirectX::XMFLOAT4, 8>
            customParameters{};
        Sprite2DLighting lighting{};
    };

    struct SpriteShaderStatus final
    {
        std::uint64_t generation{};
        std::string error;
    };
}
