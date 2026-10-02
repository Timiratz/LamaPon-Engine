#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsDeviceState.h"

#include "LamaPon/Graphics/AutoExposureAdaptation.h"
#include "LamaPon/Graphics/D3D11RenderTargetState.h"
#include "LamaPon/Graphics/D3D12Backend.h"
#include "LamaPon/Graphics/D3D12SpriteRenderer.h"
#include "LamaPon/Graphics/EnvironmentRenderer.h"
#include "LamaPon/Graphics/EnvironmentSettings.h"
#include "LamaPon/Graphics/GraphicsBackend.h"
#include "LamaPon/Graphics/GraphicsDeviceD3D12Resources.h"
#include "LamaPon/Graphics/RenderTarget.h"

#include <optional>
#include <stdexcept>
#include <string>

namespace
{
    // 現在のD3D11描画先を検証して借用します(graphics: 使用するデバイス, target: 検証する描画先, operation: 失敗文に付ける操作名)。
    [[nodiscard]] const LamaPon::Detail::D3D11RenderTargetState&
        RequireCurrentOffscreenTarget(
        const LamaPon::GraphicsDevice& graphics,
        const LamaPon::RenderTarget& target,
        const char* const operation)
    {
        if (!graphics.IsInitialized())
        {
            throw std::logic_error(
                std::string(operation)
                + " requires an initialized device.");
        }
        if (!target.IsValid()
            || !graphics.IsGraphicsViewCurrent(
                target.CurrentColorViewHandle())
            || !graphics.IsGraphicsViewCurrent(
                target.DepthViewHandle()))
        {
            throw std::invalid_argument(
                std::string(operation)
                + " requires a target owned by the active backend.");
        }

        // 現在の描画先の内部状態
        const auto* const state = dynamic_cast<const
            LamaPon::Detail::D3D11RenderTargetState*>(
                LamaPon::Detail::RenderTargetBackendAccess::Get(target));
        if (state == nullptr || !state->IsValid())
        {
            throw std::invalid_argument(
                std::string(operation)
                + " requires a target owned by the active backend.");
        }
        return *state;
    }

    // 現在のD3D11描画先を検証して変更可能に借用します(graphics: 使用するデバイス, target: 検証する描画先, operation: 失敗文に付ける操作名)。
    [[nodiscard]] LamaPon::Detail::D3D11RenderTargetState&
        RequireCurrentOffscreenTarget(
            const LamaPon::GraphicsDevice& graphics,
            LamaPon::RenderTarget& target,
            const char* const operation)
    {
        static_cast<void>(RequireCurrentOffscreenTarget(
            graphics,
            static_cast<const LamaPon::RenderTarget&>(target),
            operation));
        // 現在の描画先の内部状態
        auto* const state = dynamic_cast<
            LamaPon::Detail::D3D11RenderTargetState*>(
                LamaPon::Detail::RenderTargetBackendAccess::Get(target));
        if (state == nullptr || !state->IsValid())
        {
            throw std::invalid_argument(
                std::string(operation)
                + " requires a target owned by the active backend.");
        }
        return *state;
    }

    // D3D11のTAA入力へ変換します(source: API共通の行列入力)。
    [[nodiscard]] LamaPon::EnvironmentRenderer::TemporalInputs
        ToD3D11TemporalInputs(
            const LamaPon::TemporalAntiAliasingInputs& source)
    {
        // 変換した後処理の入力
        LamaPon::EnvironmentRenderer::TemporalInputs result{};
        result.inverseViewProjection = source.inverseViewProjection;
        result.viewProjection = source.viewProjection;
        return result;
    }

    // D3D11の光の筋の入力へ変換します(source: API共通のカメラ・光源・影情報)。
    [[nodiscard]] LamaPon::EnvironmentRenderer::VolumetricInputs
        ToD3D11VolumetricInputs(
            const LamaPon::VolumetricLightInputs& source)
    {
        // 変換した後処理の入力
        LamaPon::EnvironmentRenderer::VolumetricInputs result{};
        result.cascadeShadow = source.cascadeShadow;
        result.inverseViewProjection = source.inverseViewProjection;
        result.cameraPosition = source.cameraPosition;
        result.lightDirection = source.lightDirection;
        result.lightColor = source.lightColor;
        result.cascadeViewProjections = source.cascadeViewProjections;
        result.cascadeCount = source.cascadeCount;
        result.shadowBias = source.shadowBias;
        result.shadowResolution = source.shadowResolution;
        return result;
    }


    // 描画先を検証してD3D12後処理器を借用します(graphics: 使用するデバイス, resources: 現在のAPI資源, target: 検証する描画先, operation: 失敗文に付ける操作名)。
    [[nodiscard]] LamaPon::Detail::D3D12SpriteRenderer&
        RequireD3D12PostProcessRenderer(
            const LamaPon::GraphicsDevice& graphics,
            LamaPon::Detail::GraphicsDeviceApiResources* const resources,
            const LamaPon::RenderTarget& target,
            const char* const operation)
    {
        if (!graphics.IsInitialized())
        {
            throw std::logic_error(
                std::string(operation)
                + " requires an initialized device.");
        }
        if (!target.IsValid()
            || !graphics.IsGraphicsViewCurrent(
                target.CurrentColorViewHandle())
            || !graphics.IsGraphicsViewCurrent(
                target.DepthViewHandle()))
        {
            throw std::invalid_argument(
                std::string(operation)
                + " requires a target owned by the active backend.");
        }
        // 現在のD3D12所有資源
        auto* const d3d12 = dynamic_cast<
            LamaPon::Detail::GraphicsDeviceD3D12Resources*>(resources);
        // 借用するD3D12後処理器
        auto* const renderer = d3d12 != nullptr
            ? d3d12->TrySpriteRenderer()
            : nullptr;
        if (renderer == nullptr)
        {
            throw std::logic_error(
                "The DirectX 12 post-process renderer is not "
                "initialized.");
        }
        return *renderer;
    }
}

namespace LamaPon
{
    // 処理済みの色をバックバッファ全体へ転写します(target: 現在の資源世代の描画先)。
    void GraphicsDevice::CopyOffscreenTargetToBackBuffer(
        const RenderTarget& target)
    {
        if (ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            if (!target.IsValid()
                || !IsGraphicsViewCurrent(
                    target.CurrentColorViewHandle())
                || !IsGraphicsViewCurrent(
                    target.DisplayViewHandle()))
            {
                throw std::invalid_argument(
                    "CopyOffscreenTargetToBackBuffer requires a current "
                    "DirectX 12 target.");
            }

            // 後処理済みの色を表示画像へ確定し、バックバッファ全体へ転写します。
            // 表示画像を確定する描画先
            auto& mutableTarget = const_cast<RenderTarget&>(target);
            m_state->m_backend->PublishOffscreenTarget(mutableTarget);
            m_state->m_backend->BindBackBuffer();
            // 現在のD3D12所有資源
            auto* const resources = dynamic_cast<
                Detail::GraphicsDeviceD3D12Resources*>(
                    m_state->m_apiResources.get());
            // 借用するD3D12後処理器
            auto* const renderer = resources != nullptr
                ? resources->TrySpriteRenderer()
                : nullptr;
            if (renderer == nullptr)
            {
                throw std::logic_error(
                    "The DirectX 12 scene compositor is not initialized.");
            }
            renderer->CompositeScene(
                target.DisplayViewHandle(),
                m_state->m_whiteTextureView);
            return;
        }

        static_cast<void>(RequireCurrentOffscreenTarget(
            *this,
            target,
            "CopyOffscreenTargetToBackBuffer"));
        // D3D11の環境描画器
        auto& environment = Environment();
        // 転写元のD3D11カラーSRV
        auto* const source = TryResolveD3D11ShaderResourceView(
            target.CurrentColorViewHandle());
        if (source == nullptr)
        {
            throw std::invalid_argument(
                "CopyOffscreenTargetToBackBuffer requires a resolvable "
                "current color view.");
        }

        m_state->m_backend->BindBackBuffer();
        environment.CopyToBoundRenderTarget(source);
    }

    // 深度から遮蔽率を生成したか返します(target: 深度入力と遮蔽率の出力先, settings: 遮蔽の設定, projection: 深度復元の射影行列, sampleCount: 採取点数)。
    bool GraphicsDevice::ResolveOffscreenTargetAmbientOcclusion(
        RenderTarget& target,
        const AmbientOcclusionSettings& settings,
        const DirectX::XMFLOAT4X4& projection,
        const std::uint32_t sampleCount)
    {
        if (ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            // 遮蔽やSSRが読む深度コピーをプリパス完了後に確定します。
            m_state->m_backend->CaptureOffscreenTargetDepth(target);
            if (!settings.enabled)
            {
                return false;
            }
            return RequireD3D12PostProcessRenderer(
                *this,
                m_state->m_apiResources.get(),
                target,
                "ResolveOffscreenTargetAmbientOcclusion")
                .ResolveAmbientOcclusion(
                    target,
                    m_state->m_whiteTextureView,
                    settings,
                    projection,
                    sampleCount);
        }
        // 現在のD3D11描画先状態
        auto& targetState = RequireCurrentOffscreenTarget(
            *this,
            target,
            "ResolveOffscreenTargetAmbientOcclusion");
        if (!settings.enabled)
        {
            return false;
        }
        return targetState.ResolveAmbientOcclusion(
            Environment(),
            settings,
            projection,
            sampleCount);
    }

    // 再投影した履歴を混ぜます(target: 処理する描画先, settings: TAAの設定, inputs: 履歴と再投影行列)。
    void GraphicsDevice::ApplyOffscreenTargetTemporalAntiAliasing(
        RenderTarget& target,
        const TemporalAntiAliasingSettings& settings,
        const TemporalAntiAliasingInputs& inputs)
    {
        if (ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            if (!settings.enabled)
            {
                return;
            }
            // TAAが読む深度コピーを主描画の完了後に確定します。
            m_state->m_backend->CaptureOffscreenTargetDepth(target);
            RequireD3D12PostProcessRenderer(
                *this,
                m_state->m_apiResources.get(),
                target,
                "ApplyOffscreenTargetTemporalAntiAliasing")
                .ApplyTemporalAntiAliasing(
                    target,
                    m_state->m_whiteTextureView,
                    settings,
                    inputs);
            return;
        }

        // 現在のD3D11描画先状態
        auto& targetState = RequireCurrentOffscreenTarget(
            *this,
            target,
            "ApplyOffscreenTargetTemporalAntiAliasing");
        if (!settings.enabled)
        {
            return;
        }
        targetState.ApplyTemporalAntiAliasing(
            Environment(),
            settings,
            ToD3D11TemporalInputs(inputs));
    }

    // 影から光の筋を合成します(target: HDRの描画先, settings: 光の筋の設定, inputs: カメラと光源・影情報)。
    void GraphicsDevice::ApplyOffscreenTargetVolumetricLight(
        RenderTarget& target,
        const VolumetricLightSettings& settings,
        const VolumetricLightInputs& inputs)
    {
        if (ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            RequireD3D12PostProcessRenderer(
                *this,
                m_state->m_apiResources.get(),
                target,
                "ApplyOffscreenTargetVolumetricLight")
                .ApplyVolumetricLight(
                    target,
                    m_state->m_whiteTextureView,
                    settings,
                    inputs);
            return;
        }

        // 現在のD3D11描画先状態
        auto& targetState = RequireCurrentOffscreenTarget(
            *this,
            target,
            "ApplyOffscreenTargetVolumetricLight");
        if (!settings.enabled)
        {
            return;
        }
        targetState.ApplyVolumetricLight(
            Environment(),
            settings,
            ToD3D11VolumetricInputs(inputs));
    }

    // 焦点帯の外を深度に応じてぼかします(target: HDRの描画先, settings: 被写界深度の設定, projection: 深度復元の射影行列, sampleCount: 採取点数)。
    void GraphicsDevice::ApplyOffscreenTargetDepthOfField(
        RenderTarget& target,
        const DepthOfFieldSettings& settings,
        const DirectX::XMFLOAT4X4& projection,
        const std::uint32_t sampleCount)
    {
        if (ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            // 借用するD3D12後処理器
            auto& renderer = RequireD3D12PostProcessRenderer(
                *this,
                m_state->m_apiResources.get(),
                target,
                "ApplyOffscreenTargetDepthOfField");
            if (!settings.enabled)
            {
                return;
            }
            m_state->m_backend->CaptureOffscreenTargetDepth(target);
            renderer.ApplyDepthOfField(
                target,
                m_state->m_whiteTextureView,
                settings,
                projection,
                sampleCount);
            return;
        }

        // 現在のD3D11描画先状態
        auto& targetState = RequireCurrentOffscreenTarget(
            *this,
            target,
            "ApplyOffscreenTargetDepthOfField");
        if (!settings.enabled)
        {
            return;
        }
        targetState.ApplyDepthOfField(
            Environment(),
            settings,
            projection,
            sampleCount);
    }

    // 前フレームとの差に沿って色を平均します(target: HDRの描画先, settings: 動きぼかしの設定, inverseViewProjection: 世界座標を復元する逆行列, viewProjection: 保存する現在の射影行列, sampleCount: 採取点数)。
    void GraphicsDevice::ApplyOffscreenTargetMotionBlur(
        RenderTarget& target,
        const MotionBlurSettings& settings,
        const DirectX::XMFLOAT4X4& inverseViewProjection,
        const DirectX::XMFLOAT4X4& viewProjection,
        const std::uint32_t sampleCount)
    {
        if (ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            // 借用するD3D12後処理器
            auto& renderer = RequireD3D12PostProcessRenderer(
                *this,
                m_state->m_apiResources.get(),
                target,
                "ApplyOffscreenTargetMotionBlur");
            // 現在の描画先の内部状態
            auto* const state =
                Detail::RenderTargetBackendAccess::Get(target);
            if (state == nullptr)
            {
                throw std::logic_error(
                    "The DirectX 12 motion blur target is not "
                    "initialized.");
            }
            if (!settings.enabled)
            {
                // 無効中のカメラ移動を再有効化時へ持ち越さないよう履歴を無効にします。
                state->m_motionBlurPreviousValid = false;
                return;
            }
            if (state->m_motionBlurPreviousValid
                && settings.intensity > 0.0f
                && settings.maximumRadius > 0.0f)
            {
                m_state->m_backend->CaptureOffscreenTargetDepth(target);
                renderer.ApplyMotionBlur(
                    target,
                    m_state->m_whiteTextureView,
                    settings,
                    inverseViewProjection,
                    state->m_motionBlurPreviousViewProjection,
                    sampleCount);
            }
            // 初回はぼかさず、次回の比較行列を描画先ごとに保存します。
            state->m_motionBlurPreviousViewProjection = viewProjection;
            state->m_motionBlurPreviousValid = true;
            return;
        }

        // 現在のD3D11描画先状態
        auto& targetState = RequireCurrentOffscreenTarget(
            *this,
            target,
            "ApplyOffscreenTargetMotionBlur");
        // 無効時も処理を渡して履歴を無効にし、再有効化時の大きなブレを防ぎます。
        targetState.ApplyMotionBlur(
            Environment(),
            settings,
            inverseViewProjection,
            viewProjection,
            sampleCount);
    }

    // 高輝度部のぼかしを適用します(target: HDRの描画先, settings: ブルームの設定)。
    void GraphicsDevice::ApplyOffscreenTargetBloom(
        RenderTarget& target,
        const BloomSettings& settings)
    {
        if (ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            RequireD3D12PostProcessRenderer(
                *this,
                m_state->m_apiResources.get(),
                target,
                "ApplyOffscreenTargetBloom").ApplyBloom(
                    target,
                    m_state->m_whiteTextureView,
                    settings);
            return;
        }

        // 現在のD3D11描画先状態
        auto& targetState = RequireCurrentOffscreenTarget(
            *this,
            target,
            "ApplyOffscreenTargetBloom");
        if (!settings.enabled)
        {
            return;
        }
        targetState.ApplyBloom(Environment(), settings);
    }

    // 光条・ゴースト・ハローを合成します(target: HDRの描画先, settings: レンズフレアの設定)。
    void GraphicsDevice::ApplyOffscreenTargetScreenSpaceLensFlare(
        RenderTarget& target,
        const ScreenSpaceLensFlareSettings& settings)
    {
        if (ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            RequireD3D12PostProcessRenderer(
                *this,
                m_state->m_apiResources.get(),
                target,
                "ApplyOffscreenTargetScreenSpaceLensFlare")
                .ApplyScreenSpaceLensFlare(
                    target,
                    m_state->m_whiteTextureView,
                    settings);
            return;
        }

        // 現在のD3D11描画先状態
        auto& targetState = RequireCurrentOffscreenTarget(
            *this,
            target,
            "ApplyOffscreenTargetScreenSpaceLensFlare");
        if (!settings.enabled)
        {
            return;
        }
        targetState.ApplyScreenSpaceLensFlare(
            Environment(),
            settings);
    }

    // ACES近似と色調補正を適用します(target: HDRの描画先, settings: 色調補正の設定)。
    void GraphicsDevice::ApplyOffscreenTargetToneMapping(
        RenderTarget& target,
        const ColorGradingSettings& settings)
    {
        if (ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            RequireD3D12PostProcessRenderer(
                *this,
                m_state->m_apiResources.get(),
                target,
                "ApplyOffscreenTargetToneMapping").ApplyToneMapping(
                    target,
                    m_state->m_whiteTextureView,
                    settings);
            return;
        }

        // 現在のD3D11描画先状態
        auto& targetState = RequireCurrentOffscreenTarget(
            *this,
            target,
            "ApplyOffscreenTargetToneMapping");
        targetState.ApplyToneMapping(Environment(), settings);
    }

    // 深度と法線から輪郭を合成します(target: 処理する描画先, settings: 輪郭の設定, projection: 深度復元の射影行列)。
    void GraphicsDevice::ApplyOffscreenTargetScreenOutline(
        RenderTarget& target,
        const ScreenOutlineSettings& settings,
        const DirectX::XMFLOAT4X4& projection)
    {
        if (ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            if (!settings.enabled)
            {
                return;
            }
            // 輪郭用の最終深度コピーはTAAの有効状態によらず確定します。
            m_state->m_backend->CaptureOffscreenTargetDepth(target);
            RequireD3D12PostProcessRenderer(
                *this,
                m_state->m_apiResources.get(),
                target,
                "ApplyOffscreenTargetScreenOutline")
                .ApplyScreenOutline(
                    target,
                    m_state->m_whiteTextureView,
                    settings,
                    projection);
            return;
        }

        // 現在のD3D11描画先状態
        auto& targetState = RequireCurrentOffscreenTarget(
            *this,
            target,
            "ApplyOffscreenTargetScreenOutline");
        if (!settings.enabled)
        {
            return;
        }
        targetState.ApplyScreenOutline(
            Environment(),
            settings,
            projection);
    }

    // 輝度の縁を検出して平滑化します(target: 処理する描画先)。
    void GraphicsDevice::ApplyOffscreenTargetFXAA(
        RenderTarget& target)
    {
        if (ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            RequireD3D12PostProcessRenderer(
                *this,
                m_state->m_apiResources.get(),
                target,
                "ApplyOffscreenTargetFXAA").ApplyFXAA(
                    target,
                    m_state->m_whiteTextureView);
            return;
        }

        // 現在のD3D11描画先状態
        auto& targetState = RequireCurrentOffscreenTarget(
            *this,
            target,
            "ApplyOffscreenTargetFXAA");
        targetState.ApplyFXAA(Environment());
    }

    // 輝度へ順応した露出補正EVを返します(target: HDR入力と測定値の保存先, settings: 自動露出の設定, deltaSeconds: 前回からの経過秒)。
    float GraphicsDevice::UpdateOffscreenTargetAutoExposure(
        RenderTarget& target,
        const AutoExposureSettings& settings,
        const float deltaSeconds)
    {
        if (ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            // 借用するD3D12後処理器
            auto& renderer = RequireD3D12PostProcessRenderer(
                *this,
                m_state->m_apiResources.get(),
                target,
                "UpdateOffscreenTargetAutoExposure");
            // 現在のD3D12バックエンド
            auto* const backend = dynamic_cast<D3D12Backend*>(
                m_state->m_backend.get());
            // 現在の描画先の内部状態
            auto* const state =
                Detail::RenderTargetBackendAccess::Get(target);
            if (backend == nullptr || state == nullptr)
            {
                throw std::logic_error(
                    "The DirectX 12 auto exposure backend is not "
                    "initialized.");
            }
            if (!settings.enabled)
            {
                // 無効時は未読の測定値と順応状態を破棄します。
                backend->DiscardOffscreenTargetLuminance(target);
                Detail::ResetAutoExposure(*state);
                return 0.0f;
            }
            // 前フレーム値で順応した後、現在の輝度を測定して次回の転送を予約します。
            // 順応後の露出補正（EV）
            const float exposureStops = Detail::AdvanceAutoExposure(
                *state,
                backend->TryReadOffscreenTargetLuminance(target),
                settings,
                deltaSeconds);
            renderer.MeasureLuminance(
                target,
                m_state->m_whiteTextureView);
            backend->CaptureOffscreenTargetLuminance(target);
            return exposureStops;
        }

        // 現在のD3D11描画先状態
        auto& targetState = RequireCurrentOffscreenTarget(
            *this,
            target,
            "UpdateOffscreenTargetAutoExposure");

        // 読み戻せた線形平均輝度
        std::optional<float> measuredLuminance;
        if (settings.enabled)
        {
            measuredLuminance =
                m_state->m_backend->TryReadOffscreenTargetLuminance(target);
        }

        // 順応後の露出補正（EV）
        const float exposureStops = targetState.UpdateAutoExposure(
            Environment(),
            measuredLuminance,
            settings,
            deltaSeconds);

        if (settings.enabled)
        {
            m_state->m_backend->CaptureOffscreenTargetLuminance(target);
        }
        return exposureStops;
    }
}
