#include "LamaPon/Graphics/ComputeEffect.h"

#include "LamaPon/Graphics/ShaderCompiler.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

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
    ComputeEffect::ComputeEffect(
        ID3D11Device* const device,
        ID3D11DeviceContext* const context,
        AssetManager& assets,
        const std::filesystem::path& shaderPath)
        : m_context(context)
    {
        if (device == nullptr || context == nullptr)
        {
            throw std::invalid_argument(
                "ComputeEffect requires a Direct3D device"
                " and context.");
        }

        // ディスクキャッシュ付きの共通入口を使います
        // （クラスタライトカリングと同じ経路）。
        const auto byteCode = CompileShaderCached(
            assets,
            shaderPath,
            "CSMain",
            "cs_5_0");
        ThrowIfFailed(
            device->CreateComputeShader(
                byteCode->GetBufferPointer(),
                byteCode->GetBufferSize(),
                nullptr,
                m_computeShader.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateComputeShader"
            "(compute effect)");

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
            "ID3D11Device::CreateBuffer(compute effect)");

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
            "ID3D11Device::CreateSamplerState"
            "(compute effect)");
    }

    void ComputeEffect::Dispatch(
        const std::array<ID3D11ShaderResourceView*, 2>&
            inputTextures,
        ID3D11UnorderedAccessView* const output,
        const std::uint32_t width,
        const std::uint32_t height,
        const CustomParameters& parameters)
    {
        if (output == nullptr
            || width == 0
            || height == 0)
        {
            return;
        }

        Constants constants{};
        constants.parameters = parameters;
        constants.outputSize = {
            static_cast<float>(width),
            static_cast<float>(height),
            1.0f / static_cast<float>(width),
            1.0f / static_cast<float>(height)
        };
        m_context->UpdateSubresource(
            m_constantBuffer.Get(),
            0,
            nullptr,
            &constants,
            0,
            0);

        m_context->CSSetShader(
            m_computeShader.Get(),
            nullptr,
            0);
        ID3D11Buffer* buffers[]{ m_constantBuffer.Get() };
        m_context->CSSetConstantBuffers(0, 1, buffers);
        ID3D11ShaderResourceView* resources[]{
            inputTextures[0],
            inputTextures[1]
        };
        m_context->CSSetShaderResources(0, 2, resources);
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        m_context->CSSetSamplers(0, 1, samplers);
        ID3D11UnorderedAccessView* outputs[]{ output };
        m_context->CSSetUnorderedAccessViews(
            0,
            1,
            outputs,
            nullptr);

        // 端数のスレッドグループも回すので切り上げます。はみ出した
        // スレッドはHLSL側で出力サイズと比べて捨ててください
        // （雛形にその1行が入っています）。
        const auto groupsX =
            (width + ThreadGroupSize - 1) / ThreadGroupSize;
        const auto groupsY =
            (height + ThreadGroupSize - 1) / ThreadGroupSize;
        m_context->Dispatch(groupsX, groupsY, 1);

        // 出したUAVとSRVは必ず外します。着けたままだと、次に同じ
        // テクスチャを描画側で読もうとしたときにD3D11が黙って
        // nullへ差し替えます。
        ID3D11UnorderedAccessView* nullOutputs[]{ nullptr };
        m_context->CSSetUnorderedAccessViews(
            0,
            1,
            nullOutputs,
            nullptr);
        ID3D11ShaderResourceView* nullResources[]{
            nullptr,
            nullptr
        };
        m_context->CSSetShaderResources(
            0,
            2,
            nullResources);
        m_context->CSSetShader(nullptr, nullptr, 0);
    }
}
