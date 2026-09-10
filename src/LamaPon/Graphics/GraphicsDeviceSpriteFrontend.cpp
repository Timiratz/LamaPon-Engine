#include "LamaPon/Graphics/GraphicsDevice.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Graphics/TextLayout.h"
#include "LamaPon/Scene/SceneManager.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>

namespace
{
    [[nodiscard]] DirectX::XMFLOAT4 Premultiplied(
        const DirectX::XMFLOAT4& color) noexcept
    {
        return {
            color.x * color.w,
            color.y * color.w,
            color.z * color.w,
            color.w
        };
    }

    void DrawRectangle(
        LamaPon::SpriteRenderPass& pass,
        const float x,
        const float y,
        const float width,
        const float height,
        const DirectX::XMFLOAT4& color)
    {
        LamaPon::SpriteDrawRequest request;
        request.position = { x, y };
        request.scale = {
            std::max(width, 0.0f),
            std::max(height, 0.0f) };
        request.tint = Premultiplied(color);
        static_cast<void>(pass.Draw(request));
    }
}

namespace LamaPon
{
    void GraphicsDevice::DrawLoadingScreen(
        const float progress,
        const SceneLoadingScreenSettings& settings,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        if (!settings.enabled)
        {
            return;
        }

        // AssetManagerとcanvas寸法も現在のBackend世代に属します。passを
        // 始める前の準備中から再初期化を拒否して、世代を混在させません。
        [[maybe_unused]] auto operationLease =
            AcquireResourceLease();
        const std::uint32_t canvasWidth =
            width == 0 ? m_width : width;
        const std::uint32_t canvasHeight =
            height == 0 ? m_height : height;
        auto pass = BeginSpritePass();
        DrawRectangle(
            pass,
            0.0f,
            0.0f,
            static_cast<float>(canvasWidth),
            static_cast<float>(canvasHeight),
            settings.backgroundColor);

        const float barWidth =
            std::min(
                std::clamp(
                    static_cast<float>(canvasWidth) * 0.58f,
                    240.0f,
                    760.0f),
                static_cast<float>(canvasWidth) * 0.9f);
        const float barHeight = 18.0f;
        const float barX =
            (static_cast<float>(canvasWidth) - barWidth)
            * 0.5f;
        const float barY =
            static_cast<float>(canvasHeight) * 0.62f;
        DrawRectangle(
            pass,
            barX,
            barY,
            barWidth,
            barHeight,
            settings.barBackgroundColor);
        DrawRectangle(
            pass,
            barX,
            barY,
            barWidth
                * std::clamp(progress, 0.0f, 1.0f),
            barHeight,
            settings.barFillColor);

        std::string label = settings.message;
        if (settings.showPercentage)
        {
            label += " ";
            label += std::to_string(
                static_cast<int>(
                    std::lround(
                        std::clamp(
                            progress,
                            0.0f,
                            1.0f)
                        * 100.0f)));
            label += "%";
        }
        const float textWidth =
            std::min(
                static_cast<float>(canvasWidth) * 0.8f,
                720.0f);
        const TextLayoutOptions layout{
            { textWidth, 64.0f },
            TextHorizontalAlignment::Center,
            TextVerticalAlignment::Center,
            false
        };
        const auto text = Assets().LoadTextTexture(
            label,
            "Yu Gothic UI",
            30.0f,
            layout);
        const auto textResources = text != nullptr
            ? text->resources.Acquire()
            : nullptr;
        if (textResources != nullptr
            && textResources->shaderResourceView)
        {
            SpriteDrawRequest request;
            request.texture =
                textResources->shaderResourceView;
            request.position = {
                (static_cast<float>(canvasWidth)
                    - static_cast<float>(text->width))
                    * 0.5f,
                barY - 86.0f
            };
            // 白で生成した文字テクスチャへ描画時の色を掛けます。
            request.tint = Premultiplied(settings.textColor);
            static_cast<void>(pass.Draw(request));
        }
        pass.End();
    }

    void GraphicsDevice::DrawStartupLogo(
        const std::filesystem::path& logoPath,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        if (logoPath.empty())
        {
            return;
        }

        // FileExists/LoadTextureから描画完了まで同じAssetManagerとBackendを
        // 使います。BeginSpritePassのleaseはpass本体だけを保護します。
        [[maybe_unused]] auto operationLease =
            AcquireResourceLease();
        if (!Assets().FileExists(logoPath))
        {
            return;
        }

        std::shared_ptr<const TextureAsset> logo;
        try
        {
            // テクスチャは最初のフレームでキャッシュされるため、起動処理で
            // 小さな画像を読み込むのは一度だけです。
            logo = Assets().LoadTexture(logoPath);
        }
        catch (const std::exception&)
        {
            return;
        }
        const auto logoResources = logo != nullptr
            ? logo->resources.Acquire()
            : nullptr;
        if (logoResources == nullptr
            || !logoResources->shaderResourceView
            || logo->width == 0
            || logo->height == 0)
        {
            return;
        }

        const std::uint32_t canvasWidth =
            width == 0 ? m_width : width;
        const std::uint32_t canvasHeight =
            height == 0 ? m_height : height;
        const float maximumWidth = std::min(
            static_cast<float>(canvasWidth) * 0.32f,
            360.0f);
        const float maximumHeight = std::min(
            static_cast<float>(canvasHeight) * 0.42f,
            360.0f);
        const float scale = std::min(
            maximumWidth / static_cast<float>(logo->width),
            maximumHeight / static_cast<float>(logo->height));
        const float drawWidth =
            static_cast<float>(logo->width) * scale;
        const float drawHeight =
            static_cast<float>(logo->height) * scale;
        const float x =
            (static_cast<float>(canvasWidth) - drawWidth) * 0.5f;
        const float y =
            static_cast<float>(canvasHeight) * 0.30f
            - drawHeight * 0.5f;

        SpriteDrawRequest request;
        request.texture =
            logoResources->shaderResourceView;
        request.position = { x, y };
        request.scale = { scale, scale };
        auto pass = BeginSpritePass();
        static_cast<void>(pass.Draw(request));
        pass.End();
    }

    void GraphicsDevice::SetSprite2DOffset(
        const DirectX::XMFLOAT2& offset) noexcept
    {
        m_sprite2DOffset = offset;
    }

    const DirectX::XMFLOAT2& GraphicsDevice::Sprite2DOffset() const noexcept
    {
        return m_sprite2DOffset;
    }
}
