#include "LamaPon/Graphics/D3D11Backend.h"

#include "LamaPon/Core/Log.h"
#include "LamaPon/Graphics/ClusteredLights.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/ShadowMap.h"

// IDXGIFactory5（ティアリング許可の問い合わせ）。d3d11.hが引く
// dxgi.hには入っていません。
#include <dxgi1_5.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
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

        shadowMap.Begin(m_context.Get(), cascadeIndex);
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

        shadowMap.End(m_context.Get());
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
