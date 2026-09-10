#include "LamaPon/Graphics/D3D11Backend.h"

#include "LamaPon/Core/Log.h"
#include "LamaPon/Graphics/ClusteredLights.h"
#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/GpuProfiler.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/ShadowMap.h"

#include <CommonStates.h>
#include <Effects.h>
#include <PrimitiveBatch.h>
#include <VertexTypes.h>

// IDXGIFactory5（ティアリング許可の問い合わせ）。d3d11.hが引く
// dxgi.hには入っていません。
#include <dxgi1_5.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    class D3D11OutputState final
        : public LamaPon::GraphicsOutputState
    {
    public:
        Microsoft::WRL::ComPtr<ID3D11Device> ownerDevice;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            renderTarget;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView>
            depthTarget;
        D3D11_VIEWPORT viewport{};
        bool hasViewport{};
    };

    class D3D11ResourceDomain final
        : public LamaPon::Detail::GraphicsResourceDomain
    {
    };

    class D3D11TexturePayload final
        : public LamaPon::Detail::GraphicsTexturePayload
    {
    public:
        D3D11TexturePayload(
            std::shared_ptr<LamaPon::Detail::GraphicsResourceDomain> domain,
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
            const D3D11_TEXTURE2D_DESC& description)
            : GraphicsTexturePayload(std::move(domain))
            , native(std::move(texture))
            , description(description)
        {
        }

        Microsoft::WRL::ComPtr<ID3D11Texture2D> native;
        D3D11_TEXTURE2D_DESC description{};
    };

    // 共通Texture3D handleが所有するDirectX 11側の実体です。
    // API固有型はこのBackend内部に閉じ込めます。
    class D3D11Texture3DPayload final
        : public LamaPon::Detail::GraphicsTexturePayload
    {
    public:
        D3D11Texture3DPayload(
            std::shared_ptr<LamaPon::Detail::GraphicsResourceDomain> domain,
            Microsoft::WRL::ComPtr<ID3D11Texture3D> texture,
            const D3D11_TEXTURE3D_DESC& description)
            : GraphicsTexturePayload(std::move(domain))
            , native(std::move(texture))
            , description(description)
        {
        }

        Microsoft::WRL::ComPtr<ID3D11Texture3D> native;
        D3D11_TEXTURE3D_DESC description{};
    };

    class D3D11BufferPayload final
        : public LamaPon::Detail::GraphicsBufferPayload
    {
    public:
        D3D11BufferPayload(
            std::shared_ptr<LamaPon::Detail::GraphicsResourceDomain> domain,
            Microsoft::WRL::ComPtr<ID3D11Buffer> buffer,
            const std::size_t byteCapacity)
            : GraphicsBufferPayload(std::move(domain))
            , native(std::move(buffer))
            , capacity(byteCapacity)
        {
        }

        Microsoft::WRL::ComPtr<ID3D11Buffer> native;
        std::size_t capacity{};
    };

    class D3D11ViewPayload final
        : public LamaPon::Detail::GraphicsViewPayload
    {
    public:
        D3D11ViewPayload(
            std::shared_ptr<LamaPon::Detail::GraphicsResourceDomain> domain,
            const LamaPon::GraphicsViewKind kind,
            LamaPon::GraphicsTextureHandle resource,
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view)
            : GraphicsViewPayload(
                std::move(domain),
                kind,
                std::move(resource))
            , native(std::move(view))
        {
        }

        D3D11ViewPayload(
            std::shared_ptr<LamaPon::Detail::GraphicsResourceDomain> domain,
            const LamaPon::GraphicsViewKind kind,
            LamaPon::GraphicsBufferHandle resource,
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view)
            : GraphicsViewPayload(
                std::move(domain),
                kind,
                std::move(resource))
            , native(std::move(view))
        {
        }

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> native;
    };

    class D3D11DebugDrawingBackend final
        : public LamaPon::DebugDrawingBackend
    {
    public:
        D3D11DebugDrawingBackend(
            ID3D11Device* const device,
            ID3D11DeviceContext* const context)
            : m_context(context)
            , m_effect(std::make_unique<DirectX::BasicEffect>(device))
            , m_states(std::make_unique<DirectX::CommonStates>(device))
            , m_batch(std::make_unique<DirectX::PrimitiveBatch<
                DirectX::VertexPositionColor>>(context))
        {
            if (device == nullptr || context == nullptr)
            {
                throw std::invalid_argument(
                    "D3D11 debug drawing requires a device and context.");
            }
            m_effect->SetVertexColorEnabled(true);

            const void* shaderByteCode{};
            std::size_t byteCodeLength{};
            m_effect->GetVertexShaderBytecode(
                &shaderByteCode,
                &byteCodeLength);
            const HRESULT result = device->CreateInputLayout(
                DirectX::VertexPositionColor::InputElements,
                DirectX::VertexPositionColor::InputElementCount,
                shaderByteCode,
                byteCodeLength,
                m_inputLayout.ReleaseAndGetAddressOf());
            if (FAILED(result))
            {
                throw std::runtime_error(
                    "Failed to create debug renderer input layout.");
            }
        }

        void DrawLines(
            const std::span<const LamaPon::DebugLine> lines,
            const DirectX::XMFLOAT4X4& view,
            const DirectX::XMFLOAT4X4& projection) override
        {
            if (lines.empty())
            {
                return;
            }

            // 直前の2D/UI等が残した状態に依存せず、補助線を常に
            // 手前へ描くという従来のDebugRenderer契約を維持します。
            m_context->OMSetBlendState(
                m_states->NonPremultiplied(),
                nullptr,
                0xFFFFFFFF);
            m_context->OMSetDepthStencilState(
                m_states->DepthNone(),
                0);
            m_context->RSSetState(m_states->CullNone());

            m_effect->SetWorld(DirectX::XMMatrixIdentity());
            m_effect->SetView(DirectX::XMLoadFloat4x4(&view));
            m_effect->SetProjection(
                DirectX::XMLoadFloat4x4(&projection));
            m_effect->Apply(m_context.Get());
            m_context->IASetInputLayout(m_inputLayout.Get());

            m_batch->Begin();
            for (const auto& line : lines)
            {
                m_batch->DrawLine(
                    DirectX::VertexPositionColor{
                        line.start,
                        line.color
                    },
                    DirectX::VertexPositionColor{
                        line.end,
                        line.color
                    });
            }
            m_batch->End();
        }

    private:
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
        std::unique_ptr<DirectX::BasicEffect> m_effect;
        std::unique_ptr<DirectX::CommonStates> m_states;
        std::unique_ptr<DirectX::PrimitiveBatch<
            DirectX::VertexPositionColor>> m_batch;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> m_inputLayout;
    };

    void ThrowIfFailed(
        const HRESULT result,
        const char* operation)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(
                std::string(operation)
                + " failed with HRESULT "
                + std::to_string(
                    static_cast<unsigned long>(result)));
        }
    }

    [[nodiscard]] DXGI_FORMAT ToDxgiFormat(
        const LamaPon::GraphicsTextureFormat format)
    {
        switch (format)
        {
        case LamaPon::GraphicsTextureFormat::Rgba8Unorm:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case LamaPon::GraphicsTextureFormat::Bgra8Unorm:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case LamaPon::GraphicsTextureFormat::Bc1Unorm:
            return DXGI_FORMAT_BC1_UNORM;
        case LamaPon::GraphicsTextureFormat::Bc3Unorm:
            return DXGI_FORMAT_BC3_UNORM;
        case LamaPon::GraphicsTextureFormat::Bc5Unorm:
            return DXGI_FORMAT_BC5_UNORM;
        case LamaPon::GraphicsTextureFormat::Rgba16Float:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default:
            throw std::invalid_argument(
                "Unsupported graphics texture format.");
        }
    }

    [[nodiscard]] std::uint32_t MaximumMipLevels(
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t depth = 1) noexcept
    {
        std::uint32_t levels = 1;
        while (width > 1 || height > 1 || depth > 1)
        {
            width = std::max(width / 2, 1u);
            height = std::max(height / 2, 1u);
            depth = std::max(depth / 2, 1u);
            ++levels;
        }
        return levels;
    }

    struct TextureSubresourceLayout final
    {
        std::uint32_t minimumRowBytes{};
        std::uint32_t rowCount{};
    };

    [[nodiscard]] TextureSubresourceLayout RequiredTextureLayout(
        const DXGI_FORMAT format,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        switch (format)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            if (width > std::numeric_limits<std::uint32_t>::max() / 4u)
            {
                throw std::invalid_argument(
                    "The texture row pitch cannot be represented.");
            }
            return { width * 4u, height };
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            if (width > std::numeric_limits<std::uint32_t>::max() / 8u)
            {
                throw std::invalid_argument(
                    "The texture row pitch cannot be represented.");
            }
            return { width * 8u, height };
        case DXGI_FORMAT_BC1_UNORM:
            return {
                std::max((width + 3u) / 4u, 1u) * 8u,
                std::max((height + 3u) / 4u, 1u)
            };
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC5_UNORM:
            return {
                std::max((width + 3u) / 4u, 1u) * 16u,
                std::max((height + 3u) / 4u, 1u)
            };
        default:
            throw std::invalid_argument(
                "The texture format has no upload layout.");
        }
    }

    void ValidateTextureSubresourceData(
        const D3D11_TEXTURE2D_DESC& texture,
        const std::uint32_t mipLevel,
        const LamaPon::GraphicsTextureSubresourceData& data)
    {
        if (mipLevel >= texture.MipLevels
            || data.bytes.empty()
            || data.rowPitch == 0
            || (data.slicePitch == 0
                && data.bytes.size()
                    > std::numeric_limits<UINT>::max()))
        {
            throw std::invalid_argument(
                "The texture subresource data is incomplete.");
        }

        const auto mipWidth = std::max(
            texture.Width >> mipLevel,
            1u);
        const auto mipHeight = std::max(
            texture.Height >> mipLevel,
            1u);
        const auto layout = RequiredTextureLayout(
            texture.Format,
            mipWidth,
            mipHeight);
        if (data.rowPitch < layout.minimumRowBytes)
        {
            throw std::invalid_argument(
                "The texture subresource row pitch is too small.");
        }
        const auto requiredBytes =
            static_cast<std::uint64_t>(data.rowPitch)
                * (layout.rowCount - 1u)
            + layout.minimumRowBytes;
        const auto slicePitch = data.slicePitch != 0
            ? static_cast<std::uint64_t>(data.slicePitch)
            : static_cast<std::uint64_t>(data.bytes.size());
        if (requiredBytes > data.bytes.size()
            || requiredBytes > slicePitch
            || slicePitch > data.bytes.size())
        {
            throw std::invalid_argument(
                "The texture subresource byte range is too small.");
        }
    }

    void ValidateTexture3DSubresourceData(
        const D3D11_TEXTURE3D_DESC& texture,
        const std::uint32_t mipLevel,
        const LamaPon::GraphicsTextureSubresourceData& data)
    {
        if (mipLevel >= texture.MipLevels
            || data.bytes.empty()
            || data.rowPitch == 0
            || data.slicePitch == 0)
        {
            throw std::invalid_argument(
                "The Texture3D subresource data is incomplete.");
        }

        const auto mipWidth = std::max(
            texture.Width >> mipLevel,
            1u);
        const auto mipHeight = std::max(
            texture.Height >> mipLevel,
            1u);
        const auto mipDepth = std::max(
            texture.Depth >> mipLevel,
            1u);
        const auto layout = RequiredTextureLayout(
            texture.Format,
            mipWidth,
            mipHeight);
        if (data.rowPitch < layout.minimumRowBytes)
        {
            throw std::invalid_argument(
                "The Texture3D row pitch is too small.");
        }
        const auto requiredSliceBytes =
            static_cast<std::uint64_t>(data.rowPitch)
                * (layout.rowCount - 1u)
            + layout.minimumRowBytes;
        const auto requiredBytes =
            static_cast<std::uint64_t>(data.slicePitch)
                * (mipDepth - 1u)
            + requiredSliceBytes;
        if (requiredSliceBytes > data.slicePitch
            || data.slicePitch > data.bytes.size()
            || requiredBytes > data.bytes.size())
        {
            throw std::invalid_argument(
                "The Texture3D subresource byte range is too small.");
        }
    }

    // この環境でティアリング許可を利用できるか。対応が無い
    // Windowsや仮想GPUではIDXGIFactory5を取得できないためfalseです。
    [[nodiscard]] bool QueryTearingSupport()
    {
        Microsoft::WRL::ComPtr<IDXGIFactory5> factory;
        if (FAILED(CreateDXGIFactory1(
                IID_PPV_ARGS(factory.GetAddressOf()))))
        {
            return false;
        }
        BOOL allowed = FALSE;
        if (FAILED(factory->CheckFeatureSupport(
                DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                &allowed,
                sizeof(allowed))))
        {
            return false;
        }
        return allowed != FALSE;
    }
}

namespace LamaPon
{
    D3D11Backend::D3D11Backend() = default;

    D3D11Backend::~D3D11Backend()
    {
        PrepareForResourceRelease();
        Shutdown();
    }

    void D3D11Backend::Initialize(
        const GraphicsBackendCreateInfo& createInfo)
    {
        // 同じインスタンスを再利用しても古いCOM参照を残しません。
        PrepareForResourceRelease();
        Shutdown();

        const HWND window = static_cast<HWND>(
            createInfo.nativeWindow);
        const std::uint32_t width = std::max(
            createInfo.width,
            1u);
        const std::uint32_t height = std::max(
            createInfo.height,
            1u);

        // ティアリングには、環境対応、スワップチェーン作成フラグ、
        // Presentの同期間隔0と提示フラグの組み合わせが必要です。
        m_tearingAllowed = QueryTearingSupport();

        DXGI_SWAP_CHAIN_DESC swapChainDescription{};
        swapChainDescription.BufferDesc.Width = width;
        swapChainDescription.BufferDesc.Height = height;
        swapChainDescription.BufferDesc.Format =
            DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDescription.SampleDesc.Count = 1;
        swapChainDescription.BufferUsage =
            DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDescription.BufferCount = 2;
        swapChainDescription.OutputWindow = window;
        swapChainDescription.Windowed = TRUE;
        swapChainDescription.SwapEffect =
            DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDescription.Flags = m_tearingAllowed
            ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
            : 0u;

        constexpr std::array featureLevels{
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0
        };

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        const bool wantDebugLayer =
#if defined(_DEBUG)
            true;
#else
            createInfo.enableDebugLayer;
#endif
        if (wantDebugLayer)
        {
            flags |= D3D11_CREATE_DEVICE_DEBUG;
        }

        D3D_FEATURE_LEVEL selectedFeatureLevel{};
        const auto createDevice =
            [this,
                &swapChainDescription,
                &featureLevels,
                &selectedFeatureLevel](
                const D3D_DRIVER_TYPE driverType,
                const UINT deviceFlags)
        {
            return D3D11CreateDeviceAndSwapChain(
                nullptr,
                driverType,
                nullptr,
                deviceFlags,
                featureLevels.data(),
                static_cast<UINT>(featureLevels.size()),
                D3D11_SDK_VERSION,
                &swapChainDescription,
                m_swapChain.ReleaseAndGetAddressOf(),
                m_device.ReleaseAndGetAddressOf(),
                &selectedFeatureLevel,
                m_context.ReleaseAndGetAddressOf());
        };

        const D3D_DRIVER_TYPE primaryDriver =
            createInfo.preferWarpAdapter
                ? D3D_DRIVER_TYPE_WARP
                : D3D_DRIVER_TYPE_HARDWARE;
        HRESULT result = createDevice(primaryDriver, flags);

        // SDKのデバッグレイヤーが利用できない場合は、フラグを外して
        // 描画そのものは継続します。
        if (wantDebugLayer
            && result == DXGI_ERROR_SDK_COMPONENT_MISSING)
        {
            flags &= ~D3D11_CREATE_DEVICE_DEBUG;
            result = createDevice(primaryDriver, flags);
        }

        // 対応判定後もティアリング付き作成が失敗するドライバーでは、
        // 提示フラグを外して再作成します。
        if (FAILED(result) && m_tearingAllowed)
        {
            m_tearingAllowed = false;
            swapChainDescription.Flags = 0;
            result = createDevice(primaryDriver, flags);
            if (SUCCEEDED(result))
            {
                Logger::Instance().Warning(
                    "ティアリング許可付きのスワップチェーンを作れ"
                    "なかったため、無効で起動しました。VSyncを切っても"
                    "モニターのリフレッシュレートがFPSの上限になります。");
            }
        }

        // GPUが使えない環境ではWARP（CPUラスタライザ）へ自動で
        // フォールバックします。
        if (FAILED(result) && !createInfo.preferWarpAdapter)
        {
            result = createDevice(
                D3D_DRIVER_TYPE_WARP,
                flags);
            if (SUCCEEDED(result))
            {
                Logger::Instance().Warning(
                    "GPUデバイスの作成に失敗したため、WARP"
                    "（CPU描画）で起動しました。描画性能は低下します。");
            }
        }

        ThrowIfFailed(result, "D3D11CreateDeviceAndSwapChain");

        // デバッガーなしでも確認できるよう、InfoQueueのメッセージを
        // エンジンログへ転送します。
        if ((flags & D3D11_CREATE_DEVICE_DEBUG) != 0)
        {
            if (SUCCEEDED(m_device.As(&m_infoQueue)))
            {
                Logger::Instance().Info(
                    "D3D11のデバッグレイヤーを有効にしました。"
                    "不正な描画はログへ出ます（描画は遅くなります）。");
            }
            else
            {
                Logger::Instance().Warning(
                    "D3D11のデバッグレイヤーは有効ですが、"
                    "InfoQueueを取得できませんでした。");
            }
        }
        else if (createInfo.enableDebugLayer)
        {
            Logger::Instance().Warning(
                "--d3ddebug が指定されましたが、D3D11の"
                "デバッグレイヤーを有効にできませんでした。"
                "Windowsのオプション機能「グラフィックス ツール」が"
                "必要です。");
        }

        if (selectedFeatureLevel < D3D_FEATURE_LEVEL_11_0)
        {
            throw std::runtime_error(
                "Direct3D feature level 11.0 is required.");
        }

        LogSelectedAdapter();
        CreateSizeDependentResources(width, height);
        m_gpuProfilerBackend = CreateProfilerBackend(
            m_device.Get(),
            m_context.Get());
        // 同じD3D11 APIでも、Initializeごとに別Device世代として扱います。
        // 外部に残った旧handleはnative解決時にこのidentityで拒否します。
        m_resourceDomain = std::make_shared<D3D11ResourceDomain>();
    }

    void D3D11Backend::PrepareForResourceRelease() noexcept
    {
        if (m_context)
        {
            m_context->ClearState();
            m_context->Flush();
        }
    }

    void D3D11Backend::Shutdown() noexcept
    {
        // 先に現役domainを外し、外部に残ったhandleをstaleにします。
        // payload側のCOM参照は、そのhandleが最後に破棄されるまで安全に残ります。
        m_resourceDomain.reset();
        m_gpuProfilerBackend.reset();
        m_infoQueue.Reset();
        m_depthStencilView.Reset();
        m_depthTexture.Reset();
        m_renderTargetView.Reset();
        m_swapChain.Reset();
        m_context.Reset();
        m_device.Reset();
        m_viewport = {};
        m_tearingAllowed = false;
        m_debugMessagesLogged = 0;
    }

    void D3D11Backend::Resize(
        const std::uint32_t width,
        const std::uint32_t height)
    {
        if (!IsInitialized() || width == 0 || height == 0)
        {
            return;
        }

        m_context->OMSetRenderTargets(0, nullptr, nullptr);
        m_renderTargetView.Reset();
        m_depthStencilView.Reset();
        m_depthTexture.Reset();
        m_context->Flush();

        // 作成時と同じフラグを渡し直し、リサイズ後もティアリング
        // 許可を維持します。
        ThrowIfFailed(
            m_swapChain->ResizeBuffers(
                0,
                width,
                height,
                DXGI_FORMAT_UNKNOWN,
                m_tearingAllowed
                    ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
                    : 0u),
            "IDXGISwapChain::ResizeBuffers");

        CreateSizeDependentResources(width, height);
    }

    void D3D11Backend::BindBackBuffer()
    {
        if (!IsInitialized() || m_renderTargetView == nullptr)
        {
            throw std::logic_error(
                "BindBackBuffer requires an initialized backend.");
        }
        ID3D11RenderTargetView* renderTargets[]{
            m_renderTargetView.Get() };
        m_context->OMSetRenderTargets(
            1,
            renderTargets,
            nullptr);
        m_context->RSSetViewports(1, &m_viewport);
    }

    void D3D11Backend::ResizeOffscreenTarget(
        RenderTarget& target,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "ResizeOffscreenTarget requires an initialized backend.");
        }

        target.Resize(m_device.Get(), width, height);
        if (!target.IsValid())
        {
            throw std::logic_error(
                "ResizeOffscreenTarget failed to create a valid target.");
        }
        if (!target.m_ambientOcclusionView
            || !target.m_colorHistoryView
            || !target.m_reflectionDepthPyramidViewHandle
            || !target.m_depthView)
        {
            // Resizeが作った4本をすべて取り込めた後にだけ公開handleを
            // 更新し、途中失敗で新旧resourceを混在させません。
            auto ambientOcclusionView =
                ImportShaderResourceViewHandle(
                    target.m_occlusionBlurShaderResourceView.Get());
            auto colorHistoryView = ImportShaderResourceViewHandle(
                target.m_historyShaderResourceView.Get());
            auto reflectionDepthView =
                ImportShaderResourceViewHandle(
                    target.m_reflectionDepthPyramidView.Get());
            auto depthView = ImportShaderResourceViewHandle(
                target.m_depthShaderResourceView.Get());
            target.m_ambientOcclusionView =
                std::move(ambientOcclusionView);
            target.m_colorHistoryView = std::move(colorHistoryView);
            target.m_reflectionDepthPyramidViewHandle =
                std::move(reflectionDepthView);
            target.m_depthView = std::move(depthView);
        }
    }

    void D3D11Backend::BeginOffscreenTarget(
        RenderTarget& target,
        const float clearColor[4])
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "BeginOffscreenTarget requires an initialized backend.");
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
        if (m_context == nullptr)
        {
            throw std::logic_error(
                "BeginOffscreenTarget requires an available DirectX 11 "
                "context.");
        }

        target.Bind(m_context.Get());
        target.Clear(m_context.Get(), clearColor);
    }

    void D3D11Backend::BindOffscreenTarget(
        RenderTarget& target)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "BindOffscreenTarget requires an initialized backend.");
        }
        if (!target.IsValid())
        {
            throw std::invalid_argument(
                "BindOffscreenTarget requires a valid target.");
        }
        if (m_context == nullptr)
        {
            throw std::logic_error(
                "BindOffscreenTarget requires an available DirectX 11 "
                "context.");
        }

        target.Bind(m_context.Get());
    }

    void D3D11Backend::PublishOffscreenTarget(
        RenderTarget& target)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "PublishOffscreenTarget requires an initialized backend.");
        }
        if (!target.IsValid())
        {
            throw std::invalid_argument(
                "PublishOffscreenTarget requires a valid target.");
        }
        if (m_context == nullptr)
        {
            throw std::logic_error(
                "PublishOffscreenTarget requires an available DirectX 11 "
                "context.");
        }

        // CopyToDisplayは描画先を変更しません。バックバッファへの復帰は
        // BeginFrameなど、既存のフレーム制御側に任せます。
        target.CopyToDisplay(m_context.Get());
    }

    void D3D11Backend::BindOffscreenTargetDepthOnly(
        RenderTarget& target)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "BindOffscreenTargetDepthOnly requires an initialized "
                "backend.");
        }
        if (!target.IsValid())
        {
            throw std::invalid_argument(
                "BindOffscreenTargetDepthOnly requires a valid target.");
        }
        if (m_context == nullptr)
        {
            throw std::logic_error(
                "BindOffscreenTargetDepthOnly requires an available "
                "DirectX 11 context.");
        }

        target.BindDepthOnly(m_context.Get());
    }

    void D3D11Backend::CaptureOffscreenTargetDepth(
        RenderTarget& target)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CaptureOffscreenTargetDepth requires an initialized "
                "backend.");
        }
        if (!target.IsValid())
        {
            throw std::invalid_argument(
                "CaptureOffscreenTargetDepth requires a valid target.");
        }
        if (m_context == nullptr)
        {
            throw std::logic_error(
                "CaptureOffscreenTargetDepth requires an available "
                "DirectX 11 context.");
        }

        target.CaptureDepthForReflections(m_context.Get());
    }

    void D3D11Backend::CaptureOffscreenTargetColorHistory(
        RenderTarget& target,
        const DirectX::XMFLOAT4X4& viewProjection)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CaptureOffscreenTargetColorHistory requires an "
                "initialized backend.");
        }
        if (!target.IsValid())
        {
            throw std::invalid_argument(
                "CaptureOffscreenTargetColorHistory requires a valid "
                "target.");
        }
        if (m_context == nullptr)
        {
            throw std::logic_error(
                "CaptureOffscreenTargetColorHistory requires an "
                "available DirectX 11 context.");
        }

        target.CaptureColorHistory(
            m_context.Get(),
            viewProjection);
    }

    void D3D11Backend::CaptureOffscreenTargetTemporalHistory(
        RenderTarget& target,
        const DirectX::XMFLOAT4X4& viewProjection)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CaptureOffscreenTargetTemporalHistory requires an "
                "initialized backend.");
        }
        if (!target.IsValid())
        {
            throw std::invalid_argument(
                "CaptureOffscreenTargetTemporalHistory requires a valid "
                "target.");
        }
        if (m_context == nullptr)
        {
            throw std::logic_error(
                "CaptureOffscreenTargetTemporalHistory requires an "
                "available DirectX 11 context.");
        }

        target.CaptureTemporalHistory(
            m_context.Get(),
            viewProjection);
    }

    std::optional<float>
        D3D11Backend::TryReadOffscreenTargetLuminance(
            RenderTarget& target)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "TryReadOffscreenTargetLuminance requires an "
                "initialized backend.");
        }
        if (!target.IsValid())
        {
            throw std::invalid_argument(
                "TryReadOffscreenTargetLuminance requires a valid "
                "target.");
        }
        if (m_context == nullptr)
        {
            throw std::logic_error(
                "TryReadOffscreenTargetLuminance requires an "
                "available DirectX 11 context.");
        }

        return target.TryReadAutoExposureLuminance(
            m_context.Get());
    }

    void D3D11Backend::CaptureOffscreenTargetLuminance(
        RenderTarget& target)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CaptureOffscreenTargetLuminance requires an "
                "initialized backend.");
        }
        if (!target.IsValid())
        {
            throw std::invalid_argument(
                "CaptureOffscreenTargetLuminance requires a valid "
                "target.");
        }
        if (m_context == nullptr)
        {
            throw std::logic_error(
                "CaptureOffscreenTargetLuminance requires an "
                "available DirectX 11 context.");
        }

        target.CaptureAutoExposureLuminance(m_context.Get());
    }

    void D3D11Backend::InitializeShadowMap(
        ShadowMap& shadowMap,
        const std::uint32_t resolution,
        const std::uint32_t cascadeCount,
        const bool cube)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "InitializeShadowMap requires an initialized backend.");
        }

        shadowMap.Initialize(
            m_device.Get(),
            resolution,
            cascadeCount,
            cube);
        auto view = ImportShaderResourceViewHandle(
            shadowMap.m_shaderResourceView.Get());
        shadowMap.m_view = std::move(view);
    }

    void D3D11Backend::BeginShadowMap(
        ShadowMap& shadowMap,
        const std::uint32_t cascadeIndex)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "BeginShadowMap requires an initialized backend.");
        }
        if (m_context == nullptr)
        {
            throw std::logic_error(
                "BeginShadowMap requires an available "
                "DirectX 11 context.");
        }

        try
        {
            if (ResolveShaderResourceView(shadowMap.m_view)
                == shadowMap.m_shaderResourceView.Get())
            {
                shadowMap.Begin(m_context.Get(), cascadeIndex);
            }
        }
        catch (const std::invalid_argument&)
        {
            // 別Backend世代のmapは無効な入力として何もしません。
        }
    }

    void D3D11Backend::EndShadowMap(
        ShadowMap& shadowMap)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "EndShadowMap requires an initialized backend.");
        }
        if (m_context == nullptr)
        {
            throw std::logic_error(
                "EndShadowMap requires an available "
                "DirectX 11 context.");
        }

        try
        {
            if (ResolveShaderResourceView(shadowMap.m_view)
                == shadowMap.m_shaderResourceView.Get())
            {
                shadowMap.End(m_context.Get());
            }
        }
        catch (const std::invalid_argument&)
        {
            // Beginと同じく、別Backend世代のmapには触れません。
        }
    }

    void D3D11Backend::UpdateClusteredLights(
        ClusteredLights& clusteredLights,
        LightingState& lighting,
        const DirectX::XMFLOAT4X4& view,
        const DirectX::XMFLOAT4X4& projection,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "UpdateClusteredLights requires an initialized backend.");
        }
        if (m_context == nullptr)
        {
            throw std::logic_error(
                "UpdateClusteredLights requires an available "
                "DirectX 11 context.");
        }

        if (!clusteredLights.m_lightView
            || !clusteredLights.m_indexListView
            || !clusteredLights.m_countView)
        {
            // 3本すべてを作れてからownerへ反映し、途中失敗で
            // LightingStateへ不完全な組み合わせを出さないようにします。
            auto lightView = ImportShaderResourceViewHandle(
                clusteredLights.m_lightShaderResourceView.Get());
            auto indexListView = ImportShaderResourceViewHandle(
                clusteredLights.m_indexListShaderResourceView.Get());
            auto countView = ImportShaderResourceViewHandle(
                clusteredLights.m_countShaderResourceView.Get());
            clusteredLights.m_lightView = std::move(lightView);
            clusteredLights.m_indexListView =
                std::move(indexListView);
            clusteredLights.m_countView = std::move(countView);
        }

        const auto viewMatrix = DirectX::XMLoadFloat4x4(&view);
        const auto projectionMatrix =
            DirectX::XMLoadFloat4x4(&projection);
        clusteredLights.Update(
            m_context.Get(),
            lighting,
            viewMatrix,
            projectionMatrix,
            width,
            height);
    }

    std::unique_ptr<GraphicsOutputState>
        D3D11Backend::CaptureOutputState()
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CaptureOutputState requires an initialized backend.");
        }
        if (m_context == nullptr)
        {
            throw std::logic_error(
                "CaptureOutputState requires an available "
                "DirectX 11 context.");
        }

        auto state = std::make_unique<D3D11OutputState>();
        state->ownerDevice = m_device;
        m_context->OMGetRenderTargets(
            1,
            state->renderTarget.ReleaseAndGetAddressOf(),
            state->depthTarget.ReleaseAndGetAddressOf());
        UINT viewportCount = 1;
        m_context->RSGetViewports(
            &viewportCount,
            &state->viewport);
        state->hasViewport = viewportCount > 0;
        return state;
    }

    void D3D11Backend::RestoreOutputState(
        const GraphicsOutputState& state)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "RestoreOutputState requires an initialized backend.");
        }
        if (m_context == nullptr)
        {
            throw std::logic_error(
                "RestoreOutputState requires an available "
                "DirectX 11 context.");
        }

        const auto* const d3d11State =
            dynamic_cast<const D3D11OutputState*>(&state);
        if (d3d11State == nullptr
            || d3d11State->ownerDevice.Get() != m_device.Get())
        {
            throw std::invalid_argument(
                "RestoreOutputState requires a state captured by this "
                "backend.");
        }

        ID3D11RenderTargetView* renderTargets[]{
            d3d11State->renderTarget.Get() };
        m_context->OMSetRenderTargets(
            1,
            renderTargets,
            d3d11State->depthTarget.Get());
        if (d3d11State->hasViewport)
        {
            m_context->RSSetViewports(
                1,
                &d3d11State->viewport);
        }
    }

    GraphicsVideoMemoryStatistics
        D3D11Backend::QueryVideoMemoryStatistics() const noexcept
    {
        GraphicsVideoMemoryStatistics statistics;
        if (!m_device)
        {
            return statistics;
        }

        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (FAILED(m_device.As(&dxgiDevice))
            || !dxgiDevice
            || FAILED(dxgiDevice->GetAdapter(
                adapter.GetAddressOf()))
            || !adapter)
        {
            return statistics;
        }
        statistics.adapterAvailable = true;

        DXGI_ADAPTER_DESC description{};
        if (SUCCEEDED(adapter->GetDesc(&description)))
        {
            statistics.descriptionAvailable = true;
            statistics.dedicatedBytes =
                static_cast<std::uint64_t>(
                    description.DedicatedVideoMemory);
            statistics.sharedSystemBytes =
                static_cast<std::uint64_t>(
                    description.SharedSystemMemory);
        }

        Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
        if (FAILED(adapter.As(&adapter3)) || !adapter3)
        {
            return statistics;
        }

        DXGI_QUERY_VIDEO_MEMORY_INFO local{};
        DXGI_QUERY_VIDEO_MEMORY_INFO nonLocal{};
        statistics.localBudgetAvailable = SUCCEEDED(
            adapter3->QueryVideoMemoryInfo(
                0,
                DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
                &local));
        statistics.nonLocalBudgetAvailable = SUCCEEDED(
            adapter3->QueryVideoMemoryInfo(
                0,
                DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,
                &nonLocal));
        if (statistics.localBudgetAvailable)
        {
            statistics.localUsageBytes = local.CurrentUsage;
            statistics.localBudgetBytes = local.Budget;
        }
        if (statistics.nonLocalBudgetAvailable)
        {
            statistics.nonLocalUsageBytes = nonLocal.CurrentUsage;
            statistics.nonLocalBudgetBytes = nonLocal.Budget;
        }
        return statistics;
    }

    std::unique_ptr<DebugDrawingBackend>
        D3D11Backend::CreateDebugDrawingBackend()
    {
        if (!IsInitialized() || m_context == nullptr)
        {
            throw std::logic_error(
                "CreateDebugDrawingBackend requires an initialized backend.");
        }
        return std::make_unique<D3D11DebugDrawingBackend>(
            m_device.Get(),
            m_context.Get());
    }

    GpuProfilerBackend*
        D3D11Backend::ProfilerBackend() noexcept
    {
        return m_gpuProfilerBackend.get();
    }

    GraphicsTextureHandle D3D11Backend::CreateSolidRgba8Texture(
        const std::array<std::uint8_t, 4>& color)
    {
        const GraphicsTexture2DDescription description{
            1,
            1,
            1,
            GraphicsTextureFormat::Rgba8Unorm
        };
        const std::array initialData{
            GraphicsTextureSubresourceData{
                std::as_bytes(std::span{ color }),
                static_cast<std::uint32_t>(color.size()),
                static_cast<std::uint32_t>(color.size())
            }
        };
        return CreateTexture2D(description, initialData);
    }

    GraphicsViewHandle D3D11Backend::CreateShaderResourceView(
        const GraphicsTextureHandle& texture)
    {
        using Detail::GraphicsResourceHandleAccess;
        const auto* const texture2DPayload =
            dynamic_cast<const D3D11TexturePayload*>(
                GraphicsResourceHandleAccess::Payload(texture));
        const auto* const texture3DPayload =
            dynamic_cast<const D3D11Texture3DPayload*>(
                GraphicsResourceHandleAccess::Payload(texture));
        if (!IsInitialized() || m_resourceDomain == nullptr)
        {
            throw std::logic_error(
                "CreateShaderResourceView requires an initialized backend.");
        }
        if ((texture2DPayload == nullptr
                && texture3DPayload == nullptr)
            || GraphicsResourceHandleAccess::Domain(texture)
                != m_resourceDomain.get())
        {
            throw std::invalid_argument(
                "CreateShaderResourceView requires a texture from this "
                "backend generation.");
        }

        return CreateShaderResourceView(
            texture,
            GraphicsTextureViewDescription{
                0,
                texture2DPayload != nullptr
                    ? texture2DPayload->description.MipLevels
                    : texture3DPayload->description.MipLevels
            });
    }

    GraphicsTextureHandle D3D11Backend::CreateTexture2D(
        const GraphicsTexture2DDescription& description,
        const std::span<const GraphicsTextureSubresourceData>
            initialData)
    {
        if (!IsInitialized() || m_resourceDomain == nullptr)
        {
            throw std::logic_error(
                "CreateTexture2D requires an initialized backend.");
        }
        if (description.width == 0
            || description.height == 0
            || description.width
                > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
            || description.height
                > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
            || description.mipLevels == 0
            || description.mipLevels > MaximumMipLevels(
                description.width,
                description.height)
            || (description.updateMode
                    != GraphicsTextureUpdateMode::Immutable
                && description.updateMode
                    != GraphicsTextureUpdateMode::PerMipUpdate)
            || (description.updateMode
                    == GraphicsTextureUpdateMode::Immutable
                && initialData.empty())
            || (!initialData.empty()
                && initialData.size() != description.mipLevels))
        {
            throw std::invalid_argument(
                "CreateTexture2D received an invalid description or "
                "subresource count.");
        }

        D3D11_TEXTURE2D_DESC nativeDescription{};
        nativeDescription.Width = description.width;
        nativeDescription.Height = description.height;
        nativeDescription.MipLevels = description.mipLevels;
        nativeDescription.ArraySize = 1;
        nativeDescription.Format = ToDxgiFormat(description.format);
        nativeDescription.SampleDesc.Count = 1;
        nativeDescription.Usage = description.updateMode
                == GraphicsTextureUpdateMode::PerMipUpdate
            ? D3D11_USAGE_DEFAULT
            : D3D11_USAGE_IMMUTABLE;
        nativeDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        std::vector<D3D11_SUBRESOURCE_DATA> nativeInitialData;
        nativeInitialData.reserve(initialData.size());
        for (std::size_t mipLevel = 0;
            mipLevel < initialData.size();
            ++mipLevel)
        {
            const auto& subresource = initialData[mipLevel];
            ValidateTextureSubresourceData(
                nativeDescription,
                static_cast<std::uint32_t>(mipLevel),
                subresource);
            nativeInitialData.push_back(
                D3D11_SUBRESOURCE_DATA{
                    subresource.bytes.data(),
                    subresource.rowPitch,
                    subresource.slicePitch != 0
                        ? subresource.slicePitch
                        : static_cast<UINT>(
                            subresource.bytes.size())
                });
        }

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        ThrowIfFailed(
            m_device->CreateTexture2D(
                &nativeDescription,
                nativeInitialData.empty()
                    ? nullptr
                    : nativeInitialData.data(),
                texture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D");
        return Detail::GraphicsResourceHandleAccess::MakeTexture(
            std::make_shared<D3D11TexturePayload>(
                m_resourceDomain,
                std::move(texture),
                nativeDescription));
    }

    GraphicsTextureHandle D3D11Backend::CreateTexture3D(
        const GraphicsTexture3DDescription& description,
        const std::span<const GraphicsTextureSubresourceData>
            initialData)
    {
        if (!IsInitialized() || m_resourceDomain == nullptr)
        {
            throw std::logic_error(
                "CreateTexture3D requires an initialized backend.");
        }
        if (description.width == 0
            || description.height == 0
            || description.depth == 0
            || description.width
                > D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION
            || description.height
                > D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION
            || description.depth
                > D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION
            || description.mipLevels == 0
            || description.mipLevels > MaximumMipLevels(
                description.width,
                description.height,
                description.depth)
            || initialData.size() != description.mipLevels)
        {
            throw std::invalid_argument(
                "CreateTexture3D received an invalid description or "
                "subresource count.");
        }

        const auto nativeFormat = ToDxgiFormat(description.format);
        UINT formatSupport{};
        constexpr UINT requiredFormatSupport =
            D3D11_FORMAT_SUPPORT_TEXTURE3D
            | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
        if (FAILED(m_device->CheckFormatSupport(
                nativeFormat,
                &formatSupport))
            || (formatSupport & requiredFormatSupport)
                != requiredFormatSupport)
        {
            throw std::invalid_argument(
                "CreateTexture3D requires a shader-readable 3D texture "
                "format supported by the active backend.");
        }

        D3D11_TEXTURE3D_DESC nativeDescription{};
        nativeDescription.Width = description.width;
        nativeDescription.Height = description.height;
        nativeDescription.Depth = description.depth;
        nativeDescription.MipLevels = description.mipLevels;
        nativeDescription.Format = nativeFormat;
        nativeDescription.Usage = D3D11_USAGE_IMMUTABLE;
        nativeDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        std::vector<D3D11_SUBRESOURCE_DATA> nativeInitialData;
        nativeInitialData.reserve(initialData.size());
        for (std::size_t mipLevel{};
            mipLevel < initialData.size();
            ++mipLevel)
        {
            const auto& subresource = initialData[mipLevel];
            ValidateTexture3DSubresourceData(
                nativeDescription,
                static_cast<std::uint32_t>(mipLevel),
                subresource);
            nativeInitialData.push_back(
                D3D11_SUBRESOURCE_DATA{
                    subresource.bytes.data(),
                    subresource.rowPitch,
                    subresource.slicePitch
                });
        }

        Microsoft::WRL::ComPtr<ID3D11Texture3D> texture;
        ThrowIfFailed(
            m_device->CreateTexture3D(
                &nativeDescription,
                nativeInitialData.data(),
                texture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture3D");
        return Detail::GraphicsResourceHandleAccess::MakeTexture(
            std::make_shared<D3D11Texture3DPayload>(
                m_resourceDomain,
                std::move(texture),
                nativeDescription));
    }

    void D3D11Backend::UpdateTexture2D(
        const GraphicsTextureHandle& texture,
        const std::uint32_t mipLevel,
        const GraphicsTextureSubresourceData& data)
    {
        using Detail::GraphicsResourceHandleAccess;
        if (!IsInitialized() || m_resourceDomain == nullptr)
        {
            throw std::logic_error(
                "UpdateTexture2D requires an initialized backend.");
        }
        const auto* const payload =
            dynamic_cast<const D3D11TexturePayload*>(
                GraphicsResourceHandleAccess::Payload(texture));
        if (payload == nullptr
            || GraphicsResourceHandleAccess::Domain(texture)
                != m_resourceDomain.get()
            || payload->description.Usage != D3D11_USAGE_DEFAULT)
        {
            throw std::invalid_argument(
                "UpdateTexture2D requires valid data and a texture from "
                "this backend generation.");
        }
        ValidateTextureSubresourceData(
            payload->description,
            mipLevel,
            data);
        m_context->UpdateSubresource(
            payload->native.Get(),
            mipLevel,
            nullptr,
            data.bytes.data(),
            data.rowPitch,
            data.slicePitch != 0
                ? data.slicePitch
                : static_cast<UINT>(data.bytes.size()));
    }

    GraphicsViewHandle D3D11Backend::CreateShaderResourceView(
        const GraphicsTextureHandle& texture,
        const GraphicsTextureViewDescription& description)
    {
        using Detail::GraphicsResourceHandleAccess;
        if (!IsInitialized() || m_resourceDomain == nullptr)
        {
            throw std::logic_error(
                "CreateShaderResourceView requires an initialized backend.");
        }
        const auto* const texture2DPayload =
            dynamic_cast<const D3D11TexturePayload*>(
                GraphicsResourceHandleAccess::Payload(texture));
        const auto* const texture3DPayload =
            dynamic_cast<const D3D11Texture3DPayload*>(
                GraphicsResourceHandleAccess::Payload(texture));
        const auto availableMipLevels = texture2DPayload != nullptr
            ? texture2DPayload->description.MipLevels
            : texture3DPayload != nullptr
                ? texture3DPayload->description.MipLevels
                : 0u;
        if (availableMipLevels == 0
            || GraphicsResourceHandleAccess::Domain(texture)
                != m_resourceDomain.get()
            || description.mipLevels == 0
            || description.mostDetailedMip
                >= availableMipLevels
            || description.mipLevels
                > availableMipLevels
                    - description.mostDetailedMip)
        {
            throw std::invalid_argument(
                "CreateShaderResourceView requires a valid mip range and "
                "a texture from this backend generation.");
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC nativeDescription{};
        ID3D11Resource* nativeResource{};
        if (texture2DPayload != nullptr)
        {
            nativeResource = texture2DPayload->native.Get();
            nativeDescription.Format =
                texture2DPayload->description.Format;
            nativeDescription.ViewDimension =
                D3D11_SRV_DIMENSION_TEXTURE2D;
            nativeDescription.Texture2D.MostDetailedMip =
                description.mostDetailedMip;
            nativeDescription.Texture2D.MipLevels =
                description.mipLevels;
        }
        else
        {
            nativeResource = texture3DPayload->native.Get();
            nativeDescription.Format =
                texture3DPayload->description.Format;
            nativeDescription.ViewDimension =
                D3D11_SRV_DIMENSION_TEXTURE3D;
            nativeDescription.Texture3D.MostDetailedMip =
                description.mostDetailedMip;
            nativeDescription.Texture3D.MipLevels =
                description.mipLevels;
        }
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        ThrowIfFailed(
            m_device->CreateShaderResourceView(
                nativeResource,
                &nativeDescription,
                view.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateShaderResourceView");
        return GraphicsResourceHandleAccess::MakeView(
            std::make_shared<D3D11ViewPayload>(
                m_resourceDomain,
                GraphicsViewKind::ShaderResource,
                texture,
                std::move(view)));
    }

    bool D3D11Backend::UpdateDynamicVertexBuffer(
        GraphicsBufferHandle& buffer,
        const std::span<const std::byte> data)
    {
        using Detail::GraphicsResourceHandleAccess;
        if (!IsInitialized() || m_resourceDomain == nullptr)
        {
            throw std::logic_error(
                "UpdateDynamicVertexBuffer requires an initialized backend.");
        }

        const auto* payload = dynamic_cast<const D3D11BufferPayload*>(
            GraphicsResourceHandleAccess::Payload(buffer));
        if (buffer
            && (payload == nullptr
                || GraphicsResourceHandleAccess::Domain(buffer)
                    != m_resourceDomain.get()))
        {
            throw std::invalid_argument(
                "UpdateDynamicVertexBuffer requires a buffer from this "
                "backend generation.");
        }
        if (data.empty())
        {
            return false;
        }

        const std::size_t currentCapacity =
            payload != nullptr ? payload->capacity : 0;
        if (!buffer || currentCapacity < data.size())
        {
            constexpr std::size_t minimumCapacity = 4096;
            constexpr auto maximumCapacity =
                static_cast<std::size_t>(
                    std::numeric_limits<UINT>::max());
            if (data.size() > maximumCapacity)
            {
                return false;
            }
            const std::size_t doubledCapacity =
                currentCapacity > maximumCapacity / 2
                    ? maximumCapacity
                    : currentCapacity * 2;
            const std::size_t newCapacity = std::max({
                data.size(),
                doubledCapacity,
                minimumCapacity });

            D3D11_BUFFER_DESC description{};
            description.ByteWidth = static_cast<UINT>(newCapacity);
            description.Usage = D3D11_USAGE_DYNAMIC;
            description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            Microsoft::WRL::ComPtr<ID3D11Buffer> nativeBuffer;
            if (FAILED(m_device->CreateBuffer(
                    &description,
                    nullptr,
                    nativeBuffer.ReleaseAndGetAddressOf())))
            {
                return false;
            }

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(m_context->Map(
                    nativeBuffer.Get(),
                    0,
                    D3D11_MAP_WRITE_DISCARD,
                    0,
                    &mapped)))
            {
                return false;
            }
            std::memcpy(mapped.pData, data.data(), data.size());
            m_context->Unmap(nativeBuffer.Get(), 0);

            auto replacement =
                GraphicsResourceHandleAccess::MakeBuffer(
                    std::make_shared<D3D11BufferPayload>(
                        m_resourceDomain,
                        std::move(nativeBuffer),
                        newCapacity));
            buffer = std::move(replacement);
            return true;
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (payload == nullptr
            || FAILED(m_context->Map(
                payload->native.Get(),
                0,
                D3D11_MAP_WRITE_DISCARD,
                0,
                &mapped)))
        {
            return false;
        }
        std::memcpy(mapped.pData, data.data(), data.size());
        m_context->Unmap(payload->native.Get(), 0);
        return true;
    }

    void D3D11Backend::BindVertexBuffer(
        const GraphicsBufferHandle& buffer,
        const std::uint32_t slot,
        const std::uint32_t stride,
        const std::uint32_t offset)
    {
        if (!IsInitialized()
            || m_context == nullptr
            || m_resourceDomain == nullptr)
        {
            throw std::logic_error(
                "BindVertexBuffer requires an initialized backend.");
        }
        if (!buffer
            || slot >= D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT
            || stride == 0)
        {
            throw std::invalid_argument(
                "BindVertexBuffer requires a non-empty buffer, a valid "
                "slot, and a non-zero stride.");
        }

        auto* const nativeBuffer = ResolveBuffer(buffer);
        const UINT nativeStride = stride;
        const UINT nativeOffset = offset;
        m_context->IASetVertexBuffers(
            static_cast<UINT>(slot),
            1,
            &nativeBuffer,
            &nativeStride,
            &nativeOffset);
    }

    bool D3D11Backend::TryBindPixelShaderResources(
        const std::uint32_t firstSlot,
        const std::span<const GraphicsViewHandle> resources,
        const GraphicsViewHandle& fallback) noexcept
    {
        if (!IsInitialized()
            || m_context == nullptr
            || m_resourceDomain == nullptr)
        {
            return false;
        }

        constexpr std::size_t MaximumSlots =
            D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
        const auto first = static_cast<std::size_t>(firstSlot);
        if (first >= MaximumSlots
            || resources.size() > MaximumSlots - first)
        {
            return false;
        }
        if (resources.empty())
        {
            return true;
        }

        std::array<
            ID3D11ShaderResourceView*,
            D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT>
            nativeResources{};
        ID3D11ShaderResourceView* nativeFallback{};
        bool fallbackResolved = false;
        const auto resolveFallback = [&]() noexcept
        {
            if (fallbackResolved)
            {
                return nativeFallback;
            }
            fallbackResolved = true;
            if (!fallback)
            {
                return nativeFallback;
            }
            try
            {
                nativeFallback = ResolveShaderResourceView(fallback);
            }
            catch (...)
            {
                nativeFallback = nullptr;
            }
            return nativeFallback;
        };

        for (std::size_t index{}; index < resources.size(); ++index)
        {
            if (!resources[index])
            {
                nativeResources[index] = resolveFallback();
                continue;
            }
            try
            {
                nativeResources[index] =
                    ResolveShaderResourceView(resources[index]);
            }
            catch (...)
            {
                nativeResources[index] = resolveFallback();
            }
        }

        m_context->PSSetShaderResources(
            static_cast<UINT>(firstSlot),
            static_cast<UINT>(resources.size()),
            nativeResources.data());
        return true;
    }

    ID3D11ShaderResourceView* D3D11Backend::ResolveShaderResourceView(
        const GraphicsViewHandle& view) const
    {
        using Detail::GraphicsResourceHandleAccess;
        if (!view)
        {
            return nullptr;
        }
        const auto* const payload = dynamic_cast<const D3D11ViewPayload*>(
            GraphicsResourceHandleAccess::Payload(view));
        if (view.Kind() != GraphicsViewKind::ShaderResource
            || payload == nullptr
            || m_resourceDomain == nullptr
            || GraphicsResourceHandleAccess::Domain(view)
                != m_resourceDomain.get())
        {
            throw std::invalid_argument(
                "The shader-resource view does not belong to this "
                "backend generation.");
        }
        return payload->native.Get();
    }

    bool D3D11Backend::IsViewCurrent(
        const GraphicsViewHandle& view) const noexcept
    {
        using Detail::GraphicsResourceHandleAccess;
        return IsInitialized()
            && m_resourceDomain != nullptr
            && view
            && dynamic_cast<const D3D11ViewPayload*>(
                GraphicsResourceHandleAccess::Payload(view)) != nullptr
            && GraphicsResourceHandleAccess::Domain(view)
                == m_resourceDomain.get();
    }

    ID3D11Buffer* D3D11Backend::ResolveBuffer(
        const GraphicsBufferHandle& buffer) const
    {
        using Detail::GraphicsResourceHandleAccess;
        if (!buffer)
        {
            return nullptr;
        }
        const auto* const payload = dynamic_cast<const D3D11BufferPayload*>(
            GraphicsResourceHandleAccess::Payload(buffer));
        if (payload == nullptr
            || m_resourceDomain == nullptr
            || GraphicsResourceHandleAccess::Domain(buffer)
                != m_resourceDomain.get())
        {
            throw std::invalid_argument(
                "The buffer does not belong to this backend generation.");
        }
        return payload->native.Get();
    }

    std::pair<GraphicsTextureHandle, GraphicsViewHandle>
        D3D11Backend::ImportShaderResourceView(
            ID3D11ShaderResourceView* const view)
    {
        if (!IsInitialized() || m_resourceDomain == nullptr)
        {
            throw std::logic_error(
                "ImportShaderResourceView requires an initialized backend.");
        }
        if (view == nullptr)
        {
            throw std::invalid_argument(
                "ImportShaderResourceView requires a native view.");
        }

        Microsoft::WRL::ComPtr<ID3D11Device> ownerDevice;
        view->GetDevice(ownerDevice.ReleaseAndGetAddressOf());
        if (ownerDevice.Get() != m_device.Get())
        {
            throw std::invalid_argument(
                "The native shader-resource view belongs to another "
                "DirectX 11 device.");
        }

        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        view->GetResource(resource.ReleaseAndGetAddressOf());
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        ThrowIfFailed(
            resource.As(&texture),
            "ImportShaderResourceView(texture2D)");
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);

        auto textureHandle =
            Detail::GraphicsResourceHandleAccess::MakeTexture(
                std::make_shared<D3D11TexturePayload>(
                    m_resourceDomain,
                    std::move(texture),
                    description));
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ownedView = view;
        auto viewHandle =
            Detail::GraphicsResourceHandleAccess::MakeView(
                std::make_shared<D3D11ViewPayload>(
                    m_resourceDomain,
                    GraphicsViewKind::ShaderResource,
                    textureHandle,
                    std::move(ownedView)));
        return {
            std::move(textureHandle),
            std::move(viewHandle)
        };
    }

    GraphicsViewHandle D3D11Backend::ImportShaderResourceViewHandle(
        ID3D11ShaderResourceView* const view)
    {
        if (!IsInitialized() || m_resourceDomain == nullptr)
        {
            throw std::logic_error(
                "ImportShaderResourceViewHandle requires an initialized "
                "backend.");
        }
        if (view == nullptr)
        {
            throw std::invalid_argument(
                "ImportShaderResourceViewHandle requires a native view.");
        }

        Microsoft::WRL::ComPtr<ID3D11Device> ownerDevice;
        view->GetDevice(ownerDevice.ReleaseAndGetAddressOf());
        if (ownerDevice.Get() != m_device.Get())
        {
            throw std::invalid_argument(
                "The native shader-resource view belongs to another "
                "DirectX 11 device.");
        }

        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        view->GetResource(resource.ReleaseAndGetAddressOf());
        D3D11_RESOURCE_DIMENSION dimension{};
        resource->GetType(&dimension);
        if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        {
            auto imported = ImportShaderResourceView(view);
            return std::move(imported.second);
        }

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ownedView = view;
        switch (dimension)
        {
        case D3D11_RESOURCE_DIMENSION_BUFFER:
        {
            Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
            ThrowIfFailed(
                resource.As(&buffer),
                "ImportShaderResourceViewHandle(buffer)");
            D3D11_BUFFER_DESC description{};
            buffer->GetDesc(&description);
            auto bufferHandle =
                Detail::GraphicsResourceHandleAccess::MakeBuffer(
                    std::make_shared<D3D11BufferPayload>(
                        m_resourceDomain,
                        std::move(buffer),
                        description.ByteWidth));
            return Detail::GraphicsResourceHandleAccess::MakeView(
                std::make_shared<D3D11ViewPayload>(
                    m_resourceDomain,
                    GraphicsViewKind::ShaderResource,
                    std::move(bufferHandle),
                    std::move(ownedView)));
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
        {
            Microsoft::WRL::ComPtr<ID3D11Texture3D> texture;
            ThrowIfFailed(
                resource.As(&texture),
                "ImportShaderResourceViewHandle(texture3D)");
            D3D11_TEXTURE3D_DESC description{};
            texture->GetDesc(&description);
            auto textureHandle =
                Detail::GraphicsResourceHandleAccess::MakeTexture(
                    std::make_shared<D3D11Texture3DPayload>(
                        m_resourceDomain,
                        std::move(texture),
                        description));
            return Detail::GraphicsResourceHandleAccess::MakeView(
                std::make_shared<D3D11ViewPayload>(
                    m_resourceDomain,
                    GraphicsViewKind::ShaderResource,
                    std::move(textureHandle),
                    std::move(ownedView)));
        }
        case D3D11_RESOURCE_DIMENSION_UNKNOWN:
        case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
        default:
            throw std::invalid_argument(
                "The native shader-resource view uses an unsupported "
                "resource dimension.");
        }
    }

    GraphicsViewHandle D3D11Backend::CreateOffscreenDisplayView(
        const RenderTarget& target)
    {
        if (!IsInitialized() || m_resourceDomain == nullptr)
        {
            throw std::logic_error(
                "CreateOffscreenDisplayView requires an initialized backend.");
        }

        auto* const displayView =
            target.DisplayShaderResourceView();
        if (!target.IsValid() || displayView == nullptr)
        {
            throw std::invalid_argument(
                "CreateOffscreenDisplayView requires a valid offscreen "
                "target display view.");
        }

        auto imported = ImportShaderResourceView(displayView);
        return std::move(imported.second);
    }

    void D3D11Backend::BindAndClearBackBuffer(
        const float clearColor[4])
    {
        ID3D11RenderTargetView* renderTargets[]{
            m_renderTargetView.Get() };
        m_context->OMSetRenderTargets(
            1,
            renderTargets,
            m_depthStencilView.Get());
        m_context->RSSetViewports(1, &m_viewport);
        m_context->ClearRenderTargetView(
            m_renderTargetView.Get(),
            clearColor);
        m_context->ClearDepthStencilView(
            m_depthStencilView.Get(),
            D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
            1.0f,
            0);
    }

    void D3D11Backend::DrainDebugMessages()
    {
        if (!m_infoQueue)
        {
            return;
        }

        const auto stored =
            m_infoQueue->GetNumStoredMessages();
        for (UINT64 index = 0; index < stored; ++index)
        {
            SIZE_T length = 0;
            if (FAILED(m_infoQueue->GetMessage(
                    index,
                    nullptr,
                    &length))
                || length == 0)
            {
                continue;
            }
            std::vector<std::byte> storage(length);
            auto* const message =
                reinterpret_cast<D3D11_MESSAGE*>(
                    storage.data());
            if (FAILED(m_infoQueue->GetMessage(
                    index,
                    message,
                    &length)))
            {
                continue;
            }

            const std::string text(
                message->pDescription,
                message->DescriptionByteLength > 0
                    ? message->DescriptionByteLength - 1
                    : 0);
            ++m_debugMessagesLogged;
            switch (message->Severity)
            {
            case D3D11_MESSAGE_SEVERITY_CORRUPTION:
            case D3D11_MESSAGE_SEVERITY_ERROR:
                Logger::Instance().Error("D3D11: " + text);
                break;
            case D3D11_MESSAGE_SEVERITY_WARNING:
                Logger::Instance().Warning("D3D11: " + text);
                break;
            default:
                Logger::Instance().Info("D3D11: " + text);
                break;
            }
        }
        m_infoQueue->ClearStoredMessages();
    }

    void D3D11Backend::Present(const bool vSyncEnabled)
    {
        // ALLOW_TEARINGは同期間隔0と組み合わせた時だけ有効です。
        const bool immediate = !vSyncEnabled;
        const HRESULT presented = m_swapChain->Present(
            immediate ? 0u : 1u,
            (immediate && m_tearingAllowed)
                ? DXGI_PRESENT_ALLOW_TEARING
                : 0u);

        if (presented == DXGI_ERROR_DEVICE_REMOVED
            || presented == DXGI_ERROR_DEVICE_RESET)
        {
            const HRESULT reason = m_device
                ? m_device->GetDeviceRemovedReason()
                : presented;
            std::ostringstream message;
            message
                << "The graphics device was lost while"
                   " presenting a frame (Present=0x"
                << std::hex << std::uppercase
                << static_cast<unsigned long>(presented)
                << ", reason=0x"
                << static_cast<unsigned long>(reason)
                << "). This usually means the driver rejected"
                   " the previous draw call. Run with"
                   " --d3ddebug to see which one.";
            throw std::runtime_error(message.str());
        }
        ThrowIfFailed(presented, "IDXGISwapChain::Present");
    }

    std::vector<std::uint8_t>
        D3D11Backend::CaptureBackBuffer(
            std::uint32_t& width,
            std::uint32_t& height) const
    {
        if (!IsInitialized() || m_swapChain == nullptr)
        {
            throw std::logic_error(
                "CaptureBackBuffer requires an initialized device.");
        }

        Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
        ThrowIfFailed(
            m_swapChain->GetBuffer(
                0,
                IID_PPV_ARGS(
                    backBuffer.ReleaseAndGetAddressOf())),
            "IDXGISwapChain::GetBuffer");

        D3D11_TEXTURE2D_DESC description{};
        backBuffer->GetDesc(&description);
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags =
            D3D11_CPU_ACCESS_READ;
        description.MiscFlags = 0;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        ThrowIfFailed(
            m_device->CreateTexture2D(
                &description,
                nullptr,
                staging.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(staging)");
        m_context->CopyResource(
            staging.Get(),
            backBuffer.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        ThrowIfFailed(
            m_context->Map(
                staging.Get(),
                0,
                D3D11_MAP_READ,
                0,
                &mapped),
            "ID3D11DeviceContext::Map(staging)");

        width = description.Width;
        height = description.Height;
        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(width)
            * height
            * 4);
        for (std::uint32_t row = 0; row < height; ++row)
        {
            std::memcpy(
                pixels.data()
                    + static_cast<std::size_t>(row)
                        * width * 4,
                static_cast<const std::uint8_t*>(
                    mapped.pData)
                    + static_cast<std::size_t>(row)
                        * mapped.RowPitch,
                static_cast<std::size_t>(width) * 4);
        }
        m_context->Unmap(staging.Get(), 0);
        return pixels;
    }

    void D3D11Backend::CreateSizeDependentResources(
        const std::uint32_t width,
        const std::uint32_t height)
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
        ThrowIfFailed(
            m_swapChain->GetBuffer(
                0,
                IID_PPV_ARGS(
                    backBuffer.ReleaseAndGetAddressOf())),
            "IDXGISwapChain::GetBuffer");

        ThrowIfFailed(
            m_device->CreateRenderTargetView(
                backBuffer.Get(),
                nullptr,
                m_renderTargetView.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateRenderTargetView");

        D3D11_TEXTURE2D_DESC depthDescription{};
        depthDescription.Width = width;
        depthDescription.Height = height;
        depthDescription.MipLevels = 1;
        depthDescription.ArraySize = 1;
        depthDescription.Format =
            DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthDescription.SampleDesc.Count = 1;
        depthDescription.BindFlags =
            D3D11_BIND_DEPTH_STENCIL;

        ThrowIfFailed(
            m_device->CreateTexture2D(
                &depthDescription,
                nullptr,
                m_depthTexture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(depth)");
        ThrowIfFailed(
            m_device->CreateDepthStencilView(
                m_depthTexture.Get(),
                nullptr,
                m_depthStencilView.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateDepthStencilView");

        m_viewport.TopLeftX = 0.0f;
        m_viewport.TopLeftY = 0.0f;
        m_viewport.Width = static_cast<float>(width);
        m_viewport.Height = static_cast<float>(height);
        m_viewport.MinDepth = 0.0f;
        m_viewport.MaxDepth = 1.0f;
    }

    void D3D11Backend::LogSelectedAdapter() const
    {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(m_device.As(&dxgiDevice)))
        {
            return;
        }
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDevice->GetAdapter(
                adapter.GetAddressOf()))
            || adapter == nullptr)
        {
            return;
        }
        DXGI_ADAPTER_DESC description{};
        if (FAILED(adapter->GetDesc(&description)))
        {
            return;
        }

        const bool isWarp =
            description.VendorId == 0x1414u
            && description.DeviceId == 0x8cu;
        std::string name;
        for (const auto character : description.Description)
        {
            if (character == L'\0')
            {
                break;
            }
            name.push_back(
                character < 128
                    ? static_cast<char>(character)
                    : '?');
        }
        const auto videoMemoryMegabytes =
            static_cast<std::uint64_t>(
                description.DedicatedVideoMemory)
            / (1024u * 1024u);

        auto message = "描画アダプター: " + name
            + "（VRAM " + std::to_string(
                videoMemoryMegabytes)
            + " MB）";
        if (isWarp)
        {
            Logger::Instance().Warning(
                message
                + " ※WARP（CPU描画）です。GPUを使っていないため"
                  "描画性能は大幅に低下します。");
        }
        else
        {
            Logger::Instance().Info(message);
        }
    }
}
