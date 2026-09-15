#include "LamaPon/Graphics/D3D12DebugDrawingBackend.h"

#include "LamaPon/Graphics/D3D12Backend.h"

#include <d3dcompiler.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace
{
    constexpr char DebugLineShader[] = R"(
cbuffer DebugLineConstants : register(b0)
{
    row_major float4x4 ViewProjection;
};

struct VertexInput
{
    float3 position : POSITION;
    float4 color : COLOR;
};

struct PixelInput
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

PixelInput DebugVertexMain(VertexInput input)
{
    PixelInput output;
    output.position = mul(float4(input.position, 1.0f), ViewProjection);
    output.color = input.color;
    return output;
}

float4 DebugPixelMain(PixelInput input) : SV_TARGET
{
    return input.color;
}
)";

    struct DebugVertex final
    {
        DirectX::XMFLOAT3 position{};
        DirectX::XMFLOAT4 color{};
    };

    void ThrowIfFailed(const HRESULT result, const char* operation)
    {
        if (FAILED(result))
        {
            std::ostringstream message;
            message << operation << " failed (HRESULT=0x" << std::hex
                << std::uppercase << static_cast<unsigned long>(result)
                << ").";
            throw std::runtime_error(message.str());
        }
    }

    [[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob> CompileShader(
        const char* entryPoint,
        const char* target)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        const HRESULT result = D3DCompile(
            DebugLineShader,
            sizeof(DebugLineShader) - 1u,
            "LamaPonD3D12DebugLine",
            nullptr,
            nullptr,
            entryPoint,
            target,
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0,
            bytecode.GetAddressOf(),
            errors.GetAddressOf());
        if (FAILED(result))
        {
            std::string message = std::string("D3DCompile(") + entryPoint
                + ") failed";
            if (errors != nullptr && errors->GetBufferSize() != 0)
            {
                message += ": ";
                message.append(
                    static_cast<const char*>(errors->GetBufferPointer()),
                    errors->GetBufferSize());
            }
            throw std::runtime_error(message);
        }
        return bytecode;
    }
}

namespace LamaPon
{
    D3D12DebugDrawingBackend::D3D12DebugDrawingBackend(
        D3D12Backend& backend)
        : m_backend(&backend)
    {
        if (!backend.IsInitialized())
        {
            throw std::invalid_argument(
                "D3D12 debug drawing requires an initialized backend.");
        }

        m_vertexShader = CompileShader("DebugVertexMain", "vs_5_0");
        m_pixelShader = CompileShader("DebugPixelMain", "ps_5_0");

        D3D12_ROOT_PARAMETER matrix{};
        matrix.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        matrix.Constants.ShaderRegister = 0;
        matrix.Constants.RegisterSpace = 0;
        matrix.Constants.Num32BitValues = 16;
        matrix.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_ROOT_SIGNATURE_DESC description{};
        description.NumParameters = 1;
        description.pParameters = &matrix;
        description.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        Microsoft::WRL::ComPtr<ID3DBlob> serialized;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        ThrowIfFailed(
            D3D12SerializeRootSignature(
                &description,
                D3D_ROOT_SIGNATURE_VERSION_1,
                serialized.GetAddressOf(),
                errors.GetAddressOf()),
            "D3D12SerializeRootSignature(debug line)");
        ThrowIfFailed(
            backend.Device()->CreateRootSignature(
                0,
                serialized->GetBufferPointer(),
                serialized->GetBufferSize(),
                IID_PPV_ARGS(m_rootSignature.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateRootSignature(debug line)");
    }

    void D3D12DebugDrawingBackend::DrawLines(
        const std::span<const DebugLine> lines,
        const DirectX::XMFLOAT4X4& view,
        const DirectX::XMFLOAT4X4& projection)
    {
        if (lines.empty())
        {
            return;
        }
        if (m_backend == nullptr || !m_backend->IsInitialized())
        {
            throw std::logic_error(
                "D3D12 debug drawing requires an initialized backend.");
        }
        if (lines.size() > std::numeric_limits<UINT>::max() / 2u
            || lines.size() > std::numeric_limits<UINT>::max()
                / (sizeof(DebugVertex) * 2u))
        {
            throw std::length_error("There are too many debug lines to draw.");
        }

        const auto vertexCount = static_cast<UINT>(lines.size() * 2u);
        const auto vertexBytes = static_cast<UINT>(
            vertexCount * sizeof(DebugVertex));
        auto* const commands = m_backend->BeginFrameCommands();
        const auto upload = m_backend->AllocateFrameUpload(vertexBytes, 16u);
        auto* vertices = reinterpret_cast<DebugVertex*>(upload.data);
        for (const auto& line : lines)
        {
            *vertices++ = { line.start, line.color };
            *vertices++ = { line.end, line.color };
        }

        DirectX::XMFLOAT4X4 viewProjection{};
        DirectX::XMStoreFloat4x4(
            &viewProjection,
            DirectX::XMMatrixMultiply(
                DirectX::XMLoadFloat4x4(&view),
                DirectX::XMLoadFloat4x4(&projection)));

        const D3D12_VERTEX_BUFFER_VIEW vertexBuffer{
            upload.gpuAddress,
            vertexBytes,
            sizeof(DebugVertex)
        };
        commands->SetGraphicsRootSignature(m_rootSignature.Get());
        commands->SetPipelineState(PipelineState(
            m_backend->ActiveColorFormat(),
            m_backend->ActiveDepthFormat()));
        commands->SetGraphicsRoot32BitConstants(
            0,
            16,
            &viewProjection,
            0);
        commands->RSSetViewports(1, &m_backend->ActiveViewport());
        commands->RSSetScissorRects(
            1, &m_backend->ActiveScissorRectangle());
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        commands->IASetVertexBuffers(0, 1, &vertexBuffer);
        commands->DrawInstanced(vertexCount, 1, 0, 0);
    }

    ID3D12PipelineState* D3D12DebugDrawingBackend::PipelineState(
        const DXGI_FORMAT colorFormat,
        const DXGI_FORMAT depthFormat)
    {
        const std::size_t colorIndex = colorFormat
                == D3D12Backend::PrimaryColorFormat
            ? 0u
            : colorFormat == DXGI_FORMAT_R16G16B16A16_FLOAT
                ? 1u
                : throw std::invalid_argument(
                    "The active DirectX 12 color target format is not "
                    "supported by debug drawing.");
        const std::size_t depthIndex = depthFormat == DXGI_FORMAT_UNKNOWN
            ? 0u
            : depthFormat == D3D12Backend::PrimaryDepthFormat
                ? 1u
                : depthFormat == D3D12Backend::ShadowDepthFormat
                    ? 2u
                    : throw std::invalid_argument(
                        "The active DirectX 12 depth target format is not "
                        "supported by debug drawing.");
        auto& pipeline = m_pipelineStates[colorIndex * 3u + depthIndex];
        if (pipeline != nullptr)
        {
            return pipeline.Get();
        }

        static const std::array<D3D12_INPUT_ELEMENT_DESC, 2> inputs{ {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
                offsetof(DebugVertex, position),
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
                offsetof(DebugVertex, color),
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        } };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
        description.pRootSignature = m_rootSignature.Get();
        description.VS = {
            m_vertexShader->GetBufferPointer(),
            m_vertexShader->GetBufferSize()
        };
        description.PS = {
            m_pixelShader->GetBufferPointer(),
            m_pixelShader->GetBufferSize()
        };
        auto& target = description.BlendState.RenderTarget[0];
        target.BlendEnable = TRUE;
        target.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        target.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        target.BlendOp = D3D12_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
        target.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        description.SampleMask = std::numeric_limits<UINT>::max();
        description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        description.RasterizerState.FrontCounterClockwise = FALSE;
        description.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        description.RasterizerState.DepthBiasClamp =
            D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        description.RasterizerState.SlopeScaledDepthBias =
            D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        description.RasterizerState.DepthClipEnable = TRUE;
        description.DepthStencilState.DepthEnable = FALSE;
        description.DepthStencilState.DepthWriteMask =
            D3D12_DEPTH_WRITE_MASK_ZERO;
        description.DepthStencilState.DepthFunc =
            D3D12_COMPARISON_FUNC_ALWAYS;
        description.DepthStencilState.StencilEnable = FALSE;
        description.DepthStencilState.StencilReadMask =
            D3D12_DEFAULT_STENCIL_READ_MASK;
        description.DepthStencilState.StencilWriteMask =
            D3D12_DEFAULT_STENCIL_WRITE_MASK;
        description.InputLayout = {
            inputs.data(), static_cast<UINT>(inputs.size())
        };
        description.PrimitiveTopologyType =
            D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        description.NumRenderTargets = 1;
        description.RTVFormats[0] = colorFormat;
        description.DSVFormat = depthFormat;
        description.SampleDesc.Count = 1;
        ThrowIfFailed(
            m_backend->Device()->CreateGraphicsPipelineState(
                &description,
                IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateGraphicsPipelineState(debug line)");
        return pipeline.Get();
    }
}
