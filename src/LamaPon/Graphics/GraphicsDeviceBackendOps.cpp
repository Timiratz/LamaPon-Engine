#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsDeviceState.h"

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
    // 2D画像の生成を転送します(description: 形式・寸法・更新方式, initialData: 全ミップの初期値)。
    GraphicsTextureHandle GraphicsDevice::CreateTexture2D(
        const GraphicsTexture2DDescription& description,
        const std::span<const GraphicsTextureSubresourceData>
            initialData)
    {
        if (m_state->m_backend == nullptr)
        {
            throw std::logic_error(
                "CreateTexture2D requires an initialized graphics backend.");
        }
        return m_state->m_backend->CreateTexture2D(
            description,
            initialData);
    }

    // 3D画像の生成を転送します(description: 形式と寸法, initialData: 全ミップの体積データ)。
    GraphicsTextureHandle GraphicsDevice::CreateTexture3D(
        const GraphicsTexture3DDescription& description,
        const std::span<const GraphicsTextureSubresourceData>
            initialData)
    {
        if (m_state->m_backend == nullptr)
        {
            throw std::logic_error(
                "CreateTexture3D requires an initialized graphics backend.");
        }
        return m_state->m_backend->CreateTexture3D(
            description,
            initialData);
    }

    // RGB別のGI入力を生成し、失敗なら全て空です(width: プローブ列数, height: プローブ行数, depth: プローブ奥行数, coefficients: RGB順に各4本の半精度SH係数)。
    std::array<GraphicsViewHandle, 3>
        GraphicsDevice::UploadBakedGlobalIlluminationViews(
            const std::uint32_t width,
            const std::uint32_t height,
            const std::uint32_t depth,
            const std::span<const std::uint16_t> coefficients)
            const noexcept
    {
        // RGB別のGI入力ビュー
        std::array<GraphicsViewHandle, 3> createdViews;
        // 有効なGIプローブ数
        const auto probeCount =
            BakedGlobalIlluminationProbeCount(
                width,
                height,
                depth);
        if (m_state->m_backend == nullptr
            || !probeCount.has_value()
            || coefficients.size()
                != *probeCount
                    * BakedGlobalIlluminationCoefficientsPerProbe)
        {
            return createdViews;
        }

        try
        {
            // RGBA16Fの3D画像仕様
            const GraphicsTexture3DDescription description{
                width,
                height,
                depth,
                1,
                GraphicsTextureFormat::Rgba16Float
            };
            // 色成分ごとのSH係数数
            const auto coefficientsPerChannel = *probeCount * 4;
            // RGBの色成分番号
            for (std::size_t channel{};
                channel < createdViews.size();
                ++channel)
            {
                // 当該色成分の半精度SH係数
                const auto channelCoefficients = coefficients.subspan(
                    channel * coefficientsPerChannel,
                    coefficientsPerChannel);
                // 色成分の3D画像初期値
                const std::array initialData{
                    GraphicsTextureSubresourceData{
                        std::as_bytes(channelCoefficients),
                        width * 8u,
                        width * height * 8u
                    }
                };
                // 生成する色成分の3D画像
                const auto texture = m_state->m_backend->CreateTexture3D(
                    description,
                    initialData);
                createdViews[channel] =
                    m_state->m_backend->CreateShaderResourceView(
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

    // 描画スレッドでミップ更新を転送します(texture: 更新可能な同世代の画像, mipLevel: ミップ番号, data: 画像と行ピッチ)。
    void GraphicsDevice::UpdateTexture2D(
        const GraphicsTextureHandle& texture,
        const std::uint32_t mipLevel,
        const GraphicsTextureSubresourceData& data)
    {
        if (m_state->m_backend == nullptr)
        {
            throw std::logic_error(
                "UpdateTexture2D requires an initialized graphics backend.");
        }
        m_state->m_backend->UpdateTexture2D(texture, mipLevel, data);
    }

    // 読み取りビューの生成を転送します(texture: 同世代の画像, description: 形式とミップ範囲)。
    GraphicsViewHandle GraphicsDevice::CreateShaderResourceView(
        const GraphicsTextureHandle& texture,
        const GraphicsTextureViewDescription& description)
    {
        if (m_state->m_backend == nullptr)
        {
            throw std::logic_error(
                "CreateShaderResourceView requires an initialized graphics "
                "backend.");
        }
        return m_state->m_backend->CreateShaderResourceView(
            texture,
            description);
    }

    // 現在の資源世代に属するか返します(view: 確認するビュー)。
    bool GraphicsDevice::IsGraphicsViewCurrent(
        const GraphicsViewHandle& view) const noexcept
    {
        return m_state->m_backend != nullptr
            && m_state->m_backend->IsViewCurrent(view);
    }

    // 影の描画を開始します(shadowMap: 影の描画先, cascadeIndex: 描画面の番号)。
    void GraphicsDevice::BeginShadowMap(
        ShadowMap& shadowMap,
        const std::uint32_t cascadeIndex)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "BeginShadowMap requires an initialized device.");
        }

        m_state->m_backend->BeginShadowMap(shadowMap, cascadeIndex);
    }

    // 影描画前の出力へ復元します(shadowMap: 終了する影の描画先)。
    void GraphicsDevice::EndShadowMap(ShadowMap& shadowMap)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "EndShadowMap requires an initialized device.");
        }

        m_state->m_backend->EndShadowMap(shadowMap);
    }

    // Forward+の番号表を更新します(lighting: ライト情報と結果, view: ビュー行列, projection: 射影行列, width: 画面幅, height: 画面高)。
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


        // 遅延生成したForward+資源
        auto& clusteredLights = Clusters();
        // 保存形式のビュー行列
        DirectX::XMFLOAT4X4 viewValues{};
        // 保存形式の射影行列
        DirectX::XMFLOAT4X4 projectionValues{};
        DirectX::XMStoreFloat4x4(&viewValues, view);
        DirectX::XMStoreFloat4x4(&projectionValues, projection);
        m_state->m_backend->UpdateClusteredLights(
            clusteredLights,
            lighting,
            viewValues,
            projectionValues,
            width,
            height);
    }

    // 同じ描画区間で使う主描画先の復元状態を保存します。
    std::unique_ptr<GraphicsOutputState>
        GraphicsDevice::CaptureOutputState()
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CaptureOutputState requires an initialized device.");
        }

        return m_state->m_backend->CaptureOutputState();
    }

    // 主描画先を復元します(state: 同じバックエンドで保存した状態)。
    void GraphicsDevice::RestoreOutputState(
        const GraphicsOutputState& state)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "RestoreOutputState requires an initialized device.");
        }

        m_state->m_backend->RestoreOutputState(state);
    }

    // 表示寸法が変わった場合に再作成します(width: 描画幅, height: 描画高)。
    void GraphicsDevice::Resize(const std::uint32_t width, const std::uint32_t height)
    {
        if (!IsInitialized() || width == 0 || height == 0)
        {
            return;
        }

        if (m_state->m_width == width
            && m_state->m_height == height)
        {
            return;
        }

        m_state->m_backend->Resize(width, height);
        m_state->m_width = width;
        m_state->m_height = height;
        m_state->m_uiWidth = width;
        m_state->m_uiHeight = height;
    }

    // 計測と段階転送を開始して画面を消去します(clearColor: RGBAの消去色)。
    void GraphicsDevice::BeginFrame(const float clearColor[4])
    {
        m_state->m_gpuProfiler.OpenFrame();
        RefreshMemoryStatistics();
        // 段階アップロードは描画スレッドのフレーム先頭で予算内の分だけ進めます。
        // 段階アップロードの資産管理
        if (auto* assets = TryAssets())
        {
            assets->PumpTextureUploads();
            assets->PumpModelUploads();
        }
        m_state->m_uiWidth = m_state->m_width;
        m_state->m_uiHeight = m_state->m_height;
        m_state->m_backend->BindAndClearBackBuffer(clearColor);
    }

    // 共通インスタンス頂点資源を更新します(data: 頂点のバイト列)。
    GraphicsBufferHandle GraphicsDevice::AcquireInstanceBufferHandle(
        const std::span<const std::byte> data)
    {
        if (data.empty() || !IsInitialized())
        {
            return {};
        }
        if (!m_state->m_backend->UpdateDynamicVertexBuffer(
                m_state->m_instanceBuffer,
                data))
        {
            return {};
        }
        return m_state->m_instanceBuffer;
    }

    // 頂点資源の設定を転送します(buffer: 同世代の頂点資源, slot: 入力スロット, stride: 頂点間隔のバイト数, offset: 先頭のバイト位置)。
    void GraphicsDevice::BindVertexBuffer(
        const GraphicsBufferHandle& buffer,
        const std::uint32_t slot,
        const std::uint32_t stride,
        const std::uint32_t offset)
    {
        if (m_state->m_backend == nullptr)
        {
            throw std::logic_error(
                "BindVertexBuffer requires an initialized graphics backend.");
        }
        m_state->m_backend->BindVertexBuffer(
            buffer,
            slot,
            stride,
            offset);
    }

    // 入力ビューの設定を転送します(firstSlot: 先頭t番号, resources: 設定するビュー列, fallback: 無効要素の代替ビュー)。
    bool GraphicsDevice::TryBindPixelShaderResources(
        const std::uint32_t firstSlot,
        const std::span<const GraphicsViewHandle> resources,
        const GraphicsViewHandle& fallback) noexcept
    {
        return m_state->m_backend != nullptr
            && m_state->m_backend->TryBindPixelShaderResources(
                firstSlot,
                resources,
                fallback);
    }

    // 計測と描画記録を確定して画面を表示します。
    void GraphicsDevice::EndFrame()
    {
        // デバイス喪失時の理由を拾うため、Presentより先に検証メッセージを回収します。
        m_state->m_backend->DrainDebugMessages();
        m_state->m_gpuProfiler.CloseFrame();
        // 計測区間を閉じてから描画記録を確定します。
        m_state->m_frameDebugger.EndFrame();
        m_state->m_backend->Present(
            m_state->m_graphicsSettings.vSyncEnabled);
    }

    // 表示画像をCPUへ読み戻します(width: 画像幅の出力, height: 画像高の出力)。
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
        return m_state->m_backend->CaptureBackBuffer(width, height);
    }
}
