#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsDeviceState.h"

#include "LamaPon/Graphics/EnvironmentSettings.h"
#include "LamaPon/Graphics/GpuProfiler.h"
#include "LamaPon/Graphics/GraphicsBackend.h"
#include "LamaPon/Graphics/RenderPipeline.h"
#include "LamaPon/Graphics/RenderTarget.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace LamaPon
{
    void GraphicsDevice::ResizeOffscreenTarget(
        RenderTarget& target,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "ResizeOffscreenTarget requires an initialized device.");
        }

        m_state->m_backend->ResizeOffscreenTarget(target, width, height);
        if (!target.IsValid()
            || !IsGraphicsViewCurrent(
                target.CurrentColorViewHandle())
            || !IsGraphicsViewCurrent(
                target.DisplayViewHandle()))
        {
            throw std::logic_error(
                "ResizeOffscreenTarget failed to publish current color "
                "and display views.");
        }
    }

    void GraphicsDevice::BeginOffscreenTarget(
        RenderTarget& target,
        const float clearColor[4])
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "BeginOffscreenTarget requires an initialized device.");
        }
        if (clearColor == nullptr)
        {
            throw std::invalid_argument(
                "BeginOffscreenTarget requires a clear color.");
        }
        if (!target.IsValid())
        {
            throw std::invalid_argument(
                "BeginOffscreenTarget requires a valid target.");
        }

        m_state->m_backend->BeginOffscreenTarget(target, clearColor);
    }

    void GraphicsDevice::BindOffscreenTarget(
        RenderTarget& target)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "BindOffscreenTarget requires an initialized device.");
        }
        if (!target.IsValid())
        {
            throw std::invalid_argument(
                "BindOffscreenTarget requires a valid target.");
        }

        m_state->m_backend->BindOffscreenTarget(
            target);
    }

    void GraphicsDevice::PublishOffscreenTarget(
        RenderTarget& target)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "PublishOffscreenTarget requires an initialized device.");
        }
        if (!target.IsValid())
        {
            throw std::invalid_argument(
                "PublishOffscreenTarget requires a valid target.");
        }
        if (!IsGraphicsViewCurrent(
                target.CurrentColorViewHandle())
            || !IsGraphicsViewCurrent(
                target.DisplayViewHandle()))
        {
            throw std::invalid_argument(
                "PublishOffscreenTarget requires a target owned by the "
                "active backend.");
        }

        // CopyToDisplayは描画先を変更しません。バックバッファへの復帰は
        // BeginFrameなど、既存のフレーム制御側に任せます。
        m_state->m_backend->PublishOffscreenTarget(
            target);
    }

    void GraphicsDevice::BindOffscreenTargetDepthOnly(
        RenderTarget& target)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "BindOffscreenTargetDepthOnly requires an initialized "
                "device.");
        }
        if (!target.IsValid())
        {
            throw std::invalid_argument(
                "BindOffscreenTargetDepthOnly requires a valid target.");
        }

        m_state->m_backend->BindOffscreenTargetDepthOnly(target);
    }

    void GraphicsDevice::CaptureOffscreenTargetDepth(
        RenderTarget& target)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CaptureOffscreenTargetDepth requires an initialized "
                "device.");
        }
        if (!target.IsValid())
        {
            throw std::invalid_argument(
                "CaptureOffscreenTargetDepth requires a valid target.");
        }

        m_state->m_backend->CaptureOffscreenTargetDepth(target);
    }

    void GraphicsDevice::CaptureOffscreenTargetColorHistory(
        RenderTarget& target,
        const DirectX::XMFLOAT4X4& viewProjection)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CaptureOffscreenTargetColorHistory requires an "
                "initialized device.");
        }
        if (!target.IsValid())
        {
            throw std::invalid_argument(
                "CaptureOffscreenTargetColorHistory requires a valid "
                "target.");
        }

        m_state->m_backend->CaptureOffscreenTargetColorHistory(
            target,
            viewProjection);
    }

    void GraphicsDevice::CaptureOffscreenTargetTemporalHistory(
        RenderTarget& target,
        const DirectX::XMFLOAT4X4& viewProjection)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CaptureOffscreenTargetTemporalHistory requires an "
                "initialized device.");
        }
        if (!target.IsValid())
        {
            throw std::invalid_argument(
                "CaptureOffscreenTargetTemporalHistory requires a valid "
                "target.");
        }

        m_state->m_backend->CaptureOffscreenTargetTemporalHistory(
            target,
            viewProjection);
    }

    RenderTarget& GraphicsDevice::AcquireRenderTexture(
        const std::string& name,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        const std::uint32_t safeWidth =
            width == 0 ? 1 : width;
        const std::uint32_t safeHeight =
            height == 0 ? 1 : height;

        auto& slot = m_state->m_renderTextures[name];
        const bool createdTarget = !slot;
        try
        {
            if (!slot)
            {
                slot = std::make_unique<RenderTarget>();
            }
            // Resizeは同じサイズなら何もしません（作り直しの判定は
            // RenderTarget側が持っています）。
            ResizeOffscreenTarget(
                *slot,
                safeWidth,
                safeHeight);
            return *slot;
        }
        catch (...)
        {
            if (createdTarget)
            {
                m_state->m_renderTextures.erase(name);
            }
            throw;
        }
    }

    RenderTarget& GraphicsDevice::AcquireComputeTexture(
        const std::string& name,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        const std::uint32_t safeWidth =
            width == 0 ? 1 : width;
        const std::uint32_t safeHeight =
            height == 0 ? 1 : height;

        auto& slot = m_state->m_renderTextures[name];
        const bool createdTarget = !slot;
        try
        {
            if (!slot)
            {
                slot = std::make_unique<RenderTarget>();
            }
            // UAVのバインドフラグは作成時にしか決められないので、
            // Resizeより前に印を付けます。カメラの描画先として先に
            // 既存の同名テクスチャには作成フラグを追加できないため、
            // カメラ描画先とは異なる名前を使用します。
            slot->SetComputeWritable(true);
            ResizeOffscreenTarget(
                *slot,
                safeWidth,
                safeHeight);
            return *slot;
        }
        catch (...)
        {
            if (createdTarget)
            {
                m_state->m_renderTextures.erase(name);
            }
            throw;
        }
    }

    const RenderTarget* GraphicsDevice::FindRenderTexture(
        const std::string& name) const noexcept
    {
        const auto entry = m_state->m_renderTextures.find(name);
        if (entry == m_state->m_renderTextures.end())
        {
            return nullptr;
        }
        return entry->second.get();
    }

    GraphicsViewHandle GraphicsDevice::RenderTextureViewHandle(
        const std::string& name) const noexcept
    {
        const auto* const target = FindRenderTexture(name);
        if (target == nullptr || !target->IsValid())
        {
            return {};
        }
        auto view = target->DisplayViewHandle();
        return IsGraphicsViewCurrent(view)
            ? view
            : GraphicsViewHandle{};
    }

    bool GraphicsDevice::ReleaseRenderTexture(
        const std::string& name)
    {
        return m_state->m_renderTextures.erase(name) > 0;
    }

    void GraphicsDevice::ClearRenderTextures() noexcept
    {
        m_state->m_renderTextures.clear();
    }

    std::vector<std::string>
        GraphicsDevice::RenderTextureNames() const
    {
        std::vector<std::string> names;
        names.reserve(m_state->m_renderTextures.size());
        for (const auto& [name, target] : m_state->m_renderTextures)
        {
            static_cast<void>(target);
            names.push_back(name);
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    void GraphicsDevice::BeginSceneComposition(
        const float clearColor[4])
    {
        ResizeOffscreenTarget(
            *m_state->m_sceneCompositionTarget,
            RenderWidth(),
            RenderHeight());
        BeginOffscreenTarget(
            *m_state->m_sceneCompositionTarget,
            clearColor);
    }

    void GraphicsDevice::EndSceneComposition(
        const BloomSettings& bloom,
        const ColorGradingSettings& colorGrading)
    {
        EndSceneComposition(
            bloom,
            ScreenSpaceLensFlareSettings{},
            colorGrading,
            VolumetricLightFrame{},
            TemporalAntiAliasingFrame{});
    }

    void GraphicsDevice::EndSceneComposition(
        const BloomSettings& bloom,
        const ScreenSpaceLensFlareSettings& lensFlare,
        const ColorGradingSettings& colorGrading)
    {
        EndSceneComposition(
            bloom,
            lensFlare,
            colorGrading,
            VolumetricLightFrame{},
            TemporalAntiAliasingFrame{});
    }

    void GraphicsDevice::EndSceneComposition(
        const BloomSettings& bloom,
        const ColorGradingSettings& colorGrading,
        const VolumetricLightFrame& volumetric,
        const TemporalAntiAliasingFrame& temporal)
    {
        EndSceneComposition(
            bloom,
            ScreenSpaceLensFlareSettings{},
            colorGrading,
            volumetric,
            temporal);
    }

    void GraphicsDevice::EndSceneComposition(
        const BloomSettings& bloom,
        const ScreenSpaceLensFlareSettings& lensFlare,
        const ColorGradingSettings& colorGrading,
        const VolumetricLightFrame& volumetric,
        const TemporalAntiAliasingFrame& temporal)
    {
        PostProcessFrame frame{};
        frame.bloom = bloom;
        frame.lensFlare = lensFlare;
        frame.colorGrading = colorGrading;
        frame.volumetric = volumetric;
        frame.temporal = temporal;
        EndSceneComposition(frame);
    }

    void GraphicsDevice::EndSceneComposition(
        const PostProcessFrame& frame)
    {
        // RunPostProcessが計測区間を開始するため、呼び出し側では開始しません。
        RunPostProcess(
            *this,
            *m_state->m_sceneCompositionTarget,
            frame,
            [this](
                RenderTarget& target,
                const ScreenEffectPoint point)
            {
                ApplyQueuedScreenEffects(target, point);
            });
        GpuProfiler::SectionScope transferSection{
            m_state->m_gpuProfiler,
            "画面へ転送"
        };
        // current colorのnative view解決はD3D11 bridge内へ閉じ込め、
        // 共通compositionはRenderTargetだけを渡します。
        CopyOffscreenTargetToBackBuffer(
            *m_state->m_sceneCompositionTarget);
    }
}
