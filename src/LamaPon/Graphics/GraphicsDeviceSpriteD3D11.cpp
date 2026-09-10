#include "LamaPon/Graphics/GraphicsDevice.h"

#include "LamaPon/Graphics/GraphicsDeviceApiResources.h"

#include <CommonStates.h>
#include <SpriteBatch.h>

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using SpriteResources =
        LamaPon::Detail::GraphicsDeviceD3D11Resources;
    using SpriteOwner =
        LamaPon::Detail::D3D11SpriteBatchOwner;

    [[nodiscard]] ID3D11BlendState* ResolveSpriteBlendState(
        SpriteResources& resources,
        const LamaPon::SpriteBlendMode blend)
    {
        switch (blend)
        {
        case LamaPon::SpriteBlendMode::NonPremultiplied:
            return resources.commonStates->NonPremultiplied();
        case LamaPon::SpriteBlendMode::AlphaBlend:
            return resources.commonStates->AlphaBlend();
        case LamaPon::SpriteBlendMode::Additive:
            return resources.commonStates->Additive();
        case LamaPon::SpriteBlendMode::Opaque:
            return resources.commonStates->Opaque();
        default:
            throw std::invalid_argument(
                "The sprite blend mode is invalid.");
        }
    }

    void ClearCompletedSpriteBatch(
        SpriteResources& resources) noexcept
    {
        resources.spriteShaderCallback = {};
        resources.spriteBlendState = nullptr;
        resources.uiScissorStack.clear();
        resources.spriteViewPins.clear();
        resources.spriteTexturePins.clear();
        resources.spriteBatchNativeBegun = false;
        resources.spriteBatchToken = 0;
        resources.spriteBatchOwner = SpriteOwner::None;
    }

    void RollBackSpriteReservation(
        SpriteResources& resources) noexcept
    {
        resources.spriteShaderCallback = {};
        resources.spriteBlendState = nullptr;
        resources.spriteBatchNativeBegun = false;
        resources.spriteBatchToken = 0;
        resources.spriteBatchOwner = SpriteOwner::None;
    }

    void PoisonSpriteBatch(
        SpriteResources& resources) noexcept
    {
        // DirectXTKはEnd失敗後の再利用を保証しません。Deferred Drawが
        // 参照しているhandleも、Backend再初期化までは保持します。
        resources.spriteBatchOwner = SpriteOwner::Poisoned;
    }

    void RequireAvailableSpriteBatch(
        const SpriteResources& resources)
    {
        if (resources.spriteBatchOwner == SpriteOwner::Poisoned)
        {
            throw std::logic_error(
                "The DirectX 11 sprite batch failed and must be recovered "
                "by reinitializing the graphics device.");
        }
        if (resources.spriteBatchOwner != SpriteOwner::None)
        {
            throw std::logic_error(
                "A sprite render pass is already active.");
        }
    }

    void RequireSpriteOwner(
        const SpriteResources& resources,
        const SpriteOwner owner,
        const std::uint64_t token)
    {
        if (resources.spriteBatchOwner == SpriteOwner::Poisoned)
        {
            throw std::logic_error(
                "The DirectX 11 sprite batch is unavailable after a "
                "rendering failure.");
        }
        if (resources.spriteBatchOwner != owner
            || resources.spriteBatchToken != token
            || !resources.spriteBatchNativeBegun)
        {
            throw std::logic_error(
                "The sprite render pass does not own the active batch.");
        }
    }

    void BeginNativeSpriteBatch(
        SpriteResources& resources,
        ID3D11RasterizerState* const rasterizer)
    {
        resources.spriteBatch->Begin(
            DirectX::SpriteSortMode_Deferred,
            resources.spriteBlendState,
            nullptr,
            nullptr,
            rasterizer,
            resources.spriteShaderCallback);
        resources.spriteBatchNativeBegun = true;
    }

    [[nodiscard]] bool IsFinite(
        const DirectX::XMFLOAT2& value) noexcept
    {
        return std::isfinite(value.x)
            && std::isfinite(value.y);
    }

    [[nodiscard]] bool IsFinite(
        const DirectX::XMFLOAT4& value) noexcept
    {
        return std::isfinite(value.x)
            && std::isfinite(value.y)
            && std::isfinite(value.z)
            && std::isfinite(value.w);
    }

    [[nodiscard]] bool IsFinite(
        const LamaPon::SpriteClipRectangle& value) noexcept
    {
        return std::isfinite(value.minimumX)
            && std::isfinite(value.minimumY)
            && std::isfinite(value.maximumX)
            && std::isfinite(value.maximumY);
    }

    [[nodiscard]] DirectX::SpriteEffects ResolveSpriteEffects(
        const LamaPon::SpriteFlip flip)
    {
        switch (flip)
        {
        case LamaPon::SpriteFlip::None:
            return DirectX::SpriteEffects_None;
        case LamaPon::SpriteFlip::Horizontal:
            return DirectX::SpriteEffects_FlipHorizontally;
        case LamaPon::SpriteFlip::Vertical:
            return DirectX::SpriteEffects_FlipVertically;
        case LamaPon::SpriteFlip::Both:
            return static_cast<DirectX::SpriteEffects>(
                DirectX::SpriteEffects_FlipHorizontally
                | DirectX::SpriteEffects_FlipVertically);
        default:
            throw std::invalid_argument(
                "The sprite flip mode is invalid.");
        }
    }

    [[nodiscard]] D3D11_RECT MakeScissorRectangle(
        const LamaPon::SpriteClipRectangle& rectangle,
        const std::vector<D3D11_RECT>& stack)
    {
        const auto clampLong = [](const float value) noexcept
        {
            return static_cast<LONG>(
                std::clamp(
                    static_cast<double>(value),
                    0.0,
                    static_cast<double>(
                        (std::numeric_limits<LONG>::max)())));
        };
        D3D11_RECT result{
            clampLong(rectangle.minimumX),
            clampLong(rectangle.minimumY),
            clampLong(rectangle.maximumX),
            clampLong(rectangle.maximumY) };
        if (!stack.empty())
        {
            const auto& outer = stack.back();
            result.left = std::max(result.left, outer.left);
            result.top = std::max(result.top, outer.top);
            result.right = std::min(result.right, outer.right);
            result.bottom = std::min(result.bottom, outer.bottom);
        }
        result.right = std::max(result.right, result.left);
        result.bottom = std::max(result.bottom, result.top);
        return result;
    }

    void RestartNativeSpriteBatch(
        SpriteResources& resources,
        ID3D11DeviceContext* const context,
        std::vector<D3D11_RECT> nextScissors)
    {
        try
        {
            resources.spriteBatch->End();
            resources.spriteBatchNativeBegun = false;
        }
        catch (...)
        {
            resources.spriteBatchNativeBegun = false;
            PoisonSpriteBatch(resources);
            throw;
        }

        ID3D11RasterizerState* rasterizer{};
        if (!nextScissors.empty())
        {
            context->RSSetScissorRects(
                1,
                &nextScissors.back());
            rasterizer = resources.uiScissorRasterizer.Get();
        }
        try
        {
            BeginNativeSpriteBatch(resources, rasterizer);
        }
        catch (...)
        {
            resources.spriteBatchNativeBegun = false;
            PoisonSpriteBatch(resources);
            throw;
        }
        resources.uiScissorStack.swap(nextScissors);
    }
}

namespace LamaPon
{
    DirectX::SpriteBatch& GraphicsDevice::BeginSprites()
    {
        const SpritePassDescription description;
        static_cast<void>(BeginD3D11SpritePass(
            description,
            false,
            nullptr,
            nullptr,
            nullptr));
        return *RequireD3D11ApiResources().spriteBatch;
    }

    void GraphicsDevice::EndSprites()
    {
        auto& resources = RequireD3D11ApiResources();
        RequireSpriteOwner(resources, SpriteOwner::Legacy, 0);
        try
        {
            resources.spriteBatch->End();
            resources.spriteBatchNativeBegun = false;
        }
        catch (...)
        {
            resources.spriteBatchNativeBegun = false;
            PoisonSpriteBatch(resources);
            throw;
        }
        ClearCompletedSpriteBatch(resources);
    }

    void GraphicsDevice::PushUIScissor(
        const float minimumX,
        const float minimumY,
        const float maximumX,
        const float maximumY)
    {
        auto& resources = RequireD3D11ApiResources();
        RequireSpriteOwner(resources, SpriteOwner::Legacy, 0);
        const SpriteClipRectangle rectangle{
            minimumX,
            minimumY,
            maximumX,
            maximumY };
        if (!IsFinite(rectangle))
        {
            throw std::invalid_argument(
                "A sprite scissor rectangle must be finite.");
        }
        auto nextScissors = resources.uiScissorStack;
        nextScissors.push_back(
            MakeScissorRectangle(rectangle, nextScissors));
        RestartNativeSpriteBatch(
            resources,
            Context(),
            std::move(nextScissors));
    }

    void GraphicsDevice::PopUIScissor()
    {
        auto& resources = RequireD3D11ApiResources();
        RequireSpriteOwner(resources, SpriteOwner::Legacy, 0);
        if (resources.uiScissorStack.empty())
        {
            return;
        }
        auto nextScissors = resources.uiScissorStack;
        nextScissors.pop_back();
        RestartNativeSpriteBatch(
            resources,
            Context(),
            std::move(nextScissors));
    }

    std::uint64_t GraphicsDevice::BeginD3D11SpritePass(
        const SpritePassDescription& description,
        const bool neutralOwner,
        SpriteShaderStatus* const status,
        std::uint64_t* const generation,
        std::string* const error)
    {
        auto& resources = RequireD3D11ApiResources();
        RequireAvailableSpriteBatch(resources);

        std::uint64_t token{};
        resources.spriteBatchOwner = neutralOwner
            ? SpriteOwner::Neutral
            : SpriteOwner::Legacy;
        if (neutralOwner)
        {
            token = resources.nextSpriteBatchToken++;
            if (token == 0)
            {
                token = resources.nextSpriteBatchToken++;
            }
        }
        resources.spriteBatchToken = token;

        bool nativeBeginAttempted{};
        try
        {
            SpriteShaderStatus preparedStatus;
            auto shaderCallback = PrepareD3D11SpriteShader(
                description,
                preparedStatus);
            auto* const blendState = ResolveSpriteBlendState(
                resources,
                description.blend);

            // Legacy output pointers may allocate while copying an error.
            // Finish every throwing preparation before SpriteBatch::Begin.
            if (generation != nullptr)
            {
                *generation = preparedStatus.generation;
            }
            if (error != nullptr)
            {
                *error = preparedStatus.error;
            }
            if (status != nullptr)
            {
                *status = std::move(preparedStatus);
            }

            resources.spriteTexturePins.clear();
            resources.spriteViewPins.clear();
            resources.uiScissorStack.clear();
            resources.spriteBlendState = blendState;
            resources.spriteShaderCallback =
                std::move(shaderCallback);

            nativeBeginAttempted = true;
            BeginNativeSpriteBatch(resources, nullptr);
        }
        catch (...)
        {
            if (nativeBeginAttempted)
            {
                PoisonSpriteBatch(resources);
            }
            else
            {
                RollBackSpriteReservation(resources);
            }
            throw;
        }
        return token;
    }

    bool GraphicsDevice::DrawD3D11Sprite(
        const std::uint64_t token,
        const SpriteDrawRequest& request)
    {
        auto& resources = RequireD3D11ApiResources();
        RequireSpriteOwner(
            resources,
            SpriteOwner::Neutral,
            token);

        if (!IsFinite(request.position)
            || !IsFinite(request.tint)
            || !std::isfinite(request.rotation)
            || !IsFinite(request.origin)
            || !IsFinite(request.scale)
            || !std::isfinite(request.layerDepth))
        {
            return false;
        }
        if (request.hasSourceRectangle
            && (request.sourceRectangle.right
                    <= request.sourceRectangle.left
                || request.sourceRectangle.bottom
                    <= request.sourceRectangle.top))
        {
            return false;
        }

        DirectX::SpriteEffects effects{};
        try
        {
            effects = ResolveSpriteEffects(request.flip);
        }
        catch (const std::invalid_argument&)
        {
            return false;
        }

        const auto& texture = request.texture
            ? request.texture
            : m_whiteTextureView;
        auto* const view =
            TryResolveD3D11ShaderResourceView(texture);
        if (view == nullptr)
        {
            return false;
        }

        // vectorの確保に失敗した場合はDrawを積みません。成功後は
        // scissorの内部flushをまたいでもouter pass終了まで保持します。
        resources.spriteViewPins.emplace_back(texture);
        RECT source{};
        const RECT* sourcePointer{};
        if (request.hasSourceRectangle)
        {
            source = {
                request.sourceRectangle.left,
                request.sourceRectangle.top,
                request.sourceRectangle.right,
                request.sourceRectangle.bottom
            };
            sourcePointer = &source;
        }
        try
        {
            resources.spriteBatch->Draw(
                view,
                request.position,
                sourcePointer,
                DirectX::XMLoadFloat4(&request.tint),
                request.rotation,
                request.origin,
                request.scale,
                effects,
                request.layerDepth);
        }
        catch (...)
        {
            // Draw may fail while DirectXTK still owns an active batch. Keep
            // that fact so SpriteRenderPass::Abort can attempt one safe End.
            PoisonSpriteBatch(resources);
            throw;
        }
        return true;
    }

    bool GraphicsDevice::PushD3D11SpriteScissor(
        const std::uint64_t token,
        const SpriteClipRectangle& rectangle)
    {
        auto& resources = RequireD3D11ApiResources();
        RequireSpriteOwner(
            resources,
            SpriteOwner::Neutral,
            token);
        if (!IsFinite(rectangle))
        {
            return false;
        }
        auto nextScissors = resources.uiScissorStack;
        nextScissors.push_back(
            MakeScissorRectangle(rectangle, nextScissors));
        RestartNativeSpriteBatch(
            resources,
            Context(),
            std::move(nextScissors));
        return true;
    }

    bool GraphicsDevice::PopD3D11SpriteScissor(
        const std::uint64_t token)
    {
        auto& resources = RequireD3D11ApiResources();
        RequireSpriteOwner(
            resources,
            SpriteOwner::Neutral,
            token);
        if (resources.uiScissorStack.empty())
        {
            return false;
        }
        auto nextScissors = resources.uiScissorStack;
        nextScissors.pop_back();
        RestartNativeSpriteBatch(
            resources,
            Context(),
            std::move(nextScissors));
        return true;
    }

    void GraphicsDevice::EndD3D11SpritePass(
        const std::uint64_t token)
    {
        auto& resources = RequireD3D11ApiResources();
        RequireSpriteOwner(
            resources,
            SpriteOwner::Neutral,
            token);
        try
        {
            resources.spriteBatch->End();
            resources.spriteBatchNativeBegun = false;
        }
        catch (...)
        {
            resources.spriteBatchNativeBegun = false;
            PoisonSpriteBatch(resources);
            throw;
        }
        ClearCompletedSpriteBatch(resources);
    }

    void GraphicsDevice::AbortD3D11SpritePass(
        const std::uint64_t token) noexcept
    {
        auto* const resources = TryD3D11ApiResources();
        if (resources == nullptr
            || token == 0
            || resources->spriteBatchToken != token
            || (resources->spriteBatchOwner != SpriteOwner::Neutral
                && resources->spriteBatchOwner
                    != SpriteOwner::Poisoned))
        {
            return;
        }

        const bool wasPoisoned =
            resources->spriteBatchOwner == SpriteOwner::Poisoned;
        if (wasPoisoned
            && !resources->spriteBatchNativeBegun)
        {
            // End自体が失敗した可能性があります。DirectXTKには
            // 再End/cancelの契約がないため、pinsごと再初期化まで保持します。
            return;
        }
        if (resources->spriteBatchNativeBegun)
        {
            try
            {
                resources->spriteBatch->End();
                resources->spriteBatchNativeBegun = false;
            }
            catch (...)
            {
                resources->spriteBatchNativeBegun = false;
                PoisonSpriteBatch(*resources);
                return;
            }
        }
        if (wasPoisoned)
        {
            resources->spriteShaderCallback = {};
            resources->spriteBlendState = nullptr;
            resources->uiScissorStack.clear();
            resources->spriteViewPins.clear();
            resources->spriteTexturePins.clear();
            return;
        }
        ClearCompletedSpriteBatch(*resources);
    }

    void GraphicsDevice::AbortActiveD3D11SpritePass() noexcept
    {
        auto* const resources = TryD3D11ApiResources();
        if (resources == nullptr)
        {
            return;
        }
        if (resources->spriteBatchOwner == SpriteOwner::Neutral
            || resources->spriteBatchOwner == SpriteOwner::Poisoned)
        {
            AbortD3D11SpritePass(resources->spriteBatchToken);
            return;
        }
        if (resources->spriteBatchOwner != SpriteOwner::Legacy
            || !resources->spriteBatchNativeBegun)
        {
            return;
        }
        try
        {
            resources->spriteBatch->End();
            resources->spriteBatchNativeBegun = false;
            ClearCompletedSpriteBatch(*resources);
        }
        catch (...)
        {
            resources->spriteBatchNativeBegun = false;
            PoisonSpriteBatch(*resources);
        }
    }

}
