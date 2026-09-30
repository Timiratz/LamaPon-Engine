#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsDeviceState.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Graphics/TextLayout.h"
#include "LamaPon/Scene/SceneManager.h"
#include "LamaPon/Scene/SceneTransition.h"

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

    [[nodiscard]] DirectX::XMFLOAT4 WithAlpha(
        const DirectX::XMFLOAT4& color,
        const float alpha) noexcept
    {
        return { color.x, color.y, color.z, color.w * alpha };
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

    // textureを読めない場合はnullptrを返します（組み込みやassetsの
    // 画像が無い環境でも、読み込み画面は描き続けます）。
    [[nodiscard]] std::shared_ptr<const LamaPon::TextureResourceSnapshot>
        TryLoadTextureResources(
            LamaPon::AssetManager& assets,
            const std::filesystem::path& path,
            std::uint32_t& width,
            std::uint32_t& height) noexcept
    {
        try
        {
            if (path.empty())
            {
                return nullptr;
            }
            const auto texture = assets.LoadTexture(path);
            if (texture == nullptr
                || texture->width == 0
                || texture->height == 0)
            {
                return nullptr;
            }
            auto resources = texture->resources.Acquire();
            if (resources == nullptr
                || !resources->shaderResourceView)
            {
                return nullptr;
            }
            width = texture->width;
            height = texture->height;
            return resources;
        }
        catch (const std::exception&)
        {
            return nullptr;
        }
    }

    void DrawCenteredText(
        LamaPon::SpriteRenderPass& pass,
        LamaPon::AssetManager& assets,
        const std::string& text,
        const float fontSize,
        const float canvasWidth,
        const float top,
        const DirectX::XMFLOAT4& color)
    {
        if (text.empty())
        {
            return;
        }
        const float textWidth =
            std::min(canvasWidth * 0.8f, 720.0f);
        const LamaPon::TextLayoutOptions layout{
            { textWidth, fontSize * 2.1f },
            LamaPon::TextHorizontalAlignment::Center,
            LamaPon::TextVerticalAlignment::Center,
            false
        };
        const auto texture = assets.LoadTextTexture(
            text,
            "Yu Gothic UI",
            fontSize,
            layout);
        const auto resources = texture != nullptr
            ? texture->resources.Acquire()
            : nullptr;
        if (resources == nullptr
            || !resources->shaderResourceView)
        {
            return;
        }
        LamaPon::SpriteDrawRequest request;
        request.texture = resources->shaderResourceView;
        request.position = {
            (canvasWidth - static_cast<float>(texture->width)) * 0.5f,
            top
        };
        // 白で生成した文字テクスチャへ描画時の色を掛けます。
        request.tint = Premultiplied(color);
        static_cast<void>(pass.Draw(request));
    }

    // 標準の読み込み画面です。alphaが1で追加項目が既定値のときは、
    // 従来のDrawLoadingScreenと同じ描画になります。
    void DrawLoadingOverlay(
        LamaPon::SpriteRenderPass& pass,
        LamaPon::AssetManager& assets,
        const float progress,
        const LamaPon::SceneLoadingScreenSettings& settings,
        const float alpha,
        const float elapsedSeconds,
        const float canvasWidth,
        const float canvasHeight)
    {
        DrawRectangle(
            pass,
            0.0f,
            0.0f,
            canvasWidth,
            canvasHeight,
            WithAlpha(settings.backgroundColor, alpha));

        std::uint32_t imageWidth{};
        std::uint32_t imageHeight{};
        if (const auto image = TryLoadTextureResources(
                assets,
                settings.backgroundTexture,
                imageWidth,
                imageHeight))
        {
            // 縦横比を保ったまま画面全体を覆う大きさにします。
            const float scale = std::max(
                canvasWidth / static_cast<float>(imageWidth),
                canvasHeight / static_cast<float>(imageHeight));
            LamaPon::SpriteDrawRequest request;
            request.texture = image->shaderResourceView;
            request.position = {
                (canvasWidth - static_cast<float>(imageWidth) * scale)
                    * 0.5f,
                (canvasHeight - static_cast<float>(imageHeight) * scale)
                    * 0.5f
            };
            request.scale = { scale, scale };
            request.tint = Premultiplied({ 1.0f, 1.0f, 1.0f, alpha });
            static_cast<void>(pass.Draw(request));
        }

        const float barWidth =
            std::min(
                std::clamp(
                    canvasWidth * 0.58f,
                    240.0f,
                    760.0f),
                canvasWidth * 0.9f);
        const float barHeight = 18.0f;
        const float barX = (canvasWidth - barWidth) * 0.5f;
        const float barY = canvasHeight * 0.62f;
        DrawRectangle(
            pass,
            barX,
            barY,
            barWidth,
            barHeight,
            WithAlpha(settings.barBackgroundColor, alpha));
        DrawRectangle(
            pass,
            barX,
            barY,
            barWidth * std::clamp(progress, 0.0f, 1.0f),
            barHeight,
            WithAlpha(settings.barFillColor, alpha));

        std::string label = settings.message;
        if (settings.showPercentage)
        {
            label += " ";
            label += std::to_string(
                static_cast<int>(
                    std::lround(
                        std::clamp(progress, 0.0f, 1.0f)
                        * 100.0f)));
            label += "%";
        }
        const auto textColor = WithAlpha(settings.textColor, alpha);
        {
            const float textWidth =
                std::min(canvasWidth * 0.8f, 720.0f);
            const LamaPon::TextLayoutOptions layout{
                { textWidth, 64.0f },
                LamaPon::TextHorizontalAlignment::Center,
                LamaPon::TextVerticalAlignment::Center,
                false
            };
            const auto text = assets.LoadTextTexture(
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
                LamaPon::SpriteDrawRequest request;
                request.texture =
                    textResources->shaderResourceView;
                request.position = {
                    (canvasWidth - static_cast<float>(text->width))
                        * 0.5f,
                    barY - 86.0f
                };
                // 白で生成した文字テクスチャへ描画時の色を掛けます。
                request.tint = Premultiplied(textColor);
                static_cast<void>(pass.Draw(request));
            }
        }

        DrawCenteredText(
            pass,
            assets,
            settings.hint,
            20.0f,
            canvasWidth,
            barY + barHeight + 18.0f,
            WithAlpha(settings.textColor, alpha * 0.8f));

        if (!settings.showSpinner)
        {
            return;
        }
        // 右下で回る8つの点です。進捗が止まって見える読み込みでも、
        // 固まっていないことが分かるようにします。
        std::uint32_t dotWidth{};
        std::uint32_t dotHeight{};
        const auto dot = TryLoadTextureResources(
            assets,
            L"builtin/circle",
            dotWidth,
            dotHeight);
        constexpr int DotCount = 8;
        constexpr float TwoPi = 6.28318531f;
        const float radius = 16.0f;
        const float dotSize = 7.0f;
        const float centerX = canvasWidth - 48.0f;
        const float centerY = canvasHeight - 48.0f;
        const float head = elapsedSeconds * 1.25f;
        for (int index{}; index < DotCount; ++index)
        {
            const float slot =
                static_cast<float>(index) / static_cast<float>(DotCount);
            const float angle = slot * TwoPi - TwoPi * 0.25f;
            // 先頭の点ほど濃く、後ろへ行くほど薄くします。
            float trail = head - slot;
            trail -= std::floor(trail);
            const float dotAlpha = alpha * (1.0f - trail * 0.8f);
            const float x = centerX + std::cos(angle) * radius;
            const float y = centerY + std::sin(angle) * radius;
            LamaPon::SpriteDrawRequest request;
            if (dot != nullptr)
            {
                // builtin/circleの円は一辺の112/256の半径なので、
                // 直径がdotSizeになるように拡大します。
                constexpr float CircleRadiusRatio = 112.0f / 256.0f;
                const float quad = dotSize / (2.0f * CircleRadiusRatio);
                request.texture = dot->shaderResourceView;
                request.position = { x - quad * 0.5f, y - quad * 0.5f };
                request.scale = {
                    quad / static_cast<float>(dotWidth),
                    quad / static_cast<float>(dotHeight) };
            }
            else
            {
                request.position = {
                    x - dotSize * 0.5f,
                    y - dotSize * 0.5f };
                request.scale = { dotSize, dotSize };
            }
            request.tint = Premultiplied(
                WithAlpha(settings.textColor, dotAlpha));
            static_cast<void>(pass.Draw(request));
        }
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
            width == 0 ? m_state->m_width : width;
        const std::uint32_t canvasHeight =
            height == 0 ? m_state->m_height : height;
        auto pass = BeginSpritePass();
        DrawLoadingOverlay(
            pass,
            Assets(),
            progress,
            settings,
            1.0f,
            0.0f,
            static_cast<float>(canvasWidth),
            static_cast<float>(canvasHeight));
        pass.End();
    }

    void GraphicsDevice::DrawLoadingScreen(
        const SceneTransitionFrame& frame,
        const SceneLoadingScreenSettings& settings,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        const float alpha = frame.legacyLoadingScreen
            ? (frame.loadingScreenAlpha > 0.0f ? 1.0f : 0.0f)
            : std::clamp(frame.loadingScreenAlpha, 0.0f, 1.0f);
        if (!settings.enabled || alpha <= 0.0f)
        {
            return;
        }

        [[maybe_unused]] auto operationLease =
            AcquireResourceLease();
        const std::uint32_t canvasWidth =
            width == 0 ? m_state->m_width : width;
        const std::uint32_t canvasHeight =
            height == 0 ? m_state->m_height : height;
        auto pass = BeginSpritePass();
        DrawLoadingOverlay(
            pass,
            Assets(),
            frame.loadingProgress,
            settings,
            alpha,
            frame.loadingScreenTime,
            static_cast<float>(canvasWidth),
            static_cast<float>(canvasHeight));
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
            width == 0 ? m_state->m_width : width;
        const std::uint32_t canvasHeight =
            height == 0 ? m_state->m_height : height;
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
        m_state->m_sprite2DOffset = offset;
    }

    const DirectX::XMFLOAT2& GraphicsDevice::Sprite2DOffset() const noexcept
    {
        return m_state->m_sprite2DOffset;
    }
}
