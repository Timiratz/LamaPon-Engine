#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsDeviceState.h"

#include "LamaPon/Graphics/GraphicsDeviceD3D11Resources.h"

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

    // 合成方式をDirectXTKの状態へ変換します(resources: 状態を所有する資源, blend: 画像の合成方式)。
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

    // 終了したパスの画像参照と所有状態を解除します(resources: スプライト資源)。
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

    // DirectXTK開始前の予約を解除します(resources: スプライト資源)。
    void RollBackSpriteReservation(
        SpriteResources& resources) noexcept
    {
        resources.spriteShaderCallback = {};
        resources.spriteBlendState = nullptr;
        resources.spriteBatchNativeBegun = false;
        resources.spriteBatchToken = 0;
        resources.spriteBatchOwner = SpriteOwner::None;
    }

    // 再初期化までパスの開始を拒否します(resources: 失敗したスプライト資源)。
    void PoisonSpriteBatch(
        SpriteResources& resources) noexcept
    {
        // 終了失敗後は再利用せず、待機中画像のハンドルをバックエンド再初期化まで保持します。
        resources.spriteBatchOwner = SpriteOwner::Poisoned;
    }

    // 失敗状態や使用中ならlogic_errorです(resources: 確認するスプライト資源)。
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

    // 使用中パスの所有権を検証します(resources: 確認する資源, owner: 必須の所有形式, token: 必須のパス番号)。
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

    // 積んだ順に描くバッチを開始します(resources: 設定と所有状態, rasterizer: クリップ用で空なら既定)。
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

    // 2成分が全て有限か返します(value: 確認する座標)。
    [[nodiscard]] bool IsFinite(
        const DirectX::XMFLOAT2& value) noexcept
    {
        return std::isfinite(value.x)
            && std::isfinite(value.y);
    }

    // 4成分が全て有限か返します(value: 確認する色などの4値)。
    [[nodiscard]] bool IsFinite(
        const DirectX::XMFLOAT4& value) noexcept
    {
        return std::isfinite(value.x)
            && std::isfinite(value.y)
            && std::isfinite(value.z)
            && std::isfinite(value.w);
    }

    // 矩形の全座標が有限か返します(value: 確認するクリップ矩形)。
    [[nodiscard]] bool IsFinite(
        const LamaPon::SpriteClipRectangle& value) noexcept
    {
        return std::isfinite(value.minimumX)
            && std::isfinite(value.minimumY)
            && std::isfinite(value.maximumX)
            && std::isfinite(value.maximumY);
    }

    // 反転方式をDirectXTKの指定へ変換します(flip: 反転方向)。
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

    // 整数化して外側矩形との交差を返します(rectangle: 要求する画素座標の矩形, stack: 現在のクリップ列)。
    [[nodiscard]] D3D11_RECT MakeScissorRectangle(
        const LamaPon::SpriteClipRectangle& rectangle,
        const std::vector<D3D11_RECT>& stack)
    {
        // 0～LONG_MAXへ制限して整数化します(value: 画素座標)。
        const auto clampLong = [](const float value) noexcept
        {
            return static_cast<LONG>(
                std::clamp(
                    static_cast<double>(value),
                    0.0,
                    static_cast<double>(
                        (std::numeric_limits<LONG>::max)())));
        };
        // 交差・整数化したクリップ範囲
        D3D11_RECT result{
            clampLong(rectangle.minimumX),
            clampLong(rectangle.minimumY),
            clampLong(rectangle.maximumX),
            clampLong(rectangle.maximumY) };
        if (!stack.empty())
        {
            // 直前の外側クリップ範囲
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

    // 待機分を送信してクリップを切り替えます(resources: スプライト資源, context: 即時命令のコンテキスト, nextScissors: 成功後のクリップ列)。
    // Endまたは再Beginの失敗後は再初期化まで新規パスを拒否します。
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

        // クリップ有無に応じた描画状態
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
    // 互換用のD3D11バッチを開始して貸し出します。
    DirectX::SpriteBatch& GraphicsDevice::BeginSprites()
    {
        // 既定のスプライトパス設定
        const SpritePassDescription description;
        static_cast<void>(BeginD3D11SpritePass(
            description,
            false,
            nullptr,
            nullptr,
            nullptr));
        return *RequireD3D11ApiResources().spriteBatch;
    }

    // 互換用のD3D11バッチを送信して閉じます。
    void GraphicsDevice::EndSprites()
    {
        // 現在のD3D11スプライト資源
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

    // 互換用バッチへクリップを積みます(minimumX: 左端の画素位置, minimumY: 上端の画素位置, maximumX: 右端の画素位置, maximumY: 下端の画素位置)。
    void GraphicsDevice::PushUIScissor(
        const float minimumX,
        const float minimumY,
        const float maximumX,
        const float maximumY)
    {
        // 現在のD3D11スプライト資源
        auto& resources = RequireD3D11ApiResources();
        RequireSpriteOwner(resources, SpriteOwner::Legacy, 0);
        // 要求されたクリップ矩形
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
        // 成功後に確定するクリップ列
        auto nextScissors = resources.uiScissorStack;
        nextScissors.push_back(
            MakeScissorRectangle(rectangle, nextScissors));
        RestartNativeSpriteBatch(
            resources,
            Context(),
            std::move(nextScissors));
    }

    // 互換用バッチを直前のクリップ範囲へ戻します。
    void GraphicsDevice::PopUIScissor()
    {
        // 現在のD3D11スプライト資源
        auto& resources = RequireD3D11ApiResources();
        RequireSpriteOwner(resources, SpriteOwner::Legacy, 0);
        if (resources.uiScissorStack.empty())
        {
            return;
        }
        // 成功後に確定するクリップ列
        auto nextScissors = resources.uiScissorStack;
        nextScissors.pop_back();
        RestartNativeSpriteBatch(
            resources,
            Context(),
            std::move(nextScissors));
    }

    // D3D11パスを開始して番号を返します(description: 合成・シェーダー・定数, neutralOwner: 共通パスの所有形式, status: 使用状態の任意出力, generation: 世代番号の任意出力, error: 失敗文の任意出力)。
    std::uint64_t GraphicsDevice::BeginD3D11SpritePass(
        const SpritePassDescription& description,
        const bool neutralOwner,
        SpriteShaderStatus* const status,
        std::uint64_t* const generation,
        std::string* const error)
    {
        // 現在のD3D11スプライト資源
        auto& resources = RequireD3D11ApiResources();
        RequireAvailableSpriteBatch(resources);

        // 新規パスの識別番号
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

        // DirectXTKの開始試行済み
        bool nativeBeginAttempted{};
        try
        {
            // 準備済みシェーダーの使用状態
            SpriteShaderStatus preparedStatus;
            // 描画前にシェーダーを設定
            auto shaderCallback = PrepareD3D11SpriteShader(
                description,
                preparedStatus);
            // パスの合成状態
            auto* const blendState = ResolveSpriteBlendState(
                resources,
                description.blend);

            // 失敗文のコピーも例外を投げるため、Begin前に全ての準備を済ませます。
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

    // 有効な画像指定をパスへ積めたか返します(token: パス識別番号, request: 画像と位置・色・変形)。
    bool GraphicsDevice::DrawD3D11Sprite(
        const std::uint64_t token,
        const SpriteDrawRequest& request)
    {
        // 現在のD3D11スプライト資源
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

        // DirectXTKの反転指定
        DirectX::SpriteEffects effects{};
        try
        {
            effects = ResolveSpriteEffects(request.flip);
        }
        catch (const std::invalid_argument&)
        {
            return false;
        }

        // 保持して描画する入力ビュー
        const auto& texture = request.texture
            ? request.texture
            : m_state->m_whiteTextureView;
        // 入力画像のD3D11 SRV
        auto* const view =
            TryResolveD3D11ShaderResourceView(texture);
        if (view == nullptr)
        {
            return false;
        }

        // ハンドルの保持に成功してから描画を積み、クリップ切り替えをまたいでパス終了まで保持します。
        resources.spriteViewPins.emplace_back(texture);
        // 切り出し範囲の画素座標
        RECT source{};
        // 切り出し指定の有無
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
            // 描画失敗後も開始済み状態を保持し、Abortで一度だけ安全なEndを試みます。
            PoisonSpriteBatch(resources);
            throw;
        }
        return true;
    }

    // 共通パスへ有限の矩形を積めたか返します(token: パス識別番号, rectangle: ピクセル座標の矩形)。
    bool GraphicsDevice::PushD3D11SpriteScissor(
        const std::uint64_t token,
        const SpriteClipRectangle& rectangle)
    {
        // 現在のD3D11スプライト資源
        auto& resources = RequireD3D11ApiResources();
        RequireSpriteOwner(
            resources,
            SpriteOwner::Neutral,
            token);
        if (!IsFinite(rectangle))
        {
            return false;
        }
        // 成功後に確定するクリップ列
        auto nextScissors = resources.uiScissorStack;
        nextScissors.push_back(
            MakeScissorRectangle(rectangle, nextScissors));
        RestartNativeSpriteBatch(
            resources,
            Context(),
            std::move(nextScissors));
        return true;
    }

    // 共通パスを直前のクリップへ戻せたか返します(token: パス識別番号)。
    bool GraphicsDevice::PopD3D11SpriteScissor(
        const std::uint64_t token)
    {
        // 現在のD3D11スプライト資源
        auto& resources = RequireD3D11ApiResources();
        RequireSpriteOwner(
            resources,
            SpriteOwner::Neutral,
            token);
        if (resources.uiScissorStack.empty())
        {
            return false;
        }
        // 成功後に確定するクリップ列
        auto nextScissors = resources.uiScissorStack;
        nextScissors.pop_back();
        RestartNativeSpriteBatch(
            resources,
            Context(),
            std::move(nextScissors));
        return true;
    }

    // 共通パスを送信して閉じます(token: パス識別番号)。
    void GraphicsDevice::EndD3D11SpritePass(
        const std::uint64_t token)
    {
        // 現在のD3D11スプライト資源
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

    // 終了を試み、例外を外へ出しません(token: 共通パスの識別番号)。
    void GraphicsDevice::AbortD3D11SpritePass(
        const std::uint64_t token) noexcept
    {
        // 現在のD3D11スプライト資源
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

        // 失敗済みパスの終了処理
        const bool wasPoisoned =
            resources->spriteBatchOwner == SpriteOwner::Poisoned;
        if (wasPoisoned
            && !resources->spriteBatchNativeBegun)
        {
            // Endの再実行・キャンセルは保証されないため、画像参照ごと再初期化まで保持します。
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

    // 使用中の共通・互換用パスを例外なしで終了します。
    void GraphicsDevice::AbortActiveD3D11SpritePass() noexcept
    {
        // 現在のD3D11スプライト資源
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
