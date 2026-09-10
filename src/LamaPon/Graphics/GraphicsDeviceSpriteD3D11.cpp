#include "LamaPon/Graphics/GraphicsDevice.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Graphics/GraphicsDeviceApiResources.h"
#include "LamaPon/Graphics/TextLayout.h"
#include "LamaPon/Scene/SceneManager.h"

#include <CommonStates.h>
#include <SpriteBatch.h>

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>

namespace LamaPon
{
    DirectX::SpriteBatch& GraphicsDevice::BeginSprites()
    {
        auto& resources = RequireD3D11ApiResources();
        resources.spriteTexturePins.clear();
        resources.uiScissorStack.clear();
        resources.spriteBatch->Begin(
            DirectX::SpriteSortMode_Deferred,
            resources.commonStates->NonPremultiplied());
        return *resources.spriteBatch;
    }

    void GraphicsDevice::EndSprites()
    {
        auto& resources = RequireD3D11ApiResources();
        resources.spriteBatch->End();
        resources.spriteTexturePins.clear();
        resources.uiScissorStack.clear();
    }

    void GraphicsDevice::PushUIScissor(
        const float minimumX,
        const float minimumY,
        const float maximumX,
        const float maximumY)
    {
        auto& resources = RequireD3D11ApiResources();
        D3D11_RECT scissor{
            static_cast<LONG>(
                std::max(minimumX, 0.0f)),
            static_cast<LONG>(
                std::max(minimumY, 0.0f)),
            static_cast<LONG>(
                std::max(maximumX, 0.0f)),
            static_cast<LONG>(
                std::max(maximumY, 0.0f)) };
        // 入れ子は交差矩形にします。
        if (!resources.uiScissorStack.empty())
        {
            const auto& outer = resources.uiScissorStack.back();
            scissor.left =
                std::max(scissor.left, outer.left);
            scissor.top =
                std::max(scissor.top, outer.top);
            scissor.right =
                std::min(scissor.right, outer.right);
            scissor.bottom =
                std::min(scissor.bottom, outer.bottom);
        }
        scissor.right =
            std::max(scissor.right, scissor.left);
        scissor.bottom =
            std::max(scissor.bottom, scissor.top);
        resources.uiScissorStack.push_back(scissor);

        // 進行中のバッチを確定してからシザー状態へ切り替えます。
        resources.spriteBatch->End();
        Context()->RSSetScissorRects(1, &scissor);
        resources.spriteBatch->Begin(
            DirectX::SpriteSortMode_Deferred,
            resources.commonStates->NonPremultiplied(),
            nullptr,
            nullptr,
            resources.uiScissorRasterizer.Get());
    }

    void GraphicsDevice::PopUIScissor()
    {
        auto& resources = RequireD3D11ApiResources();
        if (resources.uiScissorStack.empty())
        {
            return;
        }
        resources.uiScissorStack.pop_back();
        resources.spriteBatch->End();
        if (resources.uiScissorStack.empty())
        {
            resources.spriteBatch->Begin(
                DirectX::SpriteSortMode_Deferred,
                resources.commonStates->NonPremultiplied());
            return;
        }
        Context()->RSSetScissorRects(
            1,
            &resources.uiScissorStack.back());
        resources.spriteBatch->Begin(
            DirectX::SpriteSortMode_Deferred,
            resources.commonStates->NonPremultiplied(),
            nullptr,
            nullptr,
            resources.uiScissorRasterizer.Get());
    }

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

        using namespace DirectX;
        const std::uint32_t canvasWidth =
            width == 0 ? m_width : width;
        const std::uint32_t canvasHeight =
            height == 0 ? m_height : height;
        const auto premultiplied =
            [](const XMFLOAT4& color) noexcept
            {
                return XMFLOAT4{
                    color.x * color.w,
                    color.y * color.w,
                    color.z * color.w,
                    color.w
                };
            };
        const auto drawRectangle =
            [this, &premultiplied](
                SpriteBatch& sprites,
                const float x,
                const float y,
                const float width,
                const float height,
                const XMFLOAT4& color)
            {
                const auto tint =
                    premultiplied(color);
                sprites.Draw(
                    WhiteTexture(),
                    XMFLOAT2{ x, y },
                    nullptr,
                    XMLoadFloat4(&tint),
                    0.0f,
                    XMFLOAT2{},
                    XMFLOAT2{
                        std::max(width, 0.0f),
                        std::max(height, 0.0f)
                    });
            };

        auto& sprites = BeginSprites();
        drawRectangle(
            sprites,
            0.0f,
            0.0f,
            static_cast<float>(canvasWidth),
            static_cast<float>(canvasHeight),
            settings.backgroundColor);

        const float barWidth =
            std::min(
                std::clamp(
                    static_cast<float>(
                        canvasWidth) * 0.58f,
                    240.0f,
                    760.0f),
                static_cast<float>(
                    canvasWidth) * 0.9f);
        const float barHeight = 18.0f;
        const float barX =
            (static_cast<float>(canvasWidth) - barWidth)
            * 0.5f;
        const float barY =
            static_cast<float>(canvasHeight) * 0.62f;
        drawRectangle(
            sprites,
            barX,
            barY,
            barWidth,
            barHeight,
            settings.barBackgroundColor);
        drawRectangle(
            sprites,
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
        auto* const textView = textResources != nullptr
            ? TryResolveD3D11ShaderResourceView(*textResources)
            : nullptr;
        if (textView != nullptr)
        {
            // 白で生成した文字テクスチャへ描画時の色を掛けます。
            sprites.Draw(
                textView,
                XMFLOAT2{
                    (static_cast<float>(canvasWidth)
                        - static_cast<float>(
                            text->width))
                        * 0.5f,
                    barY - 86.0f
                },
                nullptr,
                PremultipliedTextColor(
                    settings.textColor));
        }
        EndSprites();
    }

    void GraphicsDevice::DrawStartupLogo(
        const std::filesystem::path& logoPath,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        if (logoPath.empty() || !Assets().FileExists(logoPath))
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
        auto* const logoView = logoResources != nullptr
            ? TryResolveD3D11ShaderResourceView(*logoResources)
            : nullptr;
        if (logoView == nullptr
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

        auto& sprites = BeginSprites();
        const DirectX::XMFLOAT4 white{ 1.0f, 1.0f, 1.0f, 1.0f };
        sprites.Draw(
            logoView,
            DirectX::XMFLOAT2{ x, y },
            nullptr,
            DirectX::XMLoadFloat4(&white),
            0.0f,
            DirectX::XMFLOAT2{},
            DirectX::XMFLOAT2{ scale, scale });
        EndSprites();
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
