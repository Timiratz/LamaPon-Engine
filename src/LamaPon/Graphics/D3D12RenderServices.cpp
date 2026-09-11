#include "LamaPon/Graphics/D3D12RenderServices.h"

#include "LamaPon/Graphics/D3D12Backend.h"
#include "LamaPon/Graphics/GraphicsRenderServices.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using LamaPon::PrimitiveRenderVertex;

    constexpr char PrimitiveShaderSource[] = R"(
cbuffer PrimitiveConstants : register(b0)
{
    row_major float4x4 WorldViewProjection;
    row_major float4x4 World;
    float4 BaseColor;
};

Texture2D AlbedoTexture : register(t0);
SamplerState AlbedoSampler : register(s0);

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 textureCoordinate : TEXCOORD;
};

struct PixelInput
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float2 textureCoordinate : TEXCOORD;
};

PixelInput PrimitiveVertexShader(VertexInput input)
{
    PixelInput output;
    output.position = mul(float4(input.position, 1.0f), WorldViewProjection);
    output.normal = normalize(mul(float4(input.normal, 0.0f), World).xyz);
    output.textureCoordinate = input.textureCoordinate;
    return output;
}

float4 PrimitivePixelShader(PixelInput input) : SV_Target
{
    const float3 lightDirection = normalize(float3(-0.45f, 0.8f, -0.3f));
    const float diffuse = saturate(dot(normalize(input.normal), lightDirection));
    const float lighting = 0.28f + diffuse * 0.72f;
    const float4 albedo = AlbedoTexture.Sample(AlbedoSampler, input.textureCoordinate);
    return float4(albedo.rgb * BaseColor.rgb * lighting, albedo.a * BaseColor.a);
}
)";

    void ThrowIfFailed(const HRESULT result, const char* operation)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(
                std::string(operation) + " failed with HRESULT "
                + std::to_string(static_cast<unsigned long>(result)));
        }
    }

    [[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob> CompileShader(
        const char* entryPoint,
        const char* target)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        const HRESULT result = D3DCompile(
            PrimitiveShaderSource,
            sizeof(PrimitiveShaderSource) - 1u,
            "LamaPonD3D12Primitive",
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

    struct Geometry final
    {
        std::vector<PrimitiveRenderVertex> vertices;
        std::vector<std::uint32_t> indices;
    };

    void AddFace(
        Geometry& geometry,
        const DirectX::XMFLOAT3& normal,
        const std::array<DirectX::XMFLOAT3, 4>& positions)
    {
        const auto first = static_cast<std::uint32_t>(geometry.vertices.size());
        constexpr std::array<DirectX::XMFLOAT2, 4> ultravioletCoordinates{ {
            { 0.0f, 1.0f }, { 0.0f, 0.0f },
            { 1.0f, 1.0f }, { 1.0f, 0.0f }
        } };
        for (std::size_t index{}; index < positions.size(); ++index)
        {
            geometry.vertices.push_back({
                positions[index], normal, ultravioletCoordinates[index] });
        }
        geometry.indices.insert(
            geometry.indices.end(),
            { first, first + 1u, first + 2u,
                first + 2u, first + 1u, first + 3u });
    }

    [[nodiscard]] Geometry CreateCube()
    {
        Geometry result;
        constexpr float h = 0.5f;
        AddFace(result, { 0, 0, -1 }, { {
            { -h, -h, -h }, { -h, h, -h }, { h, -h, -h }, { h, h, -h } } });
        AddFace(result, { 0, 0, 1 }, { {
            { h, -h, h }, { h, h, h }, { -h, -h, h }, { -h, h, h } } });
        AddFace(result, { -1, 0, 0 }, { {
            { -h, -h, h }, { -h, h, h }, { -h, -h, -h }, { -h, h, -h } } });
        AddFace(result, { 1, 0, 0 }, { {
            { h, -h, -h }, { h, h, -h }, { h, -h, h }, { h, h, h } } });
        AddFace(result, { 0, 1, 0 }, { {
            { -h, h, -h }, { -h, h, h }, { h, h, -h }, { h, h, h } } });
        AddFace(result, { 0, -1, 0 }, { {
            { -h, -h, h }, { -h, -h, -h }, { h, -h, h }, { h, -h, -h } } });
        return result;
    }

    [[nodiscard]] Geometry CreatePlane()
    {
        Geometry result;
        AddFace(result, { 0, 1, 0 }, { {
            { -0.5f, 0, 0.5f }, { -0.5f, 0, -0.5f },
            { 0.5f, 0, 0.5f }, { 0.5f, 0, -0.5f } } });
        return result;
    }

    [[nodiscard]] Geometry CreateSphere()
    {
        Geometry result;
        constexpr std::uint32_t slices = 24;
        constexpr std::uint32_t stacks = 16;
        for (std::uint32_t stack{}; stack <= stacks; ++stack)
        {
            const float v = static_cast<float>(stack) / stacks;
            const float latitude = v * std::numbers::pi_v<float>;
            const float y = std::cos(latitude) * 0.5f;
            const float radius = std::sin(latitude) * 0.5f;
            for (std::uint32_t slice{}; slice <= slices; ++slice)
            {
                const float u = static_cast<float>(slice) / slices;
                const float longitude = u * std::numbers::pi_v<float> * 2.0f;
                const DirectX::XMFLOAT3 position{
                    std::sin(longitude) * radius,
                    y,
                    std::cos(longitude) * radius };
                result.vertices.push_back({
                    position,
                    { position.x * 2.0f, position.y * 2.0f, position.z * 2.0f },
                    { u, v } });
            }
        }
        for (std::uint32_t stack{}; stack < stacks; ++stack)
        {
            for (std::uint32_t slice{}; slice < slices; ++slice)
            {
                const auto first = stack * (slices + 1u) + slice;
                const auto next = first + slices + 1u;
                result.indices.insert(result.indices.end(), {
                    first, next, first + 1u,
                    first + 1u, next, next + 1u });
            }
        }
        return result;
    }

    [[nodiscard]] Geometry CreateCylinder()
    {
        Geometry result;
        constexpr std::uint32_t slices = 24;
        for (std::uint32_t slice{}; slice <= slices; ++slice)
        {
            const float u = static_cast<float>(slice) / slices;
            const float angle = u * std::numbers::pi_v<float> * 2.0f;
            const float x = std::sin(angle) * 0.5f;
            const float z = std::cos(angle) * 0.5f;
            const DirectX::XMFLOAT3 normal{ x * 2.0f, 0.0f, z * 2.0f };
            result.vertices.push_back({ { x, -0.5f, z }, normal, { u, 1 } });
            result.vertices.push_back({ { x, 0.5f, z }, normal, { u, 0 } });
        }
        for (std::uint32_t slice{}; slice < slices; ++slice)
        {
            const auto first = slice * 2u;
            result.indices.insert(result.indices.end(), {
                first, first + 1u, first + 2u,
                first + 2u, first + 1u, first + 3u });
        }
        const auto bottomCenter = static_cast<std::uint32_t>(result.vertices.size());
        result.vertices.push_back({ { 0, -0.5f, 0 }, { 0, -1, 0 }, { 0.5f, 0.5f } });
        const auto topCenter = static_cast<std::uint32_t>(result.vertices.size());
        result.vertices.push_back({ { 0, 0.5f, 0 }, { 0, 1, 0 }, { 0.5f, 0.5f } });
        for (std::uint32_t slice{}; slice < slices; ++slice)
        {
            const auto side = slice * 2u;
            const auto next = (slice + 1u) * 2u;
            result.indices.insert(result.indices.end(), {
                bottomCenter, next, side,
                topCenter, side + 1u, next + 1u });
        }
        return result;
    }

    [[nodiscard]] D3D12_BLEND_DESC MakeBlendDescription(bool enabled) noexcept
    {
        D3D12_RENDER_TARGET_BLEND_DESC target{};
        target.BlendEnable = enabled;
        target.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        target.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        target.BlendOp = D3D12_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D12_BLEND_ONE;
        target.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        D3D12_BLEND_DESC result{};
        for (auto& renderTarget : result.RenderTarget)
        {
            renderTarget = target;
        }
        return result;
    }

    [[nodiscard]] D3D12_RASTERIZER_DESC MakeRasterizerDescription() noexcept
    {
        D3D12_RASTERIZER_DESC result{};
        result.FillMode = D3D12_FILL_MODE_SOLID;
        result.CullMode = D3D12_CULL_MODE_NONE;
        result.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        result.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        result.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        result.DepthClipEnable = TRUE;
        return result;
    }

    [[nodiscard]] D3D12_DEPTH_STENCIL_DESC MakeDepthDescription(
        bool depthTest,
        bool depthWrite) noexcept
    {
        D3D12_DEPTH_STENCIL_DESC result{};
        result.DepthEnable = depthTest;
        result.DepthWriteMask = depthWrite
            ? D3D12_DEPTH_WRITE_MASK_ALL
            : D3D12_DEPTH_WRITE_MASK_ZERO;
        result.DepthFunc = depthTest
            ? D3D12_COMPARISON_FUNC_LESS_EQUAL
            : D3D12_COMPARISON_FUNC_ALWAYS;
        result.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        result.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        return result;
    }

    class D3D12RenderServices final : public LamaPon::GraphicsRenderServices
    {
    public:
        explicit D3D12RenderServices(LamaPon::D3D12Backend& backend)
            : m_backend(&backend)
            , m_cube(CreateCube())
            , m_sphere(CreateSphere())
            , m_cylinder(CreateCylinder())
            , m_plane(CreatePlane())
        {
            if (!backend.IsInitialized())
            {
                throw std::invalid_argument(
                    "The DirectX 12 render service requires an initialized backend.");
            }
            m_vertexShader = CompileShader("PrimitiveVertexShader", "vs_5_0");
            m_pixelShader = CompileShader("PrimitivePixelShader", "ps_5_0");

            D3D12_DESCRIPTOR_RANGE textureRange{};
            textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            textureRange.NumDescriptors = 1;
            D3D12_ROOT_PARAMETER parameters[2]{};
            parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            parameters[0].Constants.Num32BitValues = 36;
            parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[1].DescriptorTable.NumDescriptorRanges = 1;
            parameters[1].DescriptorTable.pDescriptorRanges = &textureRange;
            parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_STATIC_SAMPLER_DESC sampler{};
            sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            sampler.MaxLOD = D3D12_FLOAT32_MAX;
            sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_ROOT_SIGNATURE_DESC description{};
            description.NumParameters = 2;
            description.pParameters = parameters;
            description.NumStaticSamplers = 1;
            description.pStaticSamplers = &sampler;
            description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
            Microsoft::WRL::ComPtr<ID3DBlob> serialized;
            Microsoft::WRL::ComPtr<ID3DBlob> errors;
            ThrowIfFailed(
                D3D12SerializeRootSignature(
                    &description, D3D_ROOT_SIGNATURE_VERSION_1,
                    serialized.GetAddressOf(), errors.GetAddressOf()),
                "D3D12SerializeRootSignature(primitive)");
            ThrowIfFailed(
                backend.Device()->CreateRootSignature(
                    0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                    IID_PPV_ARGS(m_rootSignature.ReleaseAndGetAddressOf())),
                "ID3D12Device::CreateRootSignature(primitive)");
        }

        [[nodiscard]] bool DrawParticles(
            const LamaPon::ParticleDrawRequest&) override
        {
            return false;
        }

        [[nodiscard]] bool DrawPrimitive(
            const LamaPon::PrimitiveDrawRequest& request) override
        {
            const Geometry* geometry{};
            switch (request.shape)
            {
            case LamaPon::PrimitiveRenderShape::Cube: geometry = &m_cube; break;
            case LamaPon::PrimitiveRenderShape::Sphere: geometry = &m_sphere; break;
            case LamaPon::PrimitiveRenderShape::Cylinder: geometry = &m_cylinder; break;
            case LamaPon::PrimitiveRenderShape::Plane: geometry = &m_plane; break;
            case LamaPon::PrimitiveRenderShape::Procedural: break;
            default: return false;
            }
            const auto vertices = geometry != nullptr
                ? std::span<const PrimitiveRenderVertex>(geometry->vertices)
                : request.vertices;
            const auto indices = geometry != nullptr
                ? std::span<const std::uint32_t>(geometry->indices)
                : request.indices;
            if (vertices.empty() || indices.empty() || indices.size() % 3u != 0u
                || vertices.size() > std::numeric_limits<UINT>::max()
                || indices.size() > std::numeric_limits<UINT>::max())
            {
                return false;
            }
            if (std::ranges::any_of(indices, [vertices](const std::uint32_t index)
                { return index >= vertices.size(); }))
            {
                return false;
            }
            const auto& texture = request.albedo ? request.albedo : request.fallbackTexture;
            const auto binding = m_backend->TryResolveShaderResource(texture);
            auto* descriptorHeap = m_backend->ShaderResourceDescriptorHeap();
            if (!binding || descriptorHeap == nullptr)
            {
                return false;
            }

            auto* commandList = m_backend->BeginFrameCommands();
            const auto vertexBytes = static_cast<std::uint64_t>(vertices.size_bytes());
            const auto indexBytes = static_cast<std::uint64_t>(indices.size_bytes());
            if (vertexBytes > std::numeric_limits<UINT>::max()
                || indexBytes > std::numeric_limits<UINT>::max())
            {
                throw std::length_error("The DirectX 12 primitive is too large.");
            }
            const auto vertexUpload = m_backend->AllocateFrameUpload(
                vertexBytes, alignof(PrimitiveRenderVertex));
            const auto indexUpload = m_backend->AllocateFrameUpload(
                indexBytes, alignof(std::uint32_t));
            std::memcpy(vertexUpload.data, vertices.data(), vertices.size_bytes());
            std::memcpy(indexUpload.data, indices.data(), indices.size_bytes());

            struct Constants final
            {
                DirectX::XMFLOAT4X4 worldViewProjection;
                DirectX::XMFLOAT4X4 world;
                DirectX::XMFLOAT4 baseColor;
            } constants{};
            const auto world = DirectX::XMLoadFloat4x4(&request.world);
            const auto view = DirectX::XMLoadFloat4x4(&request.view);
            const auto projection = DirectX::XMLoadFloat4x4(&request.projection);
            DirectX::XMStoreFloat4x4(
                &constants.worldViewProjection,
                DirectX::XMMatrixMultiply(
                    DirectX::XMMatrixMultiply(world, view), projection));
            constants.world = request.world;
            constants.baseColor = request.baseColor;

            const D3D12_VERTEX_BUFFER_VIEW vertexView{
                vertexUpload.gpuAddress,
                static_cast<UINT>(vertexBytes),
                static_cast<UINT>(sizeof(PrimitiveRenderVertex)) };
            const D3D12_INDEX_BUFFER_VIEW indexView{
                indexUpload.gpuAddress,
                static_cast<UINT>(indexBytes),
                DXGI_FORMAT_R32_UINT };
            ID3D12DescriptorHeap* heaps[]{ descriptorHeap };
            auto* pipeline = PipelineState(
                request.alphaBlend, request.depthTest, request.depthWrite);
            commandList->SetGraphicsRootSignature(m_rootSignature.Get());
            commandList->SetPipelineState(pipeline);
            commandList->SetDescriptorHeaps(1, heaps);
            commandList->SetGraphicsRoot32BitConstants(
                0, 36, &constants, 0);
            commandList->SetGraphicsRootDescriptorTable(1, binding->descriptor);
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commandList->IASetVertexBuffers(0, 1, &vertexView);
            commandList->IASetIndexBuffer(&indexView);
            const auto& viewport = m_backend->PrimaryViewport();
            const auto& scissor = m_backend->PrimaryScissorRectangle();
            commandList->RSSetViewports(1, &viewport);
            commandList->RSSetScissorRects(1, &scissor);
            commandList->DrawIndexedInstanced(
                static_cast<UINT>(indices.size()), 1, 0, 0, 0);
            return true;
        }

    private:
        [[nodiscard]] ID3D12PipelineState* PipelineState(
            bool alphaBlend,
            bool depthTest,
            bool depthWrite)
        {
            const std::size_t index = !depthTest ? 2u : (alphaBlend ? 1u : 0u);
            auto& pipeline = m_pipelineStates[index];
            if (pipeline != nullptr)
            {
                return pipeline.Get();
            }
            static const std::array<D3D12_INPUT_ELEMENT_DESC, 3> inputs{ {
                { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
                    offsetof(PrimitiveRenderVertex, position),
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
                    offsetof(PrimitiveRenderVertex, normal),
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
                    offsetof(PrimitiveRenderVertex, textureCoordinate),
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
            } };
            D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
            description.pRootSignature = m_rootSignature.Get();
            description.VS = { m_vertexShader->GetBufferPointer(), m_vertexShader->GetBufferSize() };
            description.PS = { m_pixelShader->GetBufferPointer(), m_pixelShader->GetBufferSize() };
            description.BlendState = MakeBlendDescription(alphaBlend || !depthTest);
            description.SampleMask = std::numeric_limits<UINT>::max();
            description.RasterizerState = MakeRasterizerDescription();
            description.DepthStencilState = MakeDepthDescription(depthTest, depthWrite);
            description.InputLayout = { inputs.data(), static_cast<UINT>(inputs.size()) };
            description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            description.NumRenderTargets = 1;
            description.RTVFormats[0] = LamaPon::D3D12Backend::PrimaryColorFormat;
            description.DSVFormat = LamaPon::D3D12Backend::PrimaryDepthFormat;
            description.SampleDesc.Count = 1;
            ThrowIfFailed(
                m_backend->Device()->CreateGraphicsPipelineState(
                    &description, IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf())),
                "ID3D12Device::CreateGraphicsPipelineState(primitive)");
            return pipeline.Get();
        }

        LamaPon::D3D12Backend* m_backend{};
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
        Microsoft::WRL::ComPtr<ID3DBlob> m_vertexShader;
        Microsoft::WRL::ComPtr<ID3DBlob> m_pixelShader;
        std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 3> m_pipelineStates;
        Geometry m_cube;
        Geometry m_sphere;
        Geometry m_cylinder;
        Geometry m_plane;
    };
}

namespace LamaPon
{
    std::unique_ptr<GraphicsRenderServices>
        CreateD3D12GraphicsRenderServices(D3D12Backend& backend)
    {
        return std::make_unique<D3D12RenderServices>(backend);
    }
}
