#include "LamaPon/Graphics/ComputeEffect.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/ShaderCompiler.h"
#include "LamaPon/Graphics/ShaderManifest.h"
#include "LamaPon/Graphics/ShaderProgram.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
    [[nodiscard]] const char* StageName(
        const LamaPon::ShaderStage stage) noexcept
    {
        switch (stage)
        {
        case LamaPon::ShaderStage::Vertex:
            return "vertex";
        case LamaPon::ShaderStage::Pixel:
            return "pixel";
        case LamaPon::ShaderStage::Geometry:
            return "geometry";
        case LamaPon::ShaderStage::Hull:
            return "hull";
        case LamaPon::ShaderStage::Domain:
            return "domain";
        case LamaPon::ShaderStage::Compute:
            return "compute";
        }
        return "unknown";
    }

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
            if (asset.type != ShaderAssetType::Compute)
            {
                throw std::runtime_error(
                    "ComputeEffect requires a shader manifest"
                    " whose type is 'compute': "
                    + PathToUtf8(shaderPath));
            }

            // Validation guarantees that at least one pass has a required
            // compute stage. Optional probes do not select the runtime pass.
            const ShaderPassDesc* selectedPass{};
            for (const auto& candidate : asset.passes)
            {
                const auto* compute = FindShaderStage(
                    candidate,
                    ShaderStage::Compute);
                if (compute != nullptr && !compute->optional)
                {
                    selectedPass = &candidate;
                    break;
                }
            }
            if (selectedPass == nullptr)
            {
                throw std::runtime_error(
                    "Compute shader manifest has no non-optional"
                    " compute stage: " + PathToUtf8(shaderPath));
            }

            for (const auto& stage : selectedPass->stages)
            {
                if (stage.stage != ShaderStage::Compute)
                {
                    throw std::runtime_error(
                        "ComputeEffect manifest pass '"
                        + selectedPass->name
                        + "' contains unsupported "
                        + StageName(stage.stage)
                        + " stage; a compute pass may contain only"
                        " a compute stage: "
                        + PathToUtf8(shaderPath));
                }
            }

            ShaderProgram program;
            std::string programError;
            if (!program.Compile(
                    device,
                    assets,
                    asset.source,
                    *selectedPass,
                    programError))
            {
                throw std::runtime_error(programError);
            }
            if (program.ComputeShader() == nullptr)
            {
                throw std::runtime_error(
                    "ComputeEffect manifest pass did not create a"
                    " compute shader: " + PathToUtf8(shaderPath));
            }
            m_computeShader = program.ComputeShader();
        }
        else
        {
            // Direct HLSL keeps the original CSMain/cs_5_0 fallback.
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
