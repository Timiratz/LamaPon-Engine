#include "LamaPon/Graphics/EnvironmentRenderer.h"
#include "LamaPon/Graphics/EnvironmentCache.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/ShaderCompiler.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/PathUtils.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
    void ThrowIfFailed(const HRESULT result, const char* operation)
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

    Microsoft::WRL::ComPtr<ID3DBlob> CompileShader(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& path,
        const char* entryPoint,
        const char* target)
    {
        // ShaderCompilerを通し、コンパイル結果をディスクキャッシュから再利用します。
        return LamaPon::CompileShaderCached(
            assets,
            path,
            entryPoint,
            target);
    }

    template<typename T>
    Microsoft::WRL::ComPtr<ID3D11Buffer>
        CreateConstantBuffer(ID3D11Device* device)
    {
        static_assert(sizeof(T) % 16 == 0);
        D3D11_BUFFER_DESC description{};
        description.ByteWidth =
            static_cast<UINT>(sizeof(T));
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags =
            D3D11_BIND_CONSTANT_BUFFER;
        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
        ThrowIfFailed(
            device->CreateBuffer(
                &description,
                nullptr,
                buffer.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateBuffer(environment)");
        return buffer;
    }
}

namespace LamaPon
{
    EnvironmentRenderer::EnvironmentRenderer(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        AssetManager& assets,
        const std::filesystem::path& shaderPath)
        : m_device(device)
        , m_context(context)
    {
        if (device == nullptr || context == nullptr)
        {
            throw std::invalid_argument(
                "EnvironmentRenderer requires a Direct3D device and context.");
        }

        const auto vertexByteCode =
            CompileShader(assets, shaderPath, "VSMain", "vs_5_0");
        const auto skyByteCode =
            CompileShader(assets, shaderPath, "PSSky", "ps_5_0");
        const auto bloomByteCode =
            CompileShader(assets, shaderPath, "PSBloom", "ps_5_0");
        const auto screenOutlineByteCode = CompileShader(
            assets,
            shaderPath,
            "PSScreenOutline",
            "ps_5_0");
        const auto lensFlareByteCode = CompileShader(
            assets,
            shaderPath,
            "PSScreenSpaceLensFlare",
            "ps_5_0");
        const auto lensFlareStreakByteCode = CompileShader(
            assets,
            shaderPath,
            "PSLensFlareStreak",
            "ps_5_0");
        const auto toneMapByteCode =
            CompileShader(assets, shaderPath, "PSToneMap", "ps_5_0");
        const auto fxaaByteCode =
            CompileShader(assets, shaderPath, "PSFXAA", "ps_5_0");
        const auto copyByteCode =
            CompileShader(assets, shaderPath, "PSCopy", "ps_5_0");
        const auto copyMirrorByteCode = CompileShader(
            assets,
            shaderPath,
            "PSCopyMirrorX",
            "ps_5_0");
        const auto temporalByteCode = CompileShader(
            assets,
            shaderPath,
            "PSTemporalAntiAliasing",
            "ps_5_0");
        const auto volumetricByteCode = CompileShader(
            assets,
            shaderPath,
            "PSVolumetricLight",
            "ps_5_0");
        const auto depthOfFieldPrepareByteCode = CompileShader(
            assets,
            shaderPath,
            "PSDepthOfFieldPrepare",
            "ps_5_0");
        const auto depthOfFieldBlurByteCode = CompileShader(
            assets,
            shaderPath,
            "PSDepthOfFieldBlur",
            "ps_5_0");
        const auto depthOfFieldCompositeByteCode = CompileShader(
            assets,
            shaderPath,
            "PSDepthOfFieldComposite",
            "ps_5_0");
        const auto motionBlurByteCode = CompileShader(
            assets,
            shaderPath,
            "PSMotionBlur",
            "ps_5_0");
        const auto luminanceByteCode = CompileShader(
            assets,
            shaderPath,
            "PSLuminance",
            "ps_5_0");
        const auto ambientOcclusionByteCode = CompileShader(
            assets,
            shaderPath,
            "PSAmbientOcclusion",
            "ps_5_0");
        const auto ambientOcclusionBlurByteCode = CompileShader(
            assets,
            shaderPath,
            "PSAmbientOcclusionBlur",
            "ps_5_0");
        const auto prefilterByteCode = CompileShader(
            assets,
            shaderPath,
            "PSPrefilterEnvironment",
            "ps_5_0");
        const auto irradianceByteCode = CompileShader(
            assets,
            shaderPath,
            "PSIrradiance",
            "ps_5_0");
        const auto reflectionLinearizeByteCode =
            CompileShader(
                assets,
                shaderPath,
                "PSReflectionDepthLinearize",
                "ps_5_0");
        const auto reflectionDownsampleByteCode =
            CompileShader(
                assets,
                shaderPath,
                "PSReflectionDepthDownsample",
                "ps_5_0");
        ThrowIfFailed(
            device->CreateVertexShader(
                vertexByteCode->GetBufferPointer(),
                vertexByteCode->GetBufferSize(),
                nullptr,
                m_vertexShader.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateVertexShader(environment)");
        ThrowIfFailed(
            device->CreatePixelShader(
                skyByteCode->GetBufferPointer(),
                skyByteCode->GetBufferSize(),
                nullptr,
                m_skyPixelShader.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(sky)");
        ThrowIfFailed(
            device->CreatePixelShader(
                bloomByteCode->GetBufferPointer(),
                bloomByteCode->GetBufferSize(),
                nullptr,
                m_bloomPixelShader.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(bloom)");
        ThrowIfFailed(
            device->CreatePixelShader(
                screenOutlineByteCode->GetBufferPointer(),
                screenOutlineByteCode->GetBufferSize(),
                nullptr,
                m_screenOutlinePixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(screen outline)");
        ThrowIfFailed(
            device->CreatePixelShader(
                lensFlareByteCode->GetBufferPointer(),
                lensFlareByteCode->GetBufferSize(),
                nullptr,
                m_lensFlarePixelShader.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(screen space lens flare)");
        ThrowIfFailed(
            device->CreatePixelShader(
                lensFlareStreakByteCode->GetBufferPointer(),
                lensFlareStreakByteCode->GetBufferSize(),
                nullptr,
                m_lensFlareStreakPixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(lens flare streak)");
        ThrowIfFailed(
            device->CreatePixelShader(
                toneMapByteCode->GetBufferPointer(),
                toneMapByteCode->GetBufferSize(),
                nullptr,
                m_toneMapPixelShader.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(tone map)");
        ThrowIfFailed(
            device->CreatePixelShader(
                fxaaByteCode->GetBufferPointer(),
                fxaaByteCode->GetBufferSize(),
                nullptr,
                m_fxaaPixelShader.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(FXAA)");
        ThrowIfFailed(
            device->CreatePixelShader(
                copyByteCode->GetBufferPointer(),
                copyByteCode->GetBufferSize(),
                nullptr,
                m_copyPixelShader.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(copy)");
        ThrowIfFailed(
            device->CreatePixelShader(
                copyMirrorByteCode->GetBufferPointer(),
                copyMirrorByteCode->GetBufferSize(),
                nullptr,
                m_copyMirrorPixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(copy mirror)");
        ThrowIfFailed(
            device->CreatePixelShader(
                volumetricByteCode->GetBufferPointer(),
                volumetricByteCode->GetBufferSize(),
                nullptr,
                m_volumetricPixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(volumetric)");
        ThrowIfFailed(
            device->CreatePixelShader(
                temporalByteCode->GetBufferPointer(),
                temporalByteCode->GetBufferSize(),
                nullptr,
                m_temporalPixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(TAA)");
        ThrowIfFailed(
            device->CreatePixelShader(
                depthOfFieldPrepareByteCode
                    ->GetBufferPointer(),
                depthOfFieldPrepareByteCode->GetBufferSize(),
                nullptr,
                m_depthOfFieldPreparePixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader"
            "(depth of field prepare)");
        ThrowIfFailed(
            device->CreatePixelShader(
                depthOfFieldBlurByteCode->GetBufferPointer(),
                depthOfFieldBlurByteCode->GetBufferSize(),
                nullptr,
                m_depthOfFieldBlurPixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader"
            "(depth of field blur)");
        ThrowIfFailed(
            device->CreatePixelShader(
                depthOfFieldCompositeByteCode
                    ->GetBufferPointer(),
                depthOfFieldCompositeByteCode
                    ->GetBufferSize(),
                nullptr,
                m_depthOfFieldCompositePixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader"
            "(depth of field composite)");
        ThrowIfFailed(
            device->CreatePixelShader(
                motionBlurByteCode->GetBufferPointer(),
                motionBlurByteCode->GetBufferSize(),
                nullptr,
                m_motionBlurPixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(motion blur)");
        ThrowIfFailed(
            device->CreatePixelShader(
                luminanceByteCode->GetBufferPointer(),
                luminanceByteCode->GetBufferSize(),
                nullptr,
                m_luminancePixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(luminance)");
        ThrowIfFailed(
            device->CreatePixelShader(
                ambientOcclusionByteCode->GetBufferPointer(),
                ambientOcclusionByteCode->GetBufferSize(),
                nullptr,
                m_ambientOcclusionPixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(SSAO)");
        ThrowIfFailed(
            device->CreatePixelShader(
                ambientOcclusionBlurByteCode->GetBufferPointer(),
                ambientOcclusionBlurByteCode->GetBufferSize(),
                nullptr,
                m_ambientOcclusionBlurPixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(SSAO blur)");

        ThrowIfFailed(
            device->CreatePixelShader(
                prefilterByteCode->GetBufferPointer(),
                prefilterByteCode->GetBufferSize(),
                nullptr,
                m_prefilterPixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(prefilter)");
        ThrowIfFailed(
            device->CreatePixelShader(
                irradianceByteCode->GetBufferPointer(),
                irradianceByteCode->GetBufferSize(),
                nullptr,
                m_irradiancePixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(irradiance)");
        ThrowIfFailed(
            device->CreatePixelShader(
                reflectionLinearizeByteCode
                    ->GetBufferPointer(),
                reflectionLinearizeByteCode
                    ->GetBufferSize(),
                nullptr,
                m_reflectionLinearizePixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(hi-z linearize)");
        ThrowIfFailed(
            device->CreatePixelShader(
                reflectionDownsampleByteCode
                    ->GetBufferPointer(),
                reflectionDownsampleByteCode
                    ->GetBufferSize(),
                nullptr,
                m_reflectionDownsamplePixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(hi-z downsample)");

        m_skyBuffer =
            CreateConstantBuffer<SkyConstants>(device);
        m_prefilterBuffer =
            CreateConstantBuffer<PrefilterConstants>(
                device);
        m_bloomBuffer =
            CreateConstantBuffer<BloomConstants>(device);
        m_screenOutlineBuffer =
            CreateConstantBuffer<ScreenOutlineConstants>(device);
        m_lensFlareBuffer =
            CreateConstantBuffer<LensFlareConstants>(device);
        m_colorGradingBuffer =
            CreateConstantBuffer<ColorGradingConstants>(device);
        m_ambientOcclusionBuffer =
            CreateConstantBuffer<AmbientOcclusionConstants>(
                device);
        m_volumetricBuffer =
            CreateConstantBuffer<VolumetricConstants>(
                device);
        m_temporalBuffer =
            CreateConstantBuffer<TemporalConstants>(
                device);
        m_depthOfFieldBuffer =
            CreateConstantBuffer<DepthOfFieldConstants>(
                device);
        m_motionBlurBuffer =
            CreateConstantBuffer<MotionBlurConstants>(
                device);
        m_luminanceBuffer =
            CreateConstantBuffer<LuminanceConstants>(
                device);

        // ボリュメトリック用の影サンプラー。Litシェーダーと同じ
        // 比較サンプラーで、範囲外は「光が届いている」（1.0）に
        // します。
        D3D11_SAMPLER_DESC volumetricShadow{};
        volumetricShadow.Filter =
            D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        volumetricShadow.AddressU =
            D3D11_TEXTURE_ADDRESS_BORDER;
        volumetricShadow.AddressV =
            D3D11_TEXTURE_ADDRESS_BORDER;
        volumetricShadow.AddressW =
            D3D11_TEXTURE_ADDRESS_BORDER;
        volumetricShadow.BorderColor[0] = 1.0f;
        volumetricShadow.BorderColor[1] = 1.0f;
        volumetricShadow.BorderColor[2] = 1.0f;
        volumetricShadow.BorderColor[3] = 1.0f;
        volumetricShadow.ComparisonFunc =
            D3D11_COMPARISON_LESS_EQUAL;
        volumetricShadow.MinLOD = 0.0f;
        volumetricShadow.MaxLOD = 0.0f;
        ThrowIfFailed(
            device->CreateSamplerState(
                &volumetricShadow,
                m_volumetricShadowSampler
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateSamplerState"
            "(volumetric shadow)");

        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = std::numeric_limits<float>::max();
        ThrowIfFailed(
            device->CreateSamplerState(
                &sampler,
                m_sampler.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateSamplerState(environment)");

        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = FALSE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        ThrowIfFailed(
            device->CreateDepthStencilState(
                &depth,
                m_depthDisabled.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateDepthStencilState(environment)");

        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = TRUE;
        ThrowIfFailed(
            device->CreateRasterizerState(
                &rasterizer,
                m_rasterizer.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateRasterizerState(environment)");
    }

    EnvironmentRenderer::PrefilteredEnvironment
        EnvironmentRenderer::GetPrefilteredEnvironment(
            ID3D11ShaderResourceView* const source,
            const std::uint64_t cacheKey)
    {
        if (source == nullptr)
        {
            return {};
        }
        if (m_prefilterSource.Get() != source)
        {
            BuildPrefilteredEnvironment(source, cacheKey);
        }
        return {
            m_prefilteredSpecular.Get(),
            m_prefilteredIrradiance.Get(),
            m_prefilteredMaximumMip
        };
    }

    void EnvironmentRenderer::BuildPrefilteredEnvironment(
        ID3D11ShaderResourceView* const source,
        const std::uint64_t cacheKey)
    {
        // スカイ用キャッシュが有効な場合は、決定的な畳み込み結果を
        // ディスクから復元してGPUでの再計算を省略します。
        m_prefilterSource = source;
        if (cacheKey != 0)
        {
            auto restored = EnvironmentCache::TryLoad(
                m_device,
                cacheKey);
            if (restored.IsValid())
            {
                m_prefilteredSpecular =
                    std::move(restored.specular);
                m_prefilteredIrradiance =
                    std::move(restored.irradiance);
                m_prefilteredMaximumMip =
                    restored.specularMaximumMip;
                return;
            }
        }
        auto owned = CreatePrefilteredEnvironment(source);
        if (cacheKey != 0 && owned.IsValid())
        {
            EnvironmentCache::Store(
                cacheKey,
                m_device,
                m_context,
                owned);
        }
        m_prefilteredSpecular =
            std::move(owned.specular);
        m_prefilteredIrradiance =
            std::move(owned.irradiance);
        m_prefilteredMaximumMip =
            owned.specularMaximumMip;
    }

    EnvironmentRenderer::OwnedPrefilteredEnvironment
        EnvironmentRenderer::CreatePrefilteredEnvironment(
            ID3D11ShaderResourceView* const source,
            const bool includeSpecular)
    {
        using Microsoft::WRL::ComPtr;

        OwnedPrefilteredEnvironment result;
        if (source == nullptr)
        {
            return result;
        }

        // ソース解像度を取得します。
        ComPtr<ID3D11Resource> resource;
        source->GetResource(
            resource.ReleaseAndGetAddressOf());
        ComPtr<ID3D11Texture2D> sourceTexture;
        if (FAILED(resource.As(&sourceTexture)))
        {
            return result;
        }
        D3D11_TEXTURE2D_DESC sourceDescription{};
        sourceTexture->GetDesc(&sourceDescription);

        // スペキュラ: 128px・8ミップ、放射照度: 16px・1ミップ。
        constexpr std::uint32_t SpecularSize = 128;
        constexpr std::uint32_t SpecularMips = 8;
        constexpr std::uint32_t IrradianceSize = 16;

        const auto createCube =
            [this](
                const std::uint32_t size,
                const std::uint32_t mips,
                ComPtr<ID3D11Texture2D>& texture,
                ComPtr<ID3D11ShaderResourceView>& view)
        {
            D3D11_TEXTURE2D_DESC description{};
            description.Width = size;
            description.Height = size;
            description.MipLevels = mips;
            description.ArraySize = 6;
            description.Format =
                DXGI_FORMAT_R16G16B16A16_FLOAT;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_DEFAULT;
            description.BindFlags =
                D3D11_BIND_SHADER_RESOURCE
                | D3D11_BIND_RENDER_TARGET;
            description.MiscFlags =
                D3D11_RESOURCE_MISC_TEXTURECUBE;
            ThrowIfFailed(
                m_device->CreateTexture2D(
                    &description,
                    nullptr,
                    texture.ReleaseAndGetAddressOf()),
                "ID3D11Device::CreateTexture2D(prefilter)");
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
            viewDescription.Format = description.Format;
            viewDescription.ViewDimension =
                D3D11_SRV_DIMENSION_TEXTURECUBE;
            viewDescription.TextureCube.MipLevels = mips;
            ThrowIfFailed(
                m_device->CreateShaderResourceView(
                    texture.Get(),
                    &viewDescription,
                    view.ReleaseAndGetAddressOf()),
                "ID3D11Device::CreateShaderResourceView(prefilter)");
        };

        ComPtr<ID3D11Texture2D> specularTexture;
        ComPtr<ID3D11Texture2D> irradianceTexture;
        // 照度だけを要求する呼び出しでは、格子点ごとの主な計算負荷となる
        // スペキュラ畳み込みを省略します。
        if (includeSpecular)
        {
            createCube(
                SpecularSize,
                SpecularMips,
                specularTexture,
                result.specular);
        }
        createCube(
            IrradianceSize,
            1,
            irradianceTexture,
            result.irradiance);

        // 現在のパイプライン状態を退避します
        // （シーン描画の途中で呼ばれるため）。
        ComPtr<ID3D11RenderTargetView> previousTarget;
        ComPtr<ID3D11DepthStencilView> previousDepth;
        m_context->OMGetRenderTargets(
            1,
            previousTarget.ReleaseAndGetAddressOf(),
            previousDepth.ReleaseAndGetAddressOf());
        D3D11_VIEWPORT previousViewport{};
        UINT viewportCount = 1;
        m_context->RSGetViewports(
            &viewportCount,
            &previousViewport);
        ComPtr<ID3D11DepthStencilState>
            previousDepthState;
        UINT previousStencilReference{};
        m_context->OMGetDepthStencilState(
            previousDepthState.ReleaseAndGetAddressOf(),
            &previousStencilReference);
        ComPtr<ID3D11RasterizerState> previousRasterizer;
        m_context->RSGetState(
            previousRasterizer.ReleaseAndGetAddressOf());

        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(
            m_vertexShader.Get(),
            nullptr,
            0);
        ID3D11ShaderResourceView* sourceResources[]{
            source };
        m_context->PSSetShaderResources(
            1,
            1,
            sourceResources);
        ID3D11SamplerState* samplers[]{
            m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        ID3D11Buffer* buffers[]{
            m_prefilterBuffer.Get() };
        m_context->PSSetConstantBuffers(3, 1, buffers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(),
            0);
        m_context->RSSetState(m_rasterizer.Get());

        const auto renderFaces =
            [this](
                ID3D11Texture2D* texture,
                ID3D11PixelShader* shader,
                const std::uint32_t size,
                const std::uint32_t mips,
                const float sourceResolution)
        {
            m_context->PSSetShader(shader, nullptr, 0);
            for (std::uint32_t mip = 0;
                mip < mips;
                ++mip)
            {
                const float mipSize = static_cast<float>(
                    std::max(size >> mip, 1u));
                D3D11_VIEWPORT viewport{};
                viewport.Width = mipSize;
                viewport.Height = mipSize;
                viewport.MaxDepth = 1.0f;
                m_context->RSSetViewports(1, &viewport);
                const float roughness =
                    mips <= 1
                        ? 0.0f
                        : static_cast<float>(mip)
                            / static_cast<float>(
                                mips - 1);
                for (std::uint32_t face = 0;
                    face < 6;
                    ++face)
                {
                    PrefilterConstants constants{};
                    constants.parameters = {
                        static_cast<float>(face),
                        roughness,
                        sourceResolution,
                        0.0f
                    };
                    m_context->UpdateSubresource(
                        m_prefilterBuffer.Get(),
                        0,
                        nullptr,
                        &constants,
                        0,
                        0);

                    D3D11_RENDER_TARGET_VIEW_DESC
                        targetDescription{};
                    targetDescription.Format =
                        DXGI_FORMAT_R16G16B16A16_FLOAT;
                    targetDescription.ViewDimension =
                        D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                    targetDescription.Texture2DArray
                        .MipSlice = mip;
                    targetDescription.Texture2DArray
                        .FirstArraySlice = face;
                    targetDescription.Texture2DArray
                        .ArraySize = 1;
                    Microsoft::WRL::ComPtr<
                        ID3D11RenderTargetView> target;
                    ThrowIfFailed(
                        m_device->CreateRenderTargetView(
                            texture,
                            &targetDescription,
                            target
                                .ReleaseAndGetAddressOf()),
                        "ID3D11Device::CreateRenderTargetView(prefilter)");
                    ID3D11RenderTargetView* targets[]{
                        target.Get() };
                    m_context->OMSetRenderTargets(
                        1,
                        targets,
                        nullptr);
                    m_context->Draw(3, 0);
                }
            }
        };

        if (includeSpecular)
        {
            renderFaces(
                specularTexture.Get(),
                m_prefilterPixelShader.Get(),
                SpecularSize,
                SpecularMips,
                static_cast<float>(sourceDescription.Width));
        }
        renderFaces(
            irradianceTexture.Get(),
            m_irradiancePixelShader.Get(),
            IrradianceSize,
            1,
            static_cast<float>(sourceDescription.Width));

        // 状態を復元します。
        ID3D11ShaderResourceView* clearResources[]{
            nullptr };
        m_context->PSSetShaderResources(
            1,
            1,
            clearResources);
        ID3D11RenderTargetView* restoreTargets[]{
            previousTarget.Get() };
        m_context->OMSetRenderTargets(
            1,
            restoreTargets,
            previousDepth.Get());
        if (viewportCount > 0)
        {
            m_context->RSSetViewports(
                1,
                &previousViewport);
        }
        m_context->OMSetDepthStencilState(
            previousDepthState.Get(),
            previousStencilReference);
        m_context->RSSetState(previousRasterizer.Get());

        result.specularMaximumMip =
            static_cast<float>(SpecularMips - 1);
        return result;
    }

    void EnvironmentRenderer::DrawSky(
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection,
        const SkySettings& settings,
        ID3D11ShaderResourceView* cubemap,
        const SkySun* sun)
    {
        if (!settings.enabled)
        {
            return;
        }
        using namespace DirectX;
        XMVECTOR determinant{};
        const XMMATRIX inverseViewProjection =
            XMMatrixInverse(
                &determinant,
                view * projection);
        const XMMATRIX inverseView =
            XMMatrixInverse(&determinant, view);
        SkyConstants constants{};
        XMStoreFloat4x4(
            &constants.inverseViewProjection,
            inverseViewProjection);
        XMStoreFloat4(
            &constants.cameraPosition,
            inverseView.r[3]);
        constants.topColor = {
            settings.topColor.x,
            settings.topColor.y,
            settings.topColor.z,
            settings.intensity
        };
        constants.horizonColor = {
            settings.horizonColor.x,
            settings.horizonColor.y,
            settings.horizonColor.z,
            1.0f
        };
        constants.groundColor = {
            settings.groundColor.x,
            settings.groundColor.y,
            settings.groundColor.z,
            1.0f
        };
        if (sun != nullptr)
        {
            const auto length = std::sqrt(
                sun->directionToSun.x * sun->directionToSun.x
                + sun->directionToSun.y * sun->directionToSun.y
                + sun->directionToSun.z
                    * sun->directionToSun.z);
            const float scale =
                length > 0.0001f ? 1.0f / length : 0.0f;
            constants.sunDirection = {
                sun->directionToSun.x * scale,
                sun->directionToSun.y * scale,
                sun->directionToSun.z * scale,
                // 角半径0でも太陽円盤が消えないよう、描画時の半径には
                // 太陽相当の最小値を適用します。
                std::max(sun->angularRadius, 0.004625f)
            };
            constants.sunDiskColor = {
                sun->color.x,
                sun->color.y,
                sun->color.z,
                1.0f
            };
        }
        constants.options = {
            cubemap != nullptr ? 1.0f : 0.0f,
            0.0f,
            0.0f,
            0.0f
        };
        m_context->UpdateSubresource(
            m_skyBuffer.Get(), 0, nullptr, &constants, 0, 0);

        Microsoft::WRL::ComPtr<
            ID3D11DepthStencilState> previousDepth;
        UINT previousStencilReference{};
        m_context->OMGetDepthStencilState(
            previousDepth.ReleaseAndGetAddressOf(),
            &previousStencilReference);
        Microsoft::WRL::ComPtr<
            ID3D11RasterizerState> previousRasterizer;
        m_context->RSGetState(
            previousRasterizer.ReleaseAndGetAddressOf());
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(
            m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(
            m_skyPixelShader.Get(), nullptr, 0);
        ID3D11Buffer* buffers[]{ m_skyBuffer.Get() };
        m_context->PSSetConstantBuffers(0, 1, buffers);
        ID3D11ShaderResourceView* skyResources[]{
            cubemap };
        m_context->PSSetShaderResources(
            1,
            1,
            skyResources);
        ID3D11SamplerState* skySamplers[]{
            m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, skySamplers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(), 0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);
        ID3D11ShaderResourceView* clearResources[]{
            nullptr };
        m_context->PSSetShaderResources(
            1,
            1,
            clearResources);
        m_context->OMSetDepthStencilState(
            previousDepth.Get(),
            previousStencilReference);
        m_context->RSSetState(
            previousRasterizer.Get());
    }

    void EnvironmentRenderer::ApplyBloom(
        ID3D11ShaderResourceView* source,
        ID3D11RenderTargetView* destination,
        const std::uint32_t width,
        const std::uint32_t height,
        const BloomSettings& settings)
    {
        if (source == nullptr || destination == nullptr)
        {
            return;
        }
        const BloomConstants constants{
            {
                1.0f / static_cast<float>(std::max(width, 1u)),
                1.0f / static_cast<float>(std::max(height, 1u))
            },
            std::clamp(settings.threshold, 0.0f, 4.0f),
            settings.enabled
                ? std::clamp(settings.intensity, 0.0f, 8.0f)
                : 0.0f,
            std::clamp(settings.radius, 0.25f, 12.0f),
            {}
        };
        m_context->UpdateSubresource(
            m_bloomBuffer.Get(), 0, nullptr, &constants, 0, 0);

        ID3D11RenderTargetView* targets[]{ destination };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        D3D11_VIEWPORT viewport{
            0.0f,
            0.0f,
            static_cast<float>(std::max(width, 1u)),
            static_cast<float>(std::max(height, 1u)),
            0.0f,
            1.0f
        };
        m_context->RSSetViewports(1, &viewport);
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(
            m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(
            m_bloomPixelShader.Get(), nullptr, 0);
        ID3D11Buffer* buffers[]{ m_bloomBuffer.Get() };
        m_context->PSSetConstantBuffers(1, 1, buffers);
        ID3D11ShaderResourceView* resources[]{ source };
        m_context->PSSetShaderResources(0, 1, resources);
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(), 0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);
        ID3D11ShaderResourceView* nullResource[]{ nullptr };
        m_context->PSSetShaderResources(
            0, 1, nullResource);
    }

    void EnvironmentRenderer::ApplyScreenOutline(
        ID3D11ShaderResourceView* const source,
        ID3D11ShaderResourceView* const depth,
        ID3D11RenderTargetView* const destination,
        const std::uint32_t width,
        const std::uint32_t height,
        const ScreenOutlineSettings& settings,
        const DirectX::XMFLOAT4X4& projection)
    {
        if (!settings.enabled
            || source == nullptr
            || depth == nullptr
            || destination == nullptr
            || std::abs(projection._11) < 1e-6f
            || std::abs(projection._22) < 1e-6f)
        {
            return;
        }

        const float safeWidth =
            static_cast<float>(std::max(width, 1u));
        const float safeHeight =
            static_cast<float>(std::max(height, 1u));
        ScreenOutlineConstants constants{};
        constants.color = {
            std::clamp(settings.color.x, 0.0f, 1.0f),
            std::clamp(settings.color.y, 0.0f, 1.0f),
            std::clamp(settings.color.z, 0.0f, 1.0f),
            std::clamp(settings.intensity, 0.0f, 1.0f)
        };
        constants.parameters = {
            std::clamp(settings.thickness, 1.0f, 4.0f),
            std::clamp(settings.depthThreshold, 0.0001f, 1.0f),
            std::clamp(settings.normalThreshold, 0.0f, 1.0f),
            0.0f
        };
        constants.projection = {
            projection._33,
            projection._43,
            1.0f / projection._11,
            1.0f / projection._22
        };
        constants.texel = {
            1.0f / safeWidth,
            1.0f / safeHeight,
            safeWidth,
            safeHeight
        };
        m_context->UpdateSubresource(
            m_screenOutlineBuffer.Get(),
            0,
            nullptr,
            &constants,
            0,
            0);

        ID3D11RenderTargetView* targets[]{ destination };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        const D3D11_VIEWPORT viewport{
            0.0f,
            0.0f,
            safeWidth,
            safeHeight,
            0.0f,
            1.0f
        };
        m_context->RSSetViewports(1, &viewport);
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(
            m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(
            m_screenOutlinePixelShader.Get(), nullptr, 0);
        ID3D11Buffer* buffers[]{ m_screenOutlineBuffer.Get() };
        m_context->PSSetConstantBuffers(11, 1, buffers);
        // t0=元画像, t1=未使用（スカイ枠）, t2=シーン深度。
        ID3D11ShaderResourceView* resources[]{
            source,
            nullptr,
            depth
        };
        m_context->PSSetShaderResources(
            0,
            static_cast<UINT>(std::size(resources)),
            resources);
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(),
            0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);

        ID3D11ShaderResourceView* nullResources[3]{};
        m_context->PSSetShaderResources(
            0,
            static_cast<UINT>(std::size(nullResources)),
            nullResources);
    }

    void EnvironmentRenderer::ApplyScreenSpaceLensFlare(
        ID3D11ShaderResourceView* const source,
        ID3D11RenderTargetView* const destination,
        const std::uint32_t width,
        const std::uint32_t height,
        const ScreenSpaceLensFlareSettings& settings,
        ID3D11ShaderResourceView* const streak)
    {
        if (!settings.enabled
            || source == nullptr
            || destination == nullptr)
        {
            return;
        }

        const LensFlareConstants constants{
            {
                1.0f / static_cast<float>(std::max(width, 1u)),
                1.0f / static_cast<float>(std::max(height, 1u)),
                std::clamp(settings.threshold, 0.0f, 16.0f),
                std::clamp(settings.intensity, 0.0f, 8.0f)
            },
            {
                std::clamp(settings.ghostDispersal, 0.01f, 2.0f),
                std::clamp(settings.haloWidth, 0.05f, 1.5f),
                std::clamp(settings.chromaticAberration, 0.0f, 1.0f),
                std::clamp(settings.streakIntensity, 0.0f, 4.0f)
            },
            {
                std::clamp(settings.streakLength, 0.0f, 1.0f),
                0.0f,
                0.0f,
                0.0f
            }
        };
        m_context->UpdateSubresource(
            m_lensFlareBuffer.Get(),
            0,
            nullptr,
            &constants,
            0,
            0);

        ID3D11RenderTargetView* targets[]{ destination };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        const D3D11_VIEWPORT viewport{
            0.0f,
            0.0f,
            static_cast<float>(std::max(width, 1u)),
            static_cast<float>(std::max(height, 1u)),
            0.0f,
            1.0f
        };
        m_context->RSSetViewports(1, &viewport);
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(
            m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(
            m_lensFlarePixelShader.Get(), nullptr, 0);
        ID3D11Buffer* buffers[]{ m_lensFlareBuffer.Get() };
        m_context->PSSetConstantBuffers(7, 1, buffers);
        ID3D11ShaderResourceView* resources[]{ source };
        m_context->PSSetShaderResources(0, 1, resources);
        ID3D11ShaderResourceView* streakResources[]{ streak };
        m_context->PSSetShaderResources(5, 1, streakResources);
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(),
            0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);
        ID3D11ShaderResourceView* nullResource[]{ nullptr };
        m_context->PSSetShaderResources(
            0,
            1,
            nullResource);
        m_context->PSSetShaderResources(
            5,
            1,
            nullResource);
    }

    void EnvironmentRenderer::BuildLensFlareStreaks(
        ID3D11ShaderResourceView* const source,
        ID3D11RenderTargetView* const firstTarget,
        ID3D11ShaderResourceView* const firstResource,
        ID3D11RenderTargetView* const secondTarget,
        ID3D11ShaderResourceView* const secondResource,
        const std::uint32_t width,
        const std::uint32_t height,
        const ScreenSpaceLensFlareSettings& settings)
    {
        m_lastStreakResource = nullptr;
        if (!settings.enabled
            || source == nullptr
            || firstTarget == nullptr
            || secondTarget == nullptr)
        {
            return;
        }
        if (settings.streakIntensity <= 0.0f
            || settings.streakLength <= 0.0f)
        {
            // アナモルフィック効果が無効な場合は、関連する3パスを省略します。
            return;
        }

        const D3D11_VIEWPORT viewport{
            0.0f,
            0.0f,
            static_cast<float>(std::max(width, 1u)),
            static_cast<float>(std::max(height, 1u)),
            0.0f,
            1.0f
        };
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        ID3D11Buffer* buffers[]{ m_lensFlareBuffer.Get() };
        ID3D11ShaderResourceView* nullResource[]{ nullptr };

        // 3回。1回目だけ元の絵から高輝度を抜き、以降は前の回の
        // 結果を読みます。タップ間隔は毎回4倍に広がります。
        constexpr int PassCount = 3;
        const float longest = std::clamp(
            settings.streakLength,
            0.0f,
            1.0f);
        // 3回で longest まで届くよう、初回の刻みを逆算します。
        // 1回あたり片側2タップぶん伸びるので、刻みsに対して
        // 伸びは 2s。刻みを4倍ずつにすると合計は
        // 2(s + 4s + 16s) = 42s になります。
        const float baseStride = longest / 42.0f;

        for (int pass = 0; pass < PassCount; ++pass)
        {
            const bool first = pass == 0;
            auto* const target = first
                ? firstTarget
                : ((pass % 2) == 1 ? secondTarget : firstTarget);
            auto* const readResource = first
                ? source
                : ((pass % 2) == 1
                    ? firstResource
                    : secondResource);

            LensFlareConstants constants{};
            constants.primary = {
                1.0f / static_cast<float>(std::max(width, 1u)),
                1.0f / static_cast<float>(std::max(height, 1u)),
                std::clamp(settings.threshold, 0.0f, 16.0f),
                std::clamp(settings.intensity, 0.0f, 8.0f)
            };
            constants.secondary = {
                std::clamp(settings.ghostDispersal, 0.01f, 2.0f),
                std::clamp(settings.haloWidth, 0.05f, 1.5f),
                std::clamp(
                    settings.chromaticAberration,
                    0.0f,
                    1.0f),
                std::clamp(settings.streakIntensity, 0.0f, 4.0f)
            };
            constants.tertiary = { longest, 0.0f, 0.0f, 0.0f };
            constants.streakPass = {
                baseStride
                    * std::pow(4.0f, static_cast<float>(pass)),
                static_cast<float>(
                    std::clamp<std::uint32_t>(
                        settings.streakDirections,
                        1u,
                        4u)),
                DirectX::XMConvertToRadians(
                    settings.streakAngleDegrees),
                first ? 1.0f : 0.0f
            };
            m_context->UpdateSubresource(
                m_lensFlareBuffer.Get(),
                0,
                nullptr,
                &constants,
                0,
                0);

            ID3D11RenderTargetView* targets[]{ target };
            m_context->OMSetRenderTargets(1, targets, nullptr);
            m_context->RSSetViewports(1, &viewport);
            m_context->IASetInputLayout(nullptr);
            m_context->IASetPrimitiveTopology(
                D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_context->VSSetShader(
                m_vertexShader.Get(),
                nullptr,
                0);
            m_context->PSSetShader(
                m_lensFlareStreakPixelShader.Get(),
                nullptr,
                0);
            m_context->PSSetConstantBuffers(7, 1, buffers);
            m_context->PSSetSamplers(0, 1, samplers);
            // 1回目は元の絵をt0から、2回目以降は前の結果をt5から
            // 読みます。書き込み先と同じテクスチャをSRVへ設定すると、
            // D3D11がSRVをnullへ置換して読み取り値が0になるため分離します。
            ID3D11ShaderResourceView* sourceSlot[]{
                first ? readResource : nullptr
            };
            ID3D11ShaderResourceView* streakSlot[]{
                first ? nullptr : readResource
            };
            m_context->PSSetShaderResources(0, 1, sourceSlot);
            m_context->PSSetShaderResources(5, 1, streakSlot);
            m_context->OMSetDepthStencilState(
                m_depthDisabled.Get(),
                0);
            m_context->RSSetState(m_rasterizer.Get());
            m_context->Draw(3, 0);
            m_context->PSSetShaderResources(0, 1, nullResource);
            m_context->PSSetShaderResources(5, 1, nullResource);

            m_lastStreakResource = (target == firstTarget)
                ? firstResource
                : secondResource;
        }
        ID3D11RenderTargetView* noTargets[]{ nullptr };
        m_context->OMSetRenderTargets(1, noTargets, nullptr);
    }

    void EnvironmentRenderer::ApplyFXAA(
        ID3D11ShaderResourceView* source,
        ID3D11RenderTargetView* destination,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        if (source == nullptr || destination == nullptr)
        {
            return;
        }
        const BloomConstants constants{
            {
                1.0f / static_cast<float>(std::max(width, 1u)),
                1.0f / static_cast<float>(std::max(height, 1u))
            },
            0.0f,
            0.0f,
            1.0f,
            {}
        };
        m_context->UpdateSubresource(
            m_bloomBuffer.Get(), 0, nullptr, &constants, 0, 0);

        ID3D11RenderTargetView* targets[]{ destination };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        const D3D11_VIEWPORT viewport{
            0.0f,
            0.0f,
            static_cast<float>(std::max(width, 1u)),
            static_cast<float>(std::max(height, 1u)),
            0.0f,
            1.0f
        };
        m_context->RSSetViewports(1, &viewport);
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(
            m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(
            m_fxaaPixelShader.Get(), nullptr, 0);
        ID3D11Buffer* buffers[]{ m_bloomBuffer.Get() };
        m_context->PSSetConstantBuffers(1, 1, buffers);
        ID3D11ShaderResourceView* resources[]{ source };
        m_context->PSSetShaderResources(0, 1, resources);
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(), 0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);
        ID3D11ShaderResourceView* nullResource[]{ nullptr };
        m_context->PSSetShaderResources(0, 1, nullResource);
    }

    void EnvironmentRenderer::ApplyToneMapping(
        ID3D11ShaderResourceView* source,
        ID3D11RenderTargetView* destination,
        const std::uint32_t width,
        const std::uint32_t height,
        const ColorGradingSettings& settings)
    {
        if (source == nullptr || destination == nullptr)
        {
            return;
        }
        if (!settings.toneMappingEnabled)
        {
            Copy(
                source,
                destination,
                width,
                height);
            return;
        }

        const ColorGradingConstants constants{
            {
                std::clamp(settings.exposure, -8.0f, 8.0f),
                std::clamp(settings.contrast, 0.0f, 4.0f),
                std::clamp(settings.saturation, 0.0f, 4.0f),
                std::clamp(settings.temperature, -2.0f, 2.0f)
            },
            {
                std::clamp(settings.tint, -2.0f, 2.0f),
                std::clamp(settings.vignette, 0.0f, 1.0f),
                settings.enabled ? 1.0f : 0.0f,
                // 自動露出の補正（段数）。既存の予約領域を使うため、
                // cbufferの並びには影響しません。
                std::clamp(
                    settings.autoExposureStops,
                    -16.0f,
                    16.0f)
            }
        };
        m_context->UpdateSubresource(
            m_colorGradingBuffer.Get(),
            0,
            nullptr,
            &constants,
            0,
            0);

        ID3D11RenderTargetView* targets[]{ destination };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        const D3D11_VIEWPORT viewport{
            0.0f,
            0.0f,
            static_cast<float>(std::max(width, 1u)),
            static_cast<float>(std::max(height, 1u)),
            0.0f,
            1.0f
        };
        m_context->RSSetViewports(1, &viewport);
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(
            m_toneMapPixelShader.Get(), nullptr, 0);
        ID3D11Buffer* buffers[]{ m_colorGradingBuffer.Get() };
        m_context->PSSetConstantBuffers(2, 1, buffers);
        ID3D11ShaderResourceView* resources[]{ source };
        m_context->PSSetShaderResources(0, 1, resources);
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        m_context->OMSetDepthStencilState(m_depthDisabled.Get(), 0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);
        ID3D11ShaderResourceView* nullResource[]{ nullptr };
        m_context->PSSetShaderResources(0, 1, nullResource);
    }

    bool EnvironmentRenderer::RenderAmbientOcclusion(
        ID3D11ShaderResourceView* depth,
        ID3D11RenderTargetView* destination,
        const std::uint32_t width,
        const std::uint32_t height,
        const AmbientOcclusionSettings& settings,
        const DirectX::XMFLOAT4X4& projection,
        const std::uint32_t sampleCount)
    {
        if (depth == nullptr
            || destination == nullptr
            || !settings.enabled)
        {
            return false;
        }

        const float safeWidth =
            static_cast<float>(std::max(width, 1u));
        const float safeHeight =
            static_cast<float>(std::max(height, 1u));
        // 射影行列から、深度をビュー空間へ戻すための値を取り出します。
        // _11と_22が0の射影（正投影など）では復元できないため、
        // その場合は何もしません。
        if (std::abs(projection._11) < 1e-6f
            || std::abs(projection._22) < 1e-6f)
        {
            return false;
        }
        const AmbientOcclusionConstants constants{
            DirectX::XMFLOAT4{
                1.0f / safeWidth,
                1.0f / safeHeight,
                std::clamp(settings.radius, 0.01f, 10.0f),
                std::clamp(settings.strength, 0.0f, 1.0f)
            },
            DirectX::XMFLOAT4{
                projection._33,
                projection._43,
                1.0f / projection._11,
                1.0f / projection._22
            },
            DirectX::XMFLOAT4{
                static_cast<float>(
                    std::clamp(sampleCount, 4u, 32u)),
                0.0f,
                0.0f,
                0.0f
            }
        };
        m_context->UpdateSubresource(
            m_ambientOcclusionBuffer.Get(),
            0,
            nullptr,
            &constants,
            0,
            0);

        DrawAmbientOcclusionPass(
            m_ambientOcclusionPixelShader.Get(),
            nullptr,
            depth,
            destination,
            safeWidth,
            safeHeight);
        return true;
    }

    void EnvironmentRenderer::BlurAmbientOcclusion(
        ID3D11ShaderResourceView* occlusion,
        ID3D11ShaderResourceView* depth,
        ID3D11RenderTargetView* destination,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        if (occlusion == nullptr
            || depth == nullptr
            || destination == nullptr)
        {
            return;
        }
        // 定数バッファはRenderAmbientOcclusionが設定した内容
        // （テクセルサイズと射影）をそのまま使います。
        DrawAmbientOcclusionPass(
            m_ambientOcclusionBlurPixelShader.Get(),
            occlusion,
            depth,
            destination,
            static_cast<float>(std::max(width, 1u)),
            static_cast<float>(std::max(height, 1u)));
    }

    // SSAOの各パスに共通する描画です。t0へソース、t2へ深度を設定し、
    // 終了時にt0〜t2を解除して次の描画先として利用可能にします。
    void EnvironmentRenderer::DrawAmbientOcclusionPass(
        ID3D11PixelShader* pixelShader,
        ID3D11ShaderResourceView* source,
        ID3D11ShaderResourceView* depth,
        ID3D11RenderTargetView* destination,
        const float width,
        const float height)
    {
        if (pixelShader == nullptr)
        {
            return;
        }
        ID3D11RenderTargetView* targets[]{ destination };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        const D3D11_VIEWPORT viewport{
            0.0f,
            0.0f,
            width,
            height,
            0.0f,
            1.0f
        };
        m_context->RSSetViewports(1, &viewport);
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(
            m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(pixelShader, nullptr, 0);
        ID3D11Buffer* buffers[]{
            m_ambientOcclusionBuffer.Get()
        };
        m_context->PSSetConstantBuffers(4, 1, buffers);
        // t1はスカイキューブマップ用に空けています。
        ID3D11ShaderResourceView* resources[]{
            source,
            nullptr,
            depth
        };
        m_context->PSSetShaderResources(
            0,
            static_cast<UINT>(std::size(resources)),
            resources);
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(), 0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);

        // 次のパスの描画先として使えるよう、割り当てを外します。
        ID3D11ShaderResourceView* nullResources[]{
            nullptr,
            nullptr,
            nullptr,
            nullptr
        };
        m_context->PSSetShaderResources(
            0,
            static_cast<UINT>(std::size(nullResources)),
            nullResources);
    }

    bool EnvironmentRenderer::ApplyTemporalAntiAliasing(
        ID3D11ShaderResourceView* const source,
        ID3D11RenderTargetView* const destination,
        const std::uint32_t width,
        const std::uint32_t height,
        const TemporalAntiAliasingSettings& settings,
        const TemporalInputs& inputs)
    {
        // 履歴・深度・前フレームの行列が揃っていなければ混ぜません
        // （最初のフレーム、または切って入れ直した直後）。
        if (!settings.enabled
            || !inputs.previousValid
            || source == nullptr
            || destination == nullptr
            || inputs.history == nullptr
            || inputs.depth == nullptr)
        {
            return false;
        }

        TemporalConstants constants{};
        constants.inverseViewProjection =
            inputs.inverseViewProjection;
        constants.previousViewProjection =
            inputs.previousViewProjection;
        constants.parameters = {
            std::clamp(settings.historyWeight, 0.0f, 0.98f),
            std::max(settings.clampTolerance, 0.0f),
            1.0f / static_cast<float>(std::max(width, 1u)),
            1.0f / static_cast<float>(std::max(height, 1u))
        };
        m_context->UpdateSubresource(
            m_temporalBuffer.Get(),
            0,
            nullptr,
            &constants,
            0,
            0);

        ID3D11RenderTargetView* targets[]{ destination };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        const D3D11_VIEWPORT viewport{
            0.0f,
            0.0f,
            static_cast<float>(std::max(width, 1u)),
            static_cast<float>(std::max(height, 1u)),
            0.0f,
            1.0f
        };
        m_context->RSSetViewports(1, &viewport);
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(
            m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(
            m_temporalPixelShader.Get(), nullptr, 0);
        ID3D11Buffer* buffers[]{ m_temporalBuffer.Get() };
        m_context->PSSetConstantBuffers(6, 1, buffers);
        // t0=今のフレーム, t1=未使用（スカイ枠）, t2=深度,
        // t3=未使用（影の枠）, t4=履歴。
        ID3D11ShaderResourceView* resources[]{
            source,
            nullptr,
            inputs.depth,
            nullptr,
            inputs.history
        };
        m_context->PSSetShaderResources(
            0,
            static_cast<UINT>(std::size(resources)),
            resources);
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(), 0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);

        // 次のパスが描画先として使えるよう外します。
        ID3D11ShaderResourceView* nullResources[5]{};
        m_context->PSSetShaderResources(
            0,
            static_cast<UINT>(std::size(nullResources)),
            nullResources);
        return true;
    }

    bool EnvironmentRenderer::ApplyVolumetricLight(
        ID3D11ShaderResourceView* const source,
        ID3D11RenderTargetView* const destination,
        const std::uint32_t width,
        const std::uint32_t height,
        const VolumetricLightSettings& settings,
        const VolumetricInputs& inputs)
    {
        // 影付きの平行光源が要ります（遮るものが分からないと
        // 筋が出ないため）。揃っていなければ何もしません。
        if (!settings.enabled
            || settings.intensity <= 0.0f
            || source == nullptr
            || destination == nullptr
            || inputs.depth == nullptr
            || inputs.cascadeShadow == nullptr
            || inputs.cascadeCount == 0)
        {
            return false;
        }

        VolumetricConstants constants{};
        constants.inverseViewProjection =
            inputs.inverseViewProjection;
        constants.cameraPosition = {
            inputs.cameraPosition.x,
            inputs.cameraPosition.y,
            inputs.cameraPosition.z,
            std::max(settings.maximumDistance, 0.1f)
        };
        constants.lightDirection = {
            inputs.lightDirection.x,
            inputs.lightDirection.y,
            inputs.lightDirection.z,
            static_cast<float>(
                std::clamp<std::uint32_t>(
                    settings.sampleCount,
                    1u,
                    128u))
        };
        constants.lightColor = {
            inputs.lightColor.x * settings.intensity,
            inputs.lightColor.y * settings.intensity,
            inputs.lightColor.z * settings.intensity,
            std::clamp(settings.scattering, 0.0f, 0.95f)
        };
        constants.cascades = inputs.cascadeViewProjections;
        constants.shadowParameters = {
            static_cast<float>(
                std::min<std::uint32_t>(
                    inputs.cascadeCount,
                    4u)),
            inputs.shadowBias,
            1.0f / std::max(
                inputs.shadowResolution,
                1.0f),
            0.0f
        };
        m_context->UpdateSubresource(
            m_volumetricBuffer.Get(),
            0,
            nullptr,
            &constants,
            0,
            0);

        ID3D11RenderTargetView* targets[]{ destination };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        const D3D11_VIEWPORT viewport{
            0.0f,
            0.0f,
            static_cast<float>(std::max(width, 1u)),
            static_cast<float>(std::max(height, 1u)),
            0.0f,
            1.0f
        };
        m_context->RSSetViewports(1, &viewport);
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(
            m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(
            m_volumetricPixelShader.Get(), nullptr, 0);
        ID3D11Buffer* buffers[]{
            m_volumetricBuffer.Get()
        };
        m_context->PSSetConstantBuffers(5, 1, buffers);
        // t0=元画像, t1=未使用（スカイ枠）, t2=深度,
        // t3=カスケード影。
        ID3D11ShaderResourceView* resources[]{
            source,
            nullptr,
            inputs.depth,
            inputs.cascadeShadow
        };
        m_context->PSSetShaderResources(
            0,
            static_cast<UINT>(std::size(resources)),
            resources);
        ID3D11SamplerState* samplers[]{
            m_sampler.Get(),
            m_volumetricShadowSampler.Get()
        };
        m_context->PSSetSamplers(0, 2, samplers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(), 0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);

        // 次のパスが描画先として使えるよう外します。
        ID3D11ShaderResourceView* nullResources[4]{};
        m_context->PSSetShaderResources(
            0,
            static_cast<UINT>(std::size(nullResources)),
            nullResources);
        return true;
    }

    bool EnvironmentRenderer::ApplyDepthOfField(
        ID3D11ShaderResourceView* const source,
        ID3D11RenderTargetView* const destination,
        const std::uint32_t width,
        const std::uint32_t height,
        const DepthOfFieldSettings& settings,
        const DepthOfFieldInputs& inputs)
    {
        if (!settings.enabled
            || settings.maximumRadius <= 0.0f
            || source == nullptr
            || destination == nullptr
            || inputs.depth == nullptr
            || inputs.prepareTarget == nullptr
            || inputs.prepareResource == nullptr
            || inputs.blurTarget == nullptr
            || inputs.blurResource == nullptr
            || inputs.halfWidth == 0
            || inputs.halfHeight == 0)
        {
            return false;
        }
        // 射影から深度をカメラからの距離へ戻せない場合（正投影など）は
        // ピント位置を決められないので何もしません。
        if (std::abs(inputs.projection._11) < 1e-6f
            || std::abs(inputs.projection._22) < 1e-6f)
        {
            return false;
        }

        const float fullWidth =
            static_cast<float>(std::max(width, 1u));
        const float fullHeight =
            static_cast<float>(std::max(height, 1u));
        const float halfWidth =
            static_cast<float>(inputs.halfWidth);
        const float halfHeight =
            static_cast<float>(inputs.halfHeight);

        DepthOfFieldConstants constants{};
        constants.parameters = {
            std::max(settings.focusDistance, 0.01f),
            std::clamp(settings.focusRange, 0.0f, 1000.0f),
            std::clamp(settings.blurStrength, 0.0f, 8.0f),
            std::clamp(settings.maximumRadius, 0.0f, 32.0f)
        };
        constants.projection = {
            inputs.projection._33,
            inputs.projection._43,
            0.0f,
            0.0f
        };
        const float sampleCount = static_cast<float>(
            std::clamp<std::uint32_t>(
                inputs.sampleCount,
                4u,
                64u));

        // (1)半解像度へ色と符号付きCoCを書き出します。
        constants.texel = {
            1.0f / halfWidth,
            1.0f / halfHeight,
            sampleCount,
            0.0f
        };
        m_context->UpdateSubresource(
            m_depthOfFieldBuffer.Get(),
            0,
            nullptr,
            &constants,
            0,
            0);
        DrawDepthOfFieldPass(
            m_depthOfFieldPreparePixelShader.Get(),
            source,
            inputs.depth,
            nullptr,
            inputs.prepareTarget,
            halfWidth,
            halfHeight);

        // (2)半解像度で円形にぼかします。定数は(1)と同じ（テクセルも
        // 半解像度のまま）なので、更新せずにそのまま使います。
        DrawDepthOfFieldPass(
            m_depthOfFieldBlurPixelShader.Get(),
            nullptr,
            nullptr,
            inputs.prepareResource,
            inputs.blurTarget,
            halfWidth,
            halfHeight);

        // (3)フル解像度で合成します。テクセルだけフル解像度へ直します。
        constants.texel = {
            1.0f / fullWidth,
            1.0f / fullHeight,
            sampleCount,
            0.0f
        };
        m_context->UpdateSubresource(
            m_depthOfFieldBuffer.Get(),
            0,
            nullptr,
            &constants,
            0,
            0);
        DrawDepthOfFieldPass(
            m_depthOfFieldCompositePixelShader.Get(),
            source,
            inputs.depth,
            inputs.blurResource,
            destination,
            fullWidth,
            fullHeight);
        return true;
    }

    void EnvironmentRenderer::DrawDepthOfFieldPass(
        ID3D11PixelShader* const pixelShader,
        ID3D11ShaderResourceView* const source,
        ID3D11ShaderResourceView* const depth,
        ID3D11ShaderResourceView* const work,
        ID3D11RenderTargetView* const destination,
        const float width,
        const float height)
    {
        if (pixelShader == nullptr || destination == nullptr)
        {
            return;
        }
        ID3D11RenderTargetView* targets[]{ destination };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        const D3D11_VIEWPORT viewport{
            0.0f,
            0.0f,
            width,
            height,
            0.0f,
            1.0f
        };
        m_context->RSSetViewports(1, &viewport);
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(
            m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(pixelShader, nullptr, 0);
        ID3D11Buffer* buffers[]{
            m_depthOfFieldBuffer.Get()
        };
        m_context->PSSetConstantBuffers(8, 1, buffers);
        // t0=元画像, t1=未使用（スカイ枠）, t2=深度。
        ID3D11ShaderResourceView* resources[]{
            source,
            nullptr,
            depth
        };
        m_context->PSSetShaderResources(
            0,
            static_cast<UINT>(std::size(resources)),
            resources);
        ID3D11ShaderResourceView* workResources[]{ work };
        m_context->PSSetShaderResources(6, 1, workResources);
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(), 0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);

        // 次のパスで描画先にできるよう、割り当てを解除します。設定したままに
        // すると、同じテクスチャを描画先にした瞬間にD3D11が警告だけ
        // 出してSRVをnullにします（読んだ値が全部0になります）。
        ID3D11ShaderResourceView* nullResources[3]{};
        m_context->PSSetShaderResources(
            0,
            static_cast<UINT>(std::size(nullResources)),
            nullResources);
        m_context->PSSetShaderResources(6, 1, nullResources);
    }

    bool EnvironmentRenderer::ApplyMotionBlur(
        ID3D11ShaderResourceView* const source,
        ID3D11RenderTargetView* const destination,
        const std::uint32_t width,
        const std::uint32_t height,
        const MotionBlurSettings& settings,
        const MotionBlurInputs& inputs)
    {
        if (!settings.enabled
            || settings.intensity <= 0.0f
            || settings.maximumRadius <= 0.0f
            || source == nullptr
            || destination == nullptr
            || inputs.depth == nullptr
            // 前フレームの行列が無い最初のフレームは、伸ばす向きが
            // 決まりません。
            || !inputs.previousValid)
        {
            return false;
        }

        const float safeWidth =
            static_cast<float>(std::max(width, 1u));
        const float safeHeight =
            static_cast<float>(std::max(height, 1u));

        MotionBlurConstants constants{};
        constants.inverseViewProjection =
            inputs.inverseViewProjection;
        constants.previousViewProjection =
            inputs.previousViewProjection;
        constants.parameters = {
            std::clamp(settings.intensity, 0.0f, 4.0f),
            std::clamp(settings.maximumRadius, 0.0f, 64.0f),
            static_cast<float>(
                std::clamp<std::uint32_t>(
                    inputs.sampleCount,
                    2u,
                    32u)),
            0.0f
        };
        constants.texel = {
            1.0f / safeWidth,
            1.0f / safeHeight,
            0.0f,
            0.0f
        };
        m_context->UpdateSubresource(
            m_motionBlurBuffer.Get(),
            0,
            nullptr,
            &constants,
            0,
            0);

        ID3D11RenderTargetView* targets[]{ destination };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        const D3D11_VIEWPORT viewport{
            0.0f,
            0.0f,
            safeWidth,
            safeHeight,
            0.0f,
            1.0f
        };
        m_context->RSSetViewports(1, &viewport);
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(
            m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(
            m_motionBlurPixelShader.Get(), nullptr, 0);
        ID3D11Buffer* buffers[]{ m_motionBlurBuffer.Get() };
        m_context->PSSetConstantBuffers(9, 1, buffers);
        // t0=元画像, t1=未使用（スカイ枠）, t2=深度。
        ID3D11ShaderResourceView* resources[]{
            source,
            nullptr,
            inputs.depth
        };
        m_context->PSSetShaderResources(
            0,
            static_cast<UINT>(std::size(resources)),
            resources);
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(), 0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);

        ID3D11ShaderResourceView* nullResources[3]{};
        m_context->PSSetShaderResources(
            0,
            static_cast<UINT>(std::size(nullResources)),
            nullResources);
        return true;
    }

    void EnvironmentRenderer::RenderLuminance(
        ID3D11ShaderResourceView* const source,
        ID3D11RenderTargetView* const destination,
        ID3D11ShaderResourceView* const resource,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        if (source == nullptr
            || destination == nullptr
            || resource == nullptr)
        {
            return;
        }

        const float safeWidth =
            static_cast<float>(std::max(width, 1u));
        const float safeHeight =
            static_cast<float>(std::max(height, 1u));
        const LuminanceConstants constants{
            DirectX::XMFLOAT4{
                1.0f / safeWidth,
                1.0f / safeHeight,
                0.0f,
                0.0f
            }
        };
        m_context->UpdateSubresource(
            m_luminanceBuffer.Get(),
            0,
            nullptr,
            &constants,
            0,
            0);

        ID3D11RenderTargetView* targets[]{ destination };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        const D3D11_VIEWPORT viewport{
            0.0f,
            0.0f,
            safeWidth,
            safeHeight,
            0.0f,
            1.0f
        };
        m_context->RSSetViewports(1, &viewport);
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(
            m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(
            m_luminancePixelShader.Get(), nullptr, 0);
        ID3D11Buffer* buffers[]{ m_luminanceBuffer.Get() };
        m_context->PSSetConstantBuffers(10, 1, buffers);
        ID3D11ShaderResourceView* resources[]{ source };
        m_context->PSSetShaderResources(0, 1, resources);
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(), 0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);

        ID3D11ShaderResourceView* nullResource[]{ nullptr };
        m_context->PSSetShaderResources(0, 1, nullResource);
        // ミップ連鎖の生成は「書き終えたテクスチャを読む」操作なので、
        // 先に描画先から外します。刺したままだとD3D11が警告だけ出して
        // 何もしません。
        ID3D11RenderTargetView* noTargets[]{ nullptr };
        m_context->OMSetRenderTargets(1, noTargets, nullptr);
        // 2x2の箱フィルタを段ごとに掛けるので、いちばん小さいミップは
        // 全画素の対数を平均し、幾何平均輝度を求めます。
        m_context->GenerateMips(resource);
    }

    void EnvironmentRenderer::BuildReflectionDepthPyramid(
        RenderTarget& target,
        const float projectionZ,
        const float projectionW)
    {
        // SSRのHi-Z用に、深度→距離のminミップピラミッドを作ります。
        // ミップ0で生の深度を距離へ直し、以降は2x2の最小値で
        // 縮めていきます。呼ばれるのはフレームの途中（ライティングの
        // 準備中）なので、描画先とビューポートは退避して戻します。
        const auto mipCount =
            target.ReflectionDepthPyramidMipCount();
        auto* const rawDepth =
            target.DepthCopyShaderResourceView();
        if (mipCount == 0 || rawDepth == nullptr)
        {
            return;
        }

        // 前フレームのLit描画でt21/t22へ設定したSSRのカラーと深度を
        // 外してから、同じリソースをレンダーターゲットに設定します。
        // LitEffect::Applyが次の描画時にSRVを設定し直します。
        {
            ID3D11ShaderResourceView* nullReflection[]{
                nullptr, nullptr };
            m_context->PSSetShaderResources(
                21,
                static_cast<UINT>(
                    std::size(nullReflection)),
                nullReflection);
        }

        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            previousTarget;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView>
            previousDepth;
        m_context->OMGetRenderTargets(
            1,
            previousTarget.ReleaseAndGetAddressOf(),
            previousDepth.ReleaseAndGetAddressOf());
        D3D11_VIEWPORT previousViewport{};
        UINT previousViewportCount = 1;
        m_context->RSGetViewports(
            &previousViewportCount,
            &previousViewport);
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState>
            previousDepthState;
        UINT previousStencilReference{};
        m_context->OMGetDepthStencilState(
            previousDepthState.ReleaseAndGetAddressOf(),
            &previousStencilReference);
        Microsoft::WRL::ComPtr<ID3D11RasterizerState>
            previousRasterizer;
        m_context->RSGetState(
            previousRasterizer.ReleaseAndGetAddressOf());

        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(
            m_vertexShader.Get(), nullptr, 0);
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        ID3D11Buffer* buffers[]{ m_prefilterBuffer.Get() };
        m_context->PSSetConstantBuffers(3, 1, buffers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(), 0);
        m_context->RSSetState(m_rasterizer.Get());

        const auto runPass =
            [this](
                ID3D11PixelShader* shader,
                ID3D11ShaderResourceView* source,
                ID3D11RenderTargetView* destination,
                const std::uint32_t width,
                const std::uint32_t height,
                const float parameterX,
                const float parameterY)
        {
            PrefilterConstants constants{};
            constants.parameters = {
                parameterX,
                parameterY,
                0.0f,
                0.0f
            };
            m_context->UpdateSubresource(
                m_prefilterBuffer.Get(),
                0,
                nullptr,
                &constants,
                0,
                0);
            ID3D11RenderTargetView* targets[]{ destination };
            m_context->OMSetRenderTargets(
                1, targets, nullptr);
            const D3D11_VIEWPORT viewport{
                0.0f,
                0.0f,
                static_cast<float>(std::max(width, 1u)),
                static_cast<float>(std::max(height, 1u)),
                0.0f,
                1.0f
            };
            m_context->RSSetViewports(1, &viewport);
            m_context->PSSetShader(shader, nullptr, 0);
            ID3D11ShaderResourceView* resources[]{ source };
            m_context->PSSetShaderResources(0, 1, resources);
            m_context->Draw(3, 0);
            // 次のパスでこのRTVをSRVとして読むので、必ず外します
            // （着けたままだとD3D11がSRVを黙ってnullにします）。
            ID3D11ShaderResourceView* nullResource[]{
                nullptr };
            m_context->PSSetShaderResources(
                0, 1, nullResource);
        };

        const std::uint32_t width = target.Width();
        const std::uint32_t height = target.Height();
        runPass(
            m_reflectionLinearizePixelShader.Get(),
            rawDepth,
            target.ReflectionDepthPyramidMipTarget(0),
            width,
            height,
            projectionZ,
            projectionW);
        for (std::uint32_t mip = 1; mip < mipCount; ++mip)
        {
            const std::uint32_t parentWidth =
                std::max(width >> (mip - 1), 1u);
            const std::uint32_t parentHeight =
                std::max(height >> (mip - 1), 1u);
            runPass(
                m_reflectionDownsamplePixelShader.Get(),
                target.ReflectionDepthPyramidMipView(mip - 1),
                target.ReflectionDepthPyramidMipTarget(mip),
                std::max(width >> mip, 1u),
                std::max(height >> mip, 1u),
                static_cast<float>(parentWidth),
                static_cast<float>(parentHeight));
        }

        // 状態を戻します。
        ID3D11RenderTargetView* restoreTargets[]{
            previousTarget.Get() };
        m_context->OMSetRenderTargets(
            1, restoreTargets, previousDepth.Get());
        if (previousViewportCount > 0)
        {
            m_context->RSSetViewports(1, &previousViewport);
        }
        m_context->OMSetDepthStencilState(
            previousDepthState.Get(),
            previousStencilReference);
        m_context->RSSetState(previousRasterizer.Get());
    }

    void EnvironmentRenderer::CopyMirroredX(
        ID3D11ShaderResourceView* source,
        ID3D11RenderTargetView* destination,
        const std::uint32_t destinationWidth,
        const std::uint32_t destinationHeight)
    {
        if (source == nullptr || destination == nullptr)
        {
            return;
        }
        ID3D11RenderTargetView* targets[]{ destination };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        const D3D11_VIEWPORT viewport{
            0.0f,
            0.0f,
            static_cast<float>(
                std::max(destinationWidth, 1u)),
            static_cast<float>(
                std::max(destinationHeight, 1u)),
            0.0f,
            1.0f
        };
        m_context->RSSetViewports(1, &viewport);
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(
            m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(
            m_copyMirrorPixelShader.Get(), nullptr, 0);
        ID3D11ShaderResourceView* resources[]{ source };
        m_context->PSSetShaderResources(0, 1, resources);
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(), 0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);
        ID3D11ShaderResourceView* nullResource[]{ nullptr };
        m_context->PSSetShaderResources(0, 1, nullResource);
    }

    void EnvironmentRenderer::Copy(
        ID3D11ShaderResourceView* source,
        ID3D11RenderTargetView* destination,
        const std::uint32_t destinationWidth,
        const std::uint32_t destinationHeight)
    {
        if (source == nullptr || destination == nullptr)
        {
            return;
        }
        ID3D11RenderTargetView* targets[]{ destination };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        const D3D11_VIEWPORT viewport{
            0.0f,
            0.0f,
            static_cast<float>(
                std::max(destinationWidth, 1u)),
            static_cast<float>(
                std::max(destinationHeight, 1u)),
            0.0f,
            1.0f
        };
        m_context->RSSetViewports(1, &viewport);
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(
            m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(
            m_copyPixelShader.Get(), nullptr, 0);
        ID3D11ShaderResourceView* resources[]{ source };
        m_context->PSSetShaderResources(0, 1, resources);
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(), 0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);
        ID3D11ShaderResourceView* nullResource[]{ nullptr };
        m_context->PSSetShaderResources(0, 1, nullResource);
    }
}
