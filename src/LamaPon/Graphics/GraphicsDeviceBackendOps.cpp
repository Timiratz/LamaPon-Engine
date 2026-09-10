#include "LamaPon/Graphics/GraphicsDevice.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Graphics/ClusteredLights.h"
#include "LamaPon/Graphics/EnvironmentSettings.h"
#include "LamaPon/Graphics/GpuProfiler.h"
#include "LamaPon/Graphics/GraphicsBackend.h"
#include "LamaPon/Graphics/Lighting.h"
#include "LamaPon/Graphics/ShadowMap.h"

#include <DirectXMath.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace LamaPon
{
    GraphicsTextureHandle GraphicsDevice::CreateTexture2D(
        const GraphicsTexture2DDescription& description,
        const std::span<const GraphicsTextureSubresourceData>
            initialData)
    {
        if (m_backend == nullptr)
        {
            throw std::logic_error(
                "CreateTexture2D requires an initialized graphics backend.");
        }
        return m_backend->CreateTexture2D(
            description,
            initialData);
    }

    GraphicsTextureHandle GraphicsDevice::CreateTexture3D(
        const GraphicsTexture3DDescription& description,
        const std::span<const GraphicsTextureSubresourceData>
            initialData)
    {
        if (m_backend == nullptr)
        {
            throw std::logic_error(
                "CreateTexture3D requires an initialized graphics backend.");
        }
        return m_backend->CreateTexture3D(
            description,
            initialData);
    }

    std::array<GraphicsViewHandle, 3>
        GraphicsDevice::UploadBakedGlobalIlluminationViews(
            const std::uint32_t width,
            const std::uint32_t height,
            const std::uint32_t depth,
            const std::span<const std::uint16_t> coefficients)
            const noexcept
    {
        std::array<GraphicsViewHandle, 3> createdViews;
        const auto probeCount =
            BakedGlobalIlluminationProbeCount(
                width,
                height,
                depth);
        if (m_backend == nullptr
            || !probeCount.has_value()
            || coefficients.size()
                != *probeCount
                    * BakedGlobalIlluminationCoefficientsPerProbe)
        {
            return createdViews;
        }

        try
        {
            const GraphicsTexture3DDescription description{
                width,
                height,
                depth,
                1,
                GraphicsTextureFormat::Rgba16Float
            };
            const auto coefficientsPerChannel = *probeCount * 4;
            for (std::size_t channel{};
                channel < createdViews.size();
                ++channel)
            {
                const auto channelCoefficients = coefficients.subspan(
                    channel * coefficientsPerChannel,
                    coefficientsPerChannel);
                const std::array initialData{
                    GraphicsTextureSubresourceData{
                        std::as_bytes(channelCoefficients),
                        width * 8u,
                        width * height * 8u
                    }
                };
                const auto texture = m_backend->CreateTexture3D(
                    description,
                    initialData);
                createdViews[channel] =
                    m_backend->CreateShaderResourceView(
                        texture,
                        GraphicsTextureViewDescription{ 0, 1 });
            }
        }
        catch (...)
        {
            return {};
        }
        return createdViews;
    }

    void GraphicsDevice::UpdateTexture2D(
        const GraphicsTextureHandle& texture,
        const std::uint32_t mipLevel,
        const GraphicsTextureSubresourceData& data)
    {
        if (m_backend == nullptr)
        {
            throw std::logic_error(
                "UpdateTexture2D requires an initialized graphics backend.");
        }
        m_backend->UpdateTexture2D(texture, mipLevel, data);
    }

    GraphicsViewHandle GraphicsDevice::CreateShaderResourceView(
        const GraphicsTextureHandle& texture,
        const GraphicsTextureViewDescription& description)
    {
        if (m_backend == nullptr)
        {
            throw std::logic_error(
                "CreateShaderResourceView requires an initialized graphics "
                "backend.");
        }
        return m_backend->CreateShaderResourceView(
            texture,
            description);
    }

    bool GraphicsDevice::IsGraphicsViewCurrent(
        const GraphicsViewHandle& view) const noexcept
    {
        return m_backend != nullptr
            && m_backend->IsViewCurrent(view);
    }

    void GraphicsDevice::BeginShadowMap(
        ShadowMap& shadowMap,
        const std::uint32_t cascadeIndex)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "BeginShadowMap requires an initialized device.");
        }

        m_backend->BeginShadowMap(shadowMap, cascadeIndex);
    }

    void GraphicsDevice::EndShadowMap(ShadowMap& shadowMap)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "EndShadowMap requires an initialized device.");
        }

        m_backend->EndShadowMap(shadowMap);
    }

    void GraphicsDevice::UpdateClusteredLights(
        LightingState& lighting,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "UpdateClusteredLights requires an initialized device.");
        }

        // 既存どおり、更新の直前に初回だけシェーダーと資源を作ります。
        auto& clusteredLights = Clusters();
        DirectX::XMFLOAT4X4 viewValues{};
        DirectX::XMFLOAT4X4 projectionValues{};
        DirectX::XMStoreFloat4x4(&viewValues, view);
        DirectX::XMStoreFloat4x4(&projectionValues, projection);
        m_backend->UpdateClusteredLights(
            clusteredLights,
            lighting,
            viewValues,
            projectionValues,
            width,
            height);
    }

    std::unique_ptr<GraphicsOutputState>
        GraphicsDevice::CaptureOutputState()
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CaptureOutputState requires an initialized device.");
        }

        return m_backend->CaptureOutputState();
    }

    void GraphicsDevice::RestoreOutputState(
        const GraphicsOutputState& state)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "RestoreOutputState requires an initialized device.");
        }

        m_backend->RestoreOutputState(state);
    }

    void GraphicsDevice::Resize(const std::uint32_t width, const std::uint32_t height)
    {
        if (!IsInitialized() || width == 0 || height == 0)
        {
            return;
        }

        m_width = width;
        m_height = height;
        m_uiWidth = width;
        m_uiHeight = height;

        m_backend->Resize(m_width, m_height);
    }

    void GraphicsDevice::BeginFrame(const float clearColor[4])
    {
        m_gpuProfiler.OpenFrame();
        RefreshMemoryStatistics();
        // 大きいテクスチャの段階アップロードを予算内で進めます
        // （メインスレッドのフレーム先頭が唯一の転送ポイント）。
        if (auto* assets = TryAssets())
        {
            assets->PumpTextureUploads();
            assets->PumpModelUploads();
        }
        m_uiWidth = m_width;
        m_uiHeight = m_height;
        m_backend->BindAndClearBackBuffer(clearColor);
    }

    GraphicsBufferHandle GraphicsDevice::AcquireInstanceBufferHandle(
        const std::span<const std::byte> data)
    {
        if (data.empty() || !IsInitialized())
        {
            return {};
        }
        if (!m_backend->UpdateDynamicVertexBuffer(
                m_instanceBuffer,
                data))
        {
            return {};
        }
        return m_instanceBuffer;
    }

    void GraphicsDevice::BindVertexBuffer(
        const GraphicsBufferHandle& buffer,
        const std::uint32_t slot,
        const std::uint32_t stride,
        const std::uint32_t offset)
    {
        if (m_backend == nullptr)
        {
            throw std::logic_error(
                "BindVertexBuffer requires an initialized graphics backend.");
        }
        m_backend->BindVertexBuffer(
            buffer,
            slot,
            stride,
            offset);
    }

    bool GraphicsDevice::TryBindPixelShaderResources(
        const std::uint32_t firstSlot,
        const std::span<const GraphicsViewHandle> resources,
        const GraphicsViewHandle& fallback) noexcept
    {
        return m_backend != nullptr
            && m_backend->TryBindPixelShaderResources(
                firstSlot,
                resources,
                fallback);
    }

    void GraphicsDevice::EndFrame()
    {
        // Presentより前に流します。デバイスを失う描画があった場合、
        // その理由はこのメッセージ側に出ていることが多いためです。
        m_backend->DrainDebugMessages();
        m_gpuProfiler.CloseFrame();
        m_backend->Present(
            m_graphicsSettings.vSyncEnabled);
    }

    std::vector<std::uint8_t>
        GraphicsDevice::CaptureBackBuffer(
            std::uint32_t& width,
            std::uint32_t& height) const
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CaptureBackBuffer requires an initialized device.");
        }
        return m_backend->CaptureBackBuffer(width, height);
    }
}
