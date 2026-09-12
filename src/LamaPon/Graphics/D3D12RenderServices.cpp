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
    float4 CameraPosition;
    float4 MaterialProperties;
    float4 EmissiveFactor;
    float4 AmbientColorIntensity;
    float4 DirectionalDirectionIntensity[4];
    float4 DirectionalColors[4];
    uint4 LightCounts;
    float4 PointPositionRange[16];
    float4 PointColorIntensity[16];
    float4 SpotPositionRange[8];
    float4 SpotDirectionInnerCosine[8];
    float4 SpotColorIntensity[8];
    float4 SpotOuterCosine[8];
};

Texture2D AlbedoTexture : register(t0);
Texture2D NormalTexture : register(t1);
Texture2D RoughnessTexture : register(t2);
Texture2D MetallicTexture : register(t3);
Texture2D OcclusionTexture : register(t4);
Texture2D EmissiveTexture : register(t5);
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
    float3 worldPosition : TEXCOORD1;
    float3 normal : NORMAL;
    float2 textureCoordinate : TEXCOORD;
};

PixelInput PrimitiveVertexShader(VertexInput input)
{
    PixelInput output;
    output.position = mul(float4(input.position, 1.0f), WorldViewProjection);
    output.worldPosition = mul(float4(input.position, 1.0f), World).xyz;
    output.normal = normalize(mul(float4(input.normal, 0.0f), World).xyz);
    output.textureCoordinate = input.textureCoordinate;
    return output;
}

float4 PrimitivePixelShader(PixelInput input) : SV_Target
{
    float3 normal = normalize(input.normal);
    if (MaterialProperties.w > 0.5f)
    {
        // BC5 stores only XY. Reconstructing Z also keeps ordinary RGB
        // normal maps equivalent after their XY channels are sampled.
        const float2 sampledNormalXY = NormalTexture.Sample(
            AlbedoSampler,
            input.textureCoordinate).xy * 2.0f - 1.0f;
        const float3 sampledNormal = float3(
            sampledNormalXY,
            sqrt(saturate(
                1.0f - dot(sampledNormalXY, sampledNormalXY))));
        const float3 positionDx = ddx(input.worldPosition);
        const float3 positionDy = ddy(input.worldPosition);
        const float2 uvDx = ddx(input.textureCoordinate);
        const float2 uvDy = ddy(input.textureCoordinate);
        const float3 tangentUnscaled =
            cross(positionDy, normal) * uvDx.x
            + cross(normal, positionDx) * uvDy.x;
        const float3 bitangentUnscaled =
            cross(positionDy, normal) * uvDx.y
            + cross(normal, positionDx) * uvDy.y;
        const float inverseScale = rsqrt(max(
            max(dot(tangentUnscaled, tangentUnscaled),
                dot(bitangentUnscaled, bitangentUnscaled)),
            0.000001f));
        const float3 tangent = tangentUnscaled * inverseScale;
        const float3 bitangent = bitangentUnscaled * inverseScale;
        const float strength = max(EmissiveFactor.w, 0.0f);
        normal = normalize(
            tangent * sampledNormal.x * strength
            + bitangent * sampledNormal.y * strength
            + normal * sampledNormal.z);
    }
    const float4 albedo = AlbedoTexture.Sample(
        AlbedoSampler,
        input.textureCoordinate);
    const float3 surfaceColor = albedo.rgb * BaseColor.rgb;
    const float roughnessSample = RoughnessTexture.Sample(
        AlbedoSampler,
        input.textureCoordinate).g;
    const float metallicSample = MetallicTexture.Sample(
        AlbedoSampler,
        input.textureCoordinate).b;
    const float occlusionSample = OcclusionTexture.Sample(
        AlbedoSampler,
        input.textureCoordinate).r;
    const float3 emissiveSample = EmissiveTexture.Sample(
        AlbedoSampler,
        input.textureCoordinate).rgb;
    const float roughness = clamp(
        MaterialProperties.x * roughnessSample,
        0.02f,
        1.0f);
    const float metallic = saturate(
        MaterialProperties.y * metallicSample);
    const float occlusion = lerp(
        1.0f,
        occlusionSample,
        saturate(MaterialProperties.z));
    const float3 diffuseColor = surfaceColor * (1.0f - metallic);
    const float3 specularColor = lerp(0.04f.xxx, surfaceColor, metallic);
    const float3 viewDirection = normalize(
        CameraPosition.xyz - input.worldPosition);
    const float specularPower = lerp(128.0f, 4.0f, roughness);
    const float specularScale = lerp(1.0f, 0.08f, roughness);
    float3 result = surfaceColor
        * AmbientColorIntensity.rgb
        * AmbientColorIntensity.w
        * occlusion;
    [loop]
    for (uint index = 0; index < min(LightCounts.x, 4u); ++index)
    {
        const float3 lightDirection = normalize(
            -DirectionalDirectionIntensity[index].xyz);
        const float diffuse = saturate(dot(normal, lightDirection));
        const float3 halfVector = normalize(
            lightDirection + viewDirection);
        const float specular = pow(
            saturate(dot(normal, halfVector)),
            specularPower) * specularScale;
        result += DirectionalColors[index].rgb
            * DirectionalDirectionIntensity[index].w
            * (diffuseColor * diffuse + specularColor * specular);
    }

    // Point / SpotはD3D11の従来経路（LamaPonLit.hlsl）と同じ距離減衰と
    // コーン減衰を、この最小pipelineのLambert拡散へ掛けます。
    [loop]
    for (uint pointIndex = 0; pointIndex < min(LightCounts.y, 16u); ++pointIndex)
    {
        const float3 delta = PointPositionRange[pointIndex].xyz - input.worldPosition;
        const float lightDistance = length(delta);
        const float range = max(PointPositionRange[pointIndex].w, 0.001f);
        const float attenuation = pow(saturate(1.0f - lightDistance / range), 2.0f);
        const float3 lightDirection = delta / max(lightDistance, 0.0001f);
        const float diffuse = saturate(dot(normal, lightDirection));
        const float3 halfVector = normalize(lightDirection + viewDirection);
        const float specular = pow(
            saturate(dot(normal, halfVector)),
            specularPower) * specularScale;
        result += PointColorIntensity[pointIndex].rgb
            * PointColorIntensity[pointIndex].w
            * attenuation
            * (diffuseColor * diffuse + specularColor * specular);
    }

    [loop]
    for (uint spotIndex = 0; spotIndex < min(LightCounts.z, 8u); ++spotIndex)
    {
        const float3 lightToPixel = input.worldPosition - SpotPositionRange[spotIndex].xyz;
        const float lightDistance = length(lightToPixel);
        const float range = max(SpotPositionRange[spotIndex].w, 0.001f);
        const float3 rayDirection = lightToPixel / max(lightDistance, 0.0001f);
        const float cone = dot(normalize(SpotDirectionInnerCosine[spotIndex].xyz), rayDirection);
        const float coneAttenuation = smoothstep(
            SpotOuterCosine[spotIndex].x,
            SpotDirectionInnerCosine[spotIndex].w,
            cone);
        const float distanceAttenuation = pow(saturate(1.0f - lightDistance / range), 2.0f);
        const float diffuse = saturate(dot(normal, -rayDirection));
        const float3 halfVector = normalize(-rayDirection + viewDirection);
        const float specular = pow(
            saturate(dot(normal, halfVector)),
            specularPower) * specularScale;
        result += SpotColorIntensity[spotIndex].rgb
            * SpotColorIntensity[spotIndex].w
            * distanceAttenuation
            * coneAttenuation
            * coneAttenuation
            * (diffuseColor * diffuse + specularColor * specular);
    }
    result += emissiveSample * EmissiveFactor.rgb;
    return float4(result, albedo.a * BaseColor.a);
}
)";

    constexpr char ParticleShaderSource[] = R"(
cbuffer ParticleConstants : register(b0)
{
    row_major float4x4 ViewProjection;
};

Texture2D ParticleTexture : register(t0);
SamplerState ParticleSampler : register(s0);

struct VertexInput
{
    float3 position : POSITION;
    float4 color : COLOR;
    float2 textureCoordinate : TEXCOORD;
};

struct PixelInput
{
    float4 position : SV_Position;
    float4 color : COLOR;
    float2 textureCoordinate : TEXCOORD;
};

PixelInput ParticleVertexShader(VertexInput input)
{
    PixelInput output;
    output.position = mul(float4(input.position, 1.0f), ViewProjection);
    output.color = input.color;
    output.textureCoordinate = input.textureCoordinate;
    return output;
}

float4 ParticlePixelShader(PixelInput input) : SV_Target
{
    return ParticleTexture.Sample(ParticleSampler, input.textureCoordinate)
        * input.color;
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
        const char* source,
        std::size_t sourceSize,
        const char* sourceName,
        const char* entryPoint,
        const char* target)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        const HRESULT result = D3DCompile(
            source,
            sourceSize,
            sourceName,
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

    [[nodiscard]] D3D12_BLEND_DESC MakeParticleBlendDescription(
        bool additive) noexcept
    {
        D3D12_RENDER_TARGET_BLEND_DESC target{};
        target.BlendEnable = TRUE;
        target.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        target.DestBlend = additive
            ? D3D12_BLEND_ONE
            : D3D12_BLEND_INV_SRC_ALPHA;
        target.BlendOp = D3D12_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D12_BLEND_ONE;
        target.DestBlendAlpha = additive
            ? D3D12_BLEND_ONE
            : D3D12_BLEND_INV_SRC_ALPHA;
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
            m_vertexShader = CompileShader(
                PrimitiveShaderSource,
                sizeof(PrimitiveShaderSource) - 1u,
                "LamaPonD3D12Primitive",
                "PrimitiveVertexShader",
                "vs_5_0");
            m_pixelShader = CompileShader(
                PrimitiveShaderSource,
                sizeof(PrimitiveShaderSource) - 1u,
                "LamaPonD3D12Primitive",
                "PrimitivePixelShader",
                "ps_5_0");
            m_particleVertexShader = CompileShader(
                ParticleShaderSource,
                sizeof(ParticleShaderSource) - 1u,
                "LamaPonD3D12Particle",
                "ParticleVertexShader",
                "vs_5_0");
            m_particlePixelShader = CompileShader(
                ParticleShaderSource,
                sizeof(ParticleShaderSource) - 1u,
                "LamaPonD3D12Particle",
                "ParticlePixelShader",
                "ps_5_0");

            std::array<D3D12_DESCRIPTOR_RANGE, 6> textureRanges{};
            for (UINT index{}; index < textureRanges.size(); ++index)
            {
                textureRanges[index].RangeType =
                    D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                textureRanges[index].NumDescriptors = 1;
                textureRanges[index].BaseShaderRegister = index;
            }
            std::array<D3D12_ROOT_PARAMETER, 7> parameters{};
            parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            parameters[0].Descriptor.ShaderRegister = 0;
            parameters[0].Descriptor.RegisterSpace = 0;
            parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            for (std::size_t index{}; index < textureRanges.size(); ++index)
            {
                auto& parameter = parameters[index + 1u];
                parameter.ParameterType =
                    D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                parameter.DescriptorTable.NumDescriptorRanges = 1;
                parameter.DescriptorTable.pDescriptorRanges =
                    &textureRanges[index];
                parameter.ShaderVisibility =
                    D3D12_SHADER_VISIBILITY_PIXEL;
            }
            D3D12_STATIC_SAMPLER_DESC sampler{};
            sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            sampler.MaxLOD = D3D12_FLOAT32_MAX;
            sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_ROOT_SIGNATURE_DESC description{};
            description.NumParameters = static_cast<UINT>(parameters.size());
            description.pParameters = parameters.data();
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
            const LamaPon::ParticleDrawRequest& request) override
        {
            // 半透明particleはshadow casterにしません。通常描画入口を
            // 呼ぶとShadowMapのDSVからprimary outputへ戻るため、ここで
            // 成功扱いにしてshadow passを維持します。
            if (m_backend->IsShadowPassActive())
            {
                return true;
            }
            constexpr std::size_t maximumParticleCount = 4096u;
            if (request.vertices.empty())
            {
                return true;
            }
            if (request.vertices.size() % 4u != 0u
                || request.vertices.size() > maximumParticleCount * 4u)
            {
                throw std::invalid_argument(
                    "Particle draw requests require complete quads within "
                    "the service capacity.");
            }
            const auto& texture = request.texture
                ? request.texture
                : request.fallbackTexture;
            const auto binding = m_backend->TryResolveShaderResource(texture);
            auto* descriptorHeap = m_backend->ShaderResourceDescriptorHeap();
            if (!binding || descriptorHeap == nullptr)
            {
                return false;
            }

            auto* commandList = m_backend->BeginFrameCommands();
            const auto quadCount = request.vertices.size() / 4u;
            const auto indexCount = quadCount * 6u;
            const auto vertexBytes = static_cast<std::uint64_t>(
                request.vertices.size_bytes());
            const auto indexBytes = static_cast<std::uint64_t>(
                indexCount * sizeof(std::uint32_t));
            const auto vertexUpload = m_backend->AllocateFrameUpload(
                vertexBytes,
                alignof(LamaPon::ParticleRenderVertex));
            const auto indexUpload = m_backend->AllocateFrameUpload(
                indexBytes,
                alignof(std::uint32_t));
            std::memcpy(
                vertexUpload.data,
                request.vertices.data(),
                request.vertices.size_bytes());
            auto* indices = reinterpret_cast<std::uint32_t*>(indexUpload.data);
            for (std::size_t quad{}; quad < quadCount; ++quad)
            {
                const auto first = static_cast<std::uint32_t>(quad * 4u);
                const auto offset = quad * 6u;
                indices[offset] = first;
                indices[offset + 1u] = first + 1u;
                indices[offset + 2u] = first + 2u;
                indices[offset + 3u] = first;
                indices[offset + 4u] = first + 2u;
                indices[offset + 5u] = first + 3u;
            }

            DirectX::XMFLOAT4X4 viewProjection{};
            DirectX::XMStoreFloat4x4(
                &viewProjection,
                DirectX::XMMatrixMultiply(
                    DirectX::XMLoadFloat4x4(&request.view),
                    DirectX::XMLoadFloat4x4(&request.projection)));
            const auto constantUpload = m_backend->AllocateFrameUpload(
                sizeof(viewProjection),
                D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            std::memcpy(
                constantUpload.data,
                &viewProjection,
                sizeof(viewProjection));

            const D3D12_VERTEX_BUFFER_VIEW vertexView{
                vertexUpload.gpuAddress,
                static_cast<UINT>(vertexBytes),
                static_cast<UINT>(sizeof(LamaPon::ParticleRenderVertex)) };
            const D3D12_INDEX_BUFFER_VIEW indexView{
                indexUpload.gpuAddress,
                static_cast<UINT>(indexBytes),
                DXGI_FORMAT_R32_UINT };
            ID3D12DescriptorHeap* heaps[]{ descriptorHeap };
            commandList->SetGraphicsRootSignature(m_rootSignature.Get());
            commandList->SetPipelineState(ParticlePipelineState(request.additive));
            commandList->SetDescriptorHeaps(1, heaps);
            commandList->SetGraphicsRootConstantBufferView(
                0,
                constantUpload.gpuAddress);
            commandList->SetGraphicsRootDescriptorTable(1, binding->descriptor);
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commandList->IASetVertexBuffers(0, 1, &vertexView);
            commandList->IASetIndexBuffer(&indexView);
            const auto& viewport = m_backend->PrimaryViewport();
            const auto& scissor = m_backend->PrimaryScissorRectangle();
            commandList->RSSetViewports(1, &viewport);
            commandList->RSSetScissorRects(1, &scissor);
            commandList->DrawIndexedInstanced(
                static_cast<UINT>(indexCount),
                1,
                0,
                0,
                0);
            return true;
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
            const std::array textures{
                request.albedo,
                request.normalTexture,
                request.roughnessTexture,
                request.metallicTexture,
                request.occlusionTexture,
                request.emissiveTexture
            };
            std::array<LamaPon::D3D12Backend::ShaderResourceBinding, 6>
                bindings{};
            ID3D12DescriptorHeap* descriptorHeap{};
            if (!request.depthOnly)
            {
                for (std::size_t index{}; index < textures.size(); ++index)
                {
                    const auto& texture = textures[index]
                        ? textures[index]
                        : request.fallbackTexture;
                    const auto binding =
                        m_backend->TryResolveShaderResource(texture);
                    if (!binding)
                    {
                        return false;
                    }
                    bindings[index] = *binding;
                }
                descriptorHeap =
                    m_backend->ShaderResourceDescriptorHeap();
                if (descriptorHeap == nullptr)
                {
                    return false;
                }
            }
            else if (!m_backend->IsShadowPassActive())
            {
                return false;
            }

            auto* commandList = request.depthOnly
                ? m_backend->CurrentFrameCommands()
                : m_backend->BeginFrameCommands();
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
                DirectX::XMFLOAT4 cameraPosition;
                DirectX::XMFLOAT4 materialProperties;
                DirectX::XMFLOAT4 emissiveFactor;
                DirectX::XMFLOAT4 ambientColorIntensity;
                std::array<DirectX::XMFLOAT4, 4>
                    directionalDirectionIntensity{};
                std::array<DirectX::XMFLOAT4, 4> directionalColors{};
                std::array<std::uint32_t, 4> lightCounts{};
                std::array<DirectX::XMFLOAT4, 16> pointPositionRange{};
                std::array<DirectX::XMFLOAT4, 16> pointColorIntensity{};
                std::array<DirectX::XMFLOAT4, 8> spotPositionRange{};
                std::array<DirectX::XMFLOAT4, 8> spotDirectionInnerCosine{};
                std::array<DirectX::XMFLOAT4, 8> spotColorIntensity{};
                std::array<DirectX::XMFLOAT4, 8> spotOuterCosine{};
            } constants{};
            // HLSLのPrimitiveConstantsと同じ並び・大きさであることを保証します。
            static_assert(sizeof(constants) == 1376u);
            const auto world = DirectX::XMLoadFloat4x4(&request.world);
            const auto view = DirectX::XMLoadFloat4x4(&request.view);
            const auto projection = DirectX::XMLoadFloat4x4(&request.projection);
            DirectX::XMStoreFloat4x4(
                &constants.worldViewProjection,
                DirectX::XMMatrixMultiply(
                    DirectX::XMMatrixMultiply(world, view), projection));
            constants.world = request.world;
            constants.baseColor = request.baseColor;
            const auto inverseView = DirectX::XMMatrixInverse(nullptr, view);
            DirectX::XMStoreFloat4(
                &constants.cameraPosition,
                inverseView.r[3]);
            constants.materialProperties = {
                std::clamp(request.roughness, 0.02f, 1.0f),
                std::clamp(request.metallic, 0.0f, 1.0f),
                std::clamp(request.occlusionStrength, 0.0f, 1.0f),
                request.normalTexture ? 1.0f : 0.0f };
            constants.emissiveFactor = {
                std::max(request.emissiveFactor.x, 0.0f),
                std::max(request.emissiveFactor.y, 0.0f),
                std::max(request.emissiveFactor.z, 0.0f),
                std::clamp(request.normalStrength, 0.0f, 2.0f) };
            // D3D11のLitEffectと同じく、負の環境光強度は0へ丸めます。
            constants.ambientColorIntensity = {
                request.ambientColor.x,
                request.ambientColor.y,
                request.ambientColor.z,
                std::max(request.ambientIntensity, 0.0f) };
            constants.lightCounts[0] = static_cast<std::uint32_t>(
                std::min(
                    request.directionalLightCount,
                    request.directionalLights.size()));
            for (std::size_t index{};
                index < constants.lightCounts[0];
                ++index)
            {
                const auto& light = request.directionalLights[index];
                constants.directionalDirectionIntensity[index] = {
                    light.direction.x,
                    light.direction.y,
                    light.direction.z,
                    light.intensity };
                constants.directionalColors[index] = {
                    light.color.x,
                    light.color.y,
                    light.color.z,
                    1.0f };
            }
            constants.lightCounts[1] = static_cast<std::uint32_t>(
                std::min(
                    request.pointLightCount,
                    request.pointLights.size()));
            for (std::size_t index{};
                index < constants.lightCounts[1];
                ++index)
            {
                const auto& light = request.pointLights[index];
                constants.pointPositionRange[index] = {
                    light.position.x,
                    light.position.y,
                    light.position.z,
                    light.range };
                constants.pointColorIntensity[index] = {
                    light.color.x,
                    light.color.y,
                    light.color.z,
                    light.intensity };
            }
            constants.lightCounts[2] = static_cast<std::uint32_t>(
                std::min(
                    request.spotLightCount,
                    request.spotLights.size()));
            for (std::size_t index{};
                index < constants.lightCounts[2];
                ++index)
            {
                const auto& light = request.spotLights[index];
                constants.spotPositionRange[index] = {
                    light.position.x,
                    light.position.y,
                    light.position.z,
                    light.range };
                constants.spotDirectionInnerCosine[index] = {
                    light.direction.x,
                    light.direction.y,
                    light.direction.z,
                    light.innerConeCosine };
                constants.spotColorIntensity[index] = {
                    light.color.x,
                    light.color.y,
                    light.color.z,
                    light.intensity };
                constants.spotOuterCosine[index] = {
                    light.outerConeCosine,
                    0.0f,
                    0.0f,
                    0.0f };
            }
            const auto constantUpload = m_backend->AllocateFrameUpload(
                sizeof(constants),
                D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            std::memcpy(
                constantUpload.data,
                &constants,
                sizeof(constants));

            const D3D12_VERTEX_BUFFER_VIEW vertexView{
                vertexUpload.gpuAddress,
                static_cast<UINT>(vertexBytes),
                static_cast<UINT>(sizeof(PrimitiveRenderVertex)) };
            const D3D12_INDEX_BUFFER_VIEW indexView{
                indexUpload.gpuAddress,
                static_cast<UINT>(indexBytes),
                DXGI_FORMAT_R32_UINT };
            auto* pipeline = request.depthOnly
                ? DepthOnlyPipelineState()
                : PipelineState(
                    request.alphaBlend,
                    request.depthTest,
                    request.depthWrite);
            commandList->SetGraphicsRootSignature(m_rootSignature.Get());
            commandList->SetPipelineState(pipeline);
            commandList->SetGraphicsRootConstantBufferView(
                0,
                constantUpload.gpuAddress);
            if (!request.depthOnly)
            {
                ID3D12DescriptorHeap* heaps[]{ descriptorHeap };
                commandList->SetDescriptorHeaps(1, heaps);
                for (std::size_t index{}; index < bindings.size(); ++index)
                {
                    commandList->SetGraphicsRootDescriptorTable(
                        static_cast<UINT>(index + 1u),
                        bindings[index].descriptor);
                }
            }
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commandList->IASetVertexBuffers(0, 1, &vertexView);
            commandList->IASetIndexBuffer(&indexView);
            if (!request.depthOnly)
            {
                const auto& viewport = m_backend->PrimaryViewport();
                const auto& scissor =
                    m_backend->PrimaryScissorRectangle();
                commandList->RSSetViewports(1, &viewport);
                commandList->RSSetScissorRects(1, &scissor);
            }
            commandList->DrawIndexedInstanced(
                static_cast<UINT>(indices.size()), 1, 0, 0, 0);
            return true;
        }

    private:
        [[nodiscard]] ID3D12PipelineState* ParticlePipelineState(
            bool additive)
        {
            auto& pipeline = m_particlePipelineStates[additive ? 1u : 0u];
            if (pipeline != nullptr)
            {
                return pipeline.Get();
            }
            static const std::array<D3D12_INPUT_ELEMENT_DESC, 3> inputs{ {
                { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
                    offsetof(LamaPon::ParticleRenderVertex, position),
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
                    offsetof(LamaPon::ParticleRenderVertex, color),
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
                    offsetof(LamaPon::ParticleRenderVertex, textureCoordinate),
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
            } };
            D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
            description.pRootSignature = m_rootSignature.Get();
            description.VS = {
                m_particleVertexShader->GetBufferPointer(),
                m_particleVertexShader->GetBufferSize() };
            description.PS = {
                m_particlePixelShader->GetBufferPointer(),
                m_particlePixelShader->GetBufferSize() };
            description.BlendState = MakeParticleBlendDescription(additive);
            description.SampleMask = std::numeric_limits<UINT>::max();
            description.RasterizerState = MakeRasterizerDescription();
            description.DepthStencilState = MakeDepthDescription(true, false);
            description.InputLayout = {
                inputs.data(),
                static_cast<UINT>(inputs.size()) };
            description.PrimitiveTopologyType =
                D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            description.NumRenderTargets = 1;
            description.RTVFormats[0] =
                LamaPon::D3D12Backend::PrimaryColorFormat;
            description.DSVFormat = LamaPon::D3D12Backend::PrimaryDepthFormat;
            description.SampleDesc.Count = 1;
            ThrowIfFailed(
                m_backend->Device()->CreateGraphicsPipelineState(
                    &description,
                    IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf())),
                "ID3D12Device::CreateGraphicsPipelineState(particle)");
            return pipeline.Get();
        }

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

        [[nodiscard]] ID3D12PipelineState* DepthOnlyPipelineState()
        {
            if (m_depthOnlyPipelineState != nullptr)
            {
                return m_depthOnlyPipelineState.Get();
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
            description.VS = {
                m_vertexShader->GetBufferPointer(),
                m_vertexShader->GetBufferSize() };
            description.BlendState = MakeBlendDescription(false);
            description.SampleMask = std::numeric_limits<UINT>::max();
            description.RasterizerState = MakeRasterizerDescription();
            // Shadow mapの自己遮蔽を抑えるため、深度専用PSOにだけ
            // rasterizer biasを持たせます。
            description.RasterizerState.DepthBias = 1000;
            description.RasterizerState.SlopeScaledDepthBias = 1.0f;
            description.DepthStencilState =
                MakeDepthDescription(true, true);
            description.InputLayout = {
                inputs.data(),
                static_cast<UINT>(inputs.size()) };
            description.PrimitiveTopologyType =
                D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            description.NumRenderTargets = 0;
            description.DSVFormat =
                LamaPon::D3D12Backend::ShadowDepthFormat;
            description.SampleDesc.Count = 1;
            ThrowIfFailed(
                m_backend->Device()->CreateGraphicsPipelineState(
                    &description,
                    IID_PPV_ARGS(
                        m_depthOnlyPipelineState.ReleaseAndGetAddressOf())),
                "ID3D12Device::CreateGraphicsPipelineState(shadow depth)");
            return m_depthOnlyPipelineState.Get();
        }

        LamaPon::D3D12Backend* m_backend{};
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
        Microsoft::WRL::ComPtr<ID3DBlob> m_vertexShader;
        Microsoft::WRL::ComPtr<ID3DBlob> m_pixelShader;
        Microsoft::WRL::ComPtr<ID3DBlob> m_particleVertexShader;
        Microsoft::WRL::ComPtr<ID3DBlob> m_particlePixelShader;
        std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2>
            m_particlePipelineStates;
        std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 3> m_pipelineStates;
        Microsoft::WRL::ComPtr<ID3D12PipelineState>
            m_depthOnlyPipelineState;
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
