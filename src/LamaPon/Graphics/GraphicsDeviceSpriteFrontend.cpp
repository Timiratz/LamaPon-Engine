#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsDeviceState.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/PathUtils.h"
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
#include <vector>

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
    // 画像が無い環境でも、遷移と読み込み画面は描き続けます）。
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
                const float quad = dotSize
                    / (2.0f * LamaPon::SceneTransitionCircleRadiusRatio);
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

    // 1枚の覆いを描きます。rectangleは白の1x1、図形はtextureの大きさ
    // （builtinは256x256）を基準に拡大します。
    void DrawTransitionQuad(
        LamaPon::SpriteRenderPass& pass,
        const LamaPon::SceneTransitionQuad& quad,
        const LamaPon::GraphicsViewHandle& texture,
        const float textureWidth,
        const float textureHeight)
    {
        LamaPon::SpriteDrawRequest request;
        request.texture = texture;
        request.scale = {
            quad.width / textureWidth,
            quad.height / textureHeight };
        if (quad.rotation == 0.0f)
        {
            request.position = { quad.x, quad.y };
        }
        else
        {
            // 回転は矩形の中心を軸にします。
            request.position = {
                quad.x + quad.width * 0.5f,
                quad.y + quad.height * 0.5f };
            request.origin = {
                textureWidth * 0.5f,
                textureHeight * 0.5f };
            request.rotation = quad.rotation;
        }
        // Sprite passの既定はNonPremultipliedなので、色はそのまま渡します。
        request.tint = quad.color;
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

    void GraphicsDevice::DrawSceneTransition(
        const SceneTransitionFrame& frame,
        const SceneLoadingScreenSettings& loadingScreen,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        const bool drawsCover =
            frame.phase != SceneTransitionPhase::Idle
            && frame.settings.effect != SceneTransitionEffect::None
            && frame.coverage > 0.0f;
        const float loadingAlpha =
            std::clamp(frame.loadingScreenAlpha, 0.0f, 1.0f);
        const bool drawsLoading =
            loadingScreen.enabled && loadingAlpha > 0.0f;
        if (!drawsCover && !drawsLoading)
        {
            return;
        }

        [[maybe_unused]] auto operationLease =
            AcquireResourceLease();
        const float canvasWidth = static_cast<float>(
            width == 0 ? m_state->m_width : width);
        const float canvasHeight = static_cast<float>(
            height == 0 ? m_state->m_height : height);
        const bool revealing =
            frame.phase == SceneTransitionPhase::Revealing;

        if (drawsCover)
        {
            auto settings = frame.settings;
            bool drawn = false;
            if (settings.effect == SceneTransitionEffect::Shader
                && frame.coverage < 1.0f)
            {
                drawn = DrawSceneTransitionShader(
                    settings,
                    frame.coverage,
                    revealing,
                    canvasWidth,
                    canvasHeight);
            }
            if (!drawn)
            {
                std::vector<SceneTransitionQuad> quads;
                BuildSceneTransitionQuads(
                    settings,
                    frame.coverage,
                    revealing,
                    canvasWidth,
                    canvasHeight,
                    quads);
                std::uint32_t circleWidth{};
                std::uint32_t circleHeight{};
                std::uint32_t irisWidth{};
                std::uint32_t irisHeight{};
                std::shared_ptr<const TextureResourceSnapshot> circle;
                std::shared_ptr<const TextureResourceSnapshot> iris;
                bool missingShape = false;
                for (const auto& quad : quads)
                {
                    if (quad.shape == SceneTransitionShape::Circle
                        && circle == nullptr)
                    {
                        circle = TryLoadTextureResources(
                            Assets(),
                            L"builtin/circle",
                            circleWidth,
                            circleHeight);
                        missingShape = missingShape || circle == nullptr;
                    }
                    if (quad.shape
                            == SceneTransitionShape::InverseCircle
                        && iris == nullptr)
                    {
                        iris = TryLoadTextureResources(
                            Assets(),
                            L"builtin/iris",
                            irisWidth,
                            irisHeight);
                        missingShape = missingShape || iris == nullptr;
                    }
                }
                if (missingShape)
                {
                    // 円の画像を作れない場合も、画面は確実に覆います。
                    settings.effect = SceneTransitionEffect::Fade;
                    BuildSceneTransitionQuads(
                        settings,
                        frame.coverage,
                        revealing,
                        canvasWidth,
                        canvasHeight,
                        quads);
                }
                auto pass = BeginSpritePass();
                for (const auto& quad : quads)
                {
                    switch (quad.shape)
                    {
                    case SceneTransitionShape::Circle:
                        DrawTransitionQuad(
                            pass,
                            quad,
                            circle->shaderResourceView,
                            static_cast<float>(circleWidth),
                            static_cast<float>(circleHeight));
                        break;
                    case SceneTransitionShape::InverseCircle:
                        DrawTransitionQuad(
                            pass,
                            quad,
                            iris->shaderResourceView,
                            static_cast<float>(irisWidth),
                            static_cast<float>(irisHeight));
                        break;
                    case SceneTransitionShape::Rectangle:
                    default:
                        DrawTransitionQuad(
                            pass,
                            quad,
                            {},
                            1.0f,
                            1.0f);
                        break;
                    }
                }
                pass.End();
            }
        }

        if (drawsLoading)
        {
            auto pass = BeginSpritePass();
            DrawLoadingOverlay(
                pass,
                Assets(),
                frame.loadingProgress,
                loadingScreen,
                frame.legacyLoadingScreen ? 1.0f : loadingAlpha,
                frame.loadingScreenTime,
                canvasWidth,
                canvasHeight);
            pass.End();
        }
    }

    bool GraphicsDevice::DrawSceneTransitionShader(
        const SceneTransitionSettings& settings,
        const float coverage,
        const bool revealing,
        const float canvasWidth,
        const float canvasHeight)
    {
        std::uint32_t ruleWidth{};
        std::uint32_t ruleHeight{};
        const auto rule = TryLoadTextureResources(
            Assets(),
            settings.ruleTexture,
            ruleWidth,
            ruleHeight);
        const auto shaderFrame = BuildSceneTransitionShaderFrame(
            settings,
            coverage,
            revealing,
            canvasWidth,
            canvasHeight,
            rule != nullptr);

        SpritePassDescription description;
        description.pixelShader = settings.shader.empty()
            ? std::filesystem::path(
                std::u8string(
                    SceneTransitionBuiltInShader.begin(),
                    SceneTransitionBuiltInShader.end()))
            : settings.shader;
        description.customParameters = shaderFrame.parameters;
        auto pass = BeginSpritePass(description);
        const auto status = pass.ShaderStatus();
        if (status.fallback != SpriteShaderFallback::None
            || !status.error.empty())
        {
            // 代替シェーダーで全画面を塗ると画面が一瞬で隠れるため、
            // 何も描かずにFadeへ切り替えます。
            pass.Abort();
            if (m_state->m_sceneTransitionShaderError != status.error)
            {
                m_state->m_sceneTransitionShaderError = status.error;
                Logger::Instance().Warning(
                    "シーン遷移のシェーダーを使えないため、フェードで"
                    "代用します: "
                    + PathToUtf8(description.pixelShader)
                    + " | "
                    + status.error);
            }
            return false;
        }
        m_state->m_sceneTransitionShaderError.clear();

        SpriteDrawRequest request;
        if (rule != nullptr)
        {
            request.texture = rule->shaderResourceView;
            request.scale = {
                canvasWidth / static_cast<float>(ruleWidth),
                canvasHeight / static_cast<float>(ruleHeight) };
        }
        else
        {
            request.scale = { canvasWidth, canvasHeight };
        }
        request.tint = shaderFrame.tint;
        static_cast<void>(pass.Draw(request));
        pass.End();
        return true;
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
