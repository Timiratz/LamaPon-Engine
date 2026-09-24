#include "LamaPon/Graphics/ScreenEffect.h"
#include "LamaPon/Graphics/ShaderManifest.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/PathUtils.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
    void ThrowIfFailed(
        const HRESULT result,
        const char* operation)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(
                std::string{ operation }
                + " failed with HRESULT "
                + std::to_string(
                    static_cast<unsigned long>(result)));
        }
    }
}

namespace LamaPon
{
    ScreenEffect::ScreenEffect(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        AssetManager& assets,
        const std::filesystem::path& shaderPath)
        : m_context(context)
    {
        if (device == nullptr || context == nullptr)
        {
            throw std::invalid_argument(
                "ScreenEffect requires a Direct3D device and context.");
        }

        std::filesystem::path hlslPath = shaderPath;
        ShaderPassDesc pass;
        if (IsShaderManifestPath(shaderPath))
        {
            ShaderAssetDesc asset;
            std::string manifestError;
            if (!LoadShaderAssetDesc(
                    assets,
                    shaderPath,
                    asset,
                    manifestError))
            {
                throw std::runtime_error(manifestError);
            }
            if (asset.type != ShaderAssetType::ScreenEffect)
            {
                throw std::runtime_error(
                    "ScreenEffect requires a shader manifest"
                    " whose type is 'screenEffect': "
                    + PathToUtf8(shaderPath));
            }
            // LoadShaderAssetDescがscreenEffectの先頭passにvertexと
            // pixelがあることを検証済みです。
            hlslPath = asset.source;
            pass = asset.passes.front();
        }
        else
        {
            // 従来のHLSL直接指定は固定入口を使うfallbackとして維持します。
            ShaderStageDesc vertex;
            vertex.stage = ShaderStage::Vertex;
            vertex.entryPoint = "VSMain";
            vertex.target = "vs_5_0";
            pass.stages.emplace_back(std::move(vertex));

            ShaderStageDesc pixel;
            pixel.stage = ShaderStage::Pixel;
            pixel.entryPoint = "PSMain";
            pixel.target = "ps_5_0";
            pass.stages.emplace_back(std::move(pixel));
        }

        std::string programError;
        if (!m_program.Compile(
                device,
                assets,
                hlslPath,
                pass,
                programError))
        {
            throw std::runtime_error(programError);
        }
        if (m_program.VertexShader() == nullptr
            || m_program.PixelShader() == nullptr)
        {
            throw std::runtime_error(
                "ScreenEffect requires vertex and pixel shader stages.");
        }

        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth =
            static_cast<UINT>(sizeof(Constants));
        buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ThrowIfFailed(
            device->CreateBuffer(
                &buffer,
                nullptr,
                m_constantBuffer.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateBuffer(screen effect)");

        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        // 原作の PolygonEffectScreen は LinearWrap を使用しています。
        // シーン側UVは各HLSLでsaturateし、ノイズマスクだけを繰り返します。
        sampler.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler.MaxLOD = std::numeric_limits<float>::max();
        ThrowIfFailed(
            device->CreateSamplerState(
                &sampler,
                m_sampler.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateSamplerState(screen effect)");

        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = FALSE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        ThrowIfFailed(
            device->CreateDepthStencilState(
                &depth,
                m_depthDisabled.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateDepthStencilState(screen effect)");

        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = TRUE;
        ThrowIfFailed(
            device->CreateRasterizerState(
                &rasterizer,
                m_rasterizer.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateRasterizerState(screen effect)");
    }

    void ScreenEffect::Apply(
        ID3D11ShaderResourceView* source,
        const std::array<ID3D11ShaderResourceView*, 2>&
            auxiliaryTextures,
        ID3D11ShaderResourceView* depth,
        const DirectX::XMFLOAT4& depthParameters,
        const DirectX::XMFLOAT4& depthUnprojection,
        ID3D11RenderTargetView* destination,
        const std::uint32_t width,
        const std::uint32_t height,
        const CustomParameters& parameters)
    {
        if (source == nullptr || destination == nullptr)
        {
            return;
        }

        Constants constants{};
        constants.parameters = parameters;
        constants.screenSize = {
            static_cast<float>(std::max(width, 1u)),
            static_cast<float>(std::max(height, 1u)),
            1.0f / static_cast<float>(std::max(width, 1u)),
            1.0f / static_cast<float>(std::max(height, 1u))
        };
        // 深度が取れていないときはzを0にして渡します。Shader側が
        // それを見て「深度を使わない絵」へ倒せるようにするためで、
        // 黙って0の深度を読ませると全面が最近接扱いになります。
        constants.depthParameters = depth != nullptr
            ? depthParameters
            : DirectX::XMFLOAT4{ 0.0f, 0.0f, 0.0f, 0.0f };
        constants.depthUnprojection = depthUnprojection;
        m_context->UpdateSubresource(
            m_constantBuffer.Get(),
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
            m_program.VertexShader(), nullptr, 0);
        m_context->PSSetShader(
            m_program.PixelShader(), nullptr, 0);
        ID3D11Buffer* buffers[]{
            m_constantBuffer.Get()
        };
        m_context->VSSetConstantBuffers(0, 1, buffers);
        m_context->PSSetConstantBuffers(0, 1, buffers);
        ID3D11ShaderResourceView* resources[]{
            source,
            auxiliaryTextures[0],
            auxiliaryTextures[1],
            depth
        };
        m_context->PSSetShaderResources(0, 4, resources);
        ID3D11SamplerState* samplers[]{
            m_sampler.Get()
        };
        m_context->PSSetSamplers(0, 1, samplers);
        constexpr float blendFactor[4]{};
        m_context->OMSetBlendState(
            nullptr,
            blendFactor,
            0xffffffffu);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(),
            0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);

        // 深度はこの後DSVとして刺し直される可能性があるので、
        // 必ず外します（着けたままだと次のバインドが黙って
        // 無効化されます）。
        ID3D11ShaderResourceView* nullResources[]{
            nullptr,
            nullptr,
            nullptr,
            nullptr
        };
        m_context->PSSetShaderResources(
            0,
            4,
            nullResources);
    }
}
