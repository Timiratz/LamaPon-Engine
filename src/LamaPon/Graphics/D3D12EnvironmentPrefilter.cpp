#include "LamaPon/Graphics/D3D12EnvironmentPrefilter.h"

#include "LamaPon/Graphics/EnvironmentSettings.h"
#include "LamaPon/Graphics/GraphicsBackend.h"
#include "LamaPon/Graphics/RenderTarget.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace
{
    // D3D11のEnvironmentRendererと同じ大きさです。
    constexpr std::uint32_t SpecularSize = 128u;
    constexpr std::uint32_t SpecularMipLevels = 8u;
    constexpr std::uint32_t IrradianceSize = 16u;
    constexpr std::uint32_t IrradianceMipLevels = 1u;
    // リフレクションプローブの面の一辺です。
    constexpr std::uint32_t ProbeFaceSize =
        LamaPon::EnvironmentProbeBakeFaceSize;
    // HLSLの[numthreads]と同じです。thread groupのzは面の番号です。
    constexpr std::uint32_t ThreadGroupSize = 8u;
    constexpr UINT FaceCount = 6u;

    // LamaPonEnvironment.hlslのCubeDirection／ImportanceSampleGGX／
    // PSPrefilterEnvironment／PSIrradianceと同じ式です。D3D11は面ごとに
    // フルスクリーン三角形を描いて画素中心のuvを補間し、ここではthreadの
    // 画素中心から同じuvを求めます。
    constexpr char PrefilterShaderSource[] = R"(
cbuffer PrefilterPass : register(b0)
{
    // x=粗さ, y=ソース解像度, z=書き込むミップの一辺, w=予約
    float4 PrefilterParameters;
};

TextureCube SkyCubemap : register(t0);
RWTexture2DArray<float4> PrefilterOutput : register(u0);
SamplerState LinearSampler : register(s0);

// キューブ面のUVから方向ベクトルを作ります（D3D11の面順）。
float3 CubeDirection(uint face, float2 uv)
{
    const float2 st = float2(
        uv.x * 2.0f - 1.0f,
        1.0f - uv.y * 2.0f);
    if (face == 0u)
    {
        return normalize(float3(1.0f, st.y, -st.x));
    }
    if (face == 1u)
    {
        return normalize(float3(-1.0f, st.y, st.x));
    }
    if (face == 2u)
    {
        return normalize(float3(st.x, 1.0f, -st.y));
    }
    if (face == 3u)
    {
        return normalize(float3(st.x, -1.0f, st.y));
    }
    if (face == 4u)
    {
        return normalize(float3(st.x, st.y, 1.0f));
    }
    return normalize(float3(-st.x, st.y, -1.0f));
}

float RadicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u)
        | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u)
        | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u)
        | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u)
        | ((bits & 0xFF00FF00u) >> 8u);
    return (float)bits * 2.3283064365386963e-10f;
}

float2 Hammersley(uint index, uint count)
{
    return float2(
        (float)index / (float)count,
        RadicalInverseVdC(index));
}

// GGX分布に沿ったハーフベクトルの重点サンプリング。
float3 ImportanceSampleGGX(
    float2 xi,
    float roughness,
    float3 normal)
{
    const float alpha = roughness * roughness;
    const float phi = 6.2831853f * xi.x;
    const float cosTheta = sqrt(
        (1.0f - xi.y)
        / (1.0f + (alpha * alpha - 1.0f) * xi.y));
    const float sinTheta =
        sqrt(1.0f - cosTheta * cosTheta);
    const float3 halfVector = float3(
        sinTheta * cos(phi),
        sinTheta * sin(phi),
        cosTheta);
    const float3 up =
        abs(normal.z) < 0.999f
            ? float3(0.0f, 0.0f, 1.0f)
            : float3(1.0f, 0.0f, 0.0f);
    const float3 tangent =
        normalize(cross(up, normal));
    const float3 bitangent = cross(normal, tangent);
    return normalize(
        tangent * halfVector.x
        + bitangent * halfVector.y
        + normal * halfVector.z);
}

// スペキュラ事前畳み込み：ミップごとに粗さを上げてGGX畳み込み。
float3 PrefilterEnvironment(uint face, float2 uv)
{
    const float roughness = PrefilterParameters.x;
    const float sourceResolution =
        max(PrefilterParameters.y, 1.0f);
    const float3 normal = CubeDirection(face, uv);

    if (roughness <= 0.001f)
    {
        return SkyCubemap.SampleLevel(
            LinearSampler,
            normal,
            0.0f).rgb;
    }

    const uint SampleCount = 64u;
    const float saTexel =
        4.0f * 3.14159265f
        / (6.0f * sourceResolution
            * sourceResolution);
    float3 color = 0.0f.xxx;
    float weight = 0.0f;
    [loop]
    for (uint index = 0u; index < SampleCount; ++index)
    {
        const float3 halfVector = ImportanceSampleGGX(
            Hammersley(index, SampleCount),
            roughness,
            normal);
        const float3 lightDirection = normalize(
            2.0f * dot(normal, halfVector) * halfVector
            - normal);
        const float normalDotLight =
            saturate(dot(normal, lightDirection));
        if (normalDotLight <= 0.0f)
        {
            continue;
        }
        // pdfからソースミップを選び、ちらつきを抑えます。
        const float normalDotHalf =
            saturate(dot(normal, halfVector));
        const float alpha = roughness * roughness;
        const float denominator =
            normalDotHalf * normalDotHalf
                * (alpha * alpha - 1.0f)
            + 1.0f;
        const float distribution =
            alpha * alpha
            / (3.14159265f
                * denominator * denominator);
        const float pdf =
            distribution * normalDotHalf
                / (4.0f * max(normalDotHalf, 0.0001f))
            + 0.0001f;
        const float saSample =
            1.0f / ((float)SampleCount * pdf);
        const float mip =
            0.5f * log2(saSample / saTexel);
        color +=
            SkyCubemap.SampleLevel(
                LinearSampler,
                lightDirection,
                max(mip, 0.0f)).rgb
            * normalDotLight;
        weight += normalDotLight;
    }
    return color / max(weight, 0.0001f);
}

// 拡散用の放射照度マップ：半球コサイン畳み込み。
float3 Irradiance(uint face, float2 uv)
{
    const float3 normal = CubeDirection(face, uv);
    const float3 up =
        abs(normal.z) < 0.999f
            ? float3(0.0f, 0.0f, 1.0f)
            : float3(1.0f, 0.0f, 0.0f);
    const float3 tangent =
        normalize(cross(up, normal));
    const float3 bitangent = cross(normal, tangent);

    float3 irradiance = 0.0f.xxx;
    float weight = 0.0f;
    const float PhiStep = 6.2831853f / 32.0f;
    const float ThetaStep = 1.5707963f / 8.0f;
    [loop]
    for (uint phiIndex = 0u; phiIndex < 32u; ++phiIndex)
    {
        const float phi = (float)phiIndex * PhiStep;
        [loop]
        for (uint thetaIndex = 0u;
            thetaIndex < 8u;
            ++thetaIndex)
        {
            const float theta =
                ((float)thetaIndex + 0.5f) * ThetaStep;
            const float3 direction =
                tangent * (sin(theta) * cos(phi))
                + bitangent * (sin(theta) * sin(phi))
                + normal * cos(theta);
            const float sampleWeight =
                cos(theta) * sin(theta);
            irradiance +=
                SkyCubemap.SampleLevel(
                    LinearSampler,
                    direction,
                    2.0f).rgb
                * sampleWeight;
            weight += sampleWeight;
        }
    }
    return irradiance / max(weight, 0.0001f);
}

// 書き込むミップの外にはみ出したthreadを捨て、画素中心のuvを返します。
bool TryOutputUv(uint3 id, out float2 uv)
{
    const uint size = (uint)PrefilterParameters.z;
    uv = (float2(id.xy) + 0.5f) / max(PrefilterParameters.z, 1.0f);
    return id.x < size && id.y < size;
}

[numthreads(8, 8, 1)]
void PrefilterEnvironmentMain(uint3 id : SV_DispatchThreadID)
{
    float2 uv;
    if (TryOutputUv(id, uv))
    {
        PrefilterOutput[id] = float4(PrefilterEnvironment(id.z, uv), 1.0f);
    }
}

[numthreads(8, 8, 1)]
void IrradianceMain(uint3 id : SV_DispatchThreadID)
{
    float2 uv;
    if (TryOutputUv(id, uv))
    {
        PrefilterOutput[id] = float4(Irradiance(id.z, uv), 1.0f);
    }
}
)";

    // LamaPonEnvironment.hlslのPSCopyMirrorXと同じ左右反転コピーです。D3D11は
    // 同じ大きさの面へ(1-u, v)をlinear samplerで読むため、画素中心では左右
    // 反対側の画素そのものになります。
    constexpr char MirrorShaderSource[] = R"(
cbuffer MirrorPass : register(b0)
{
    // x=予約, y=書き込む面, z=面の一辺, w=予約
    float4 MirrorParameters;
};

Texture2D<float4> MirrorSource : register(t0);
RWTexture2DArray<float4> MirrorOutput : register(u0);

[numthreads(8, 8, 1)]
void CopyMirrorMain(uint3 id : SV_DispatchThreadID)
{
    const uint size = (uint)MirrorParameters.z;
    if (id.x >= size || id.y >= size)
    {
        return;
    }
    const uint face = (uint)MirrorParameters.y;
    MirrorOutput[uint3(id.xy, face)] =
        MirrorSource.Load(int3((int)(size - 1u - id.x), (int)id.y, 0));
}
)";

    void ThrowIfFailed(
        const HRESULT result,
        const char* const operation)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(
                std::string{ operation }
                + " failed with HRESULT "
                + std::to_string(static_cast<unsigned long>(result)));
        }
    }

    [[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob> CompileEnvironmentShader(
        const std::string_view source,
        const char* const entryPoint)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        const HRESULT result = D3DCompile(
            source.data(),
            source.size(),
            "LamaPonD3D12EnvironmentPrefilter",
            nullptr,
            nullptr,
            entryPoint,
            "cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS
                | D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0,
            bytecode.GetAddressOf(),
            errors.GetAddressOf());
        if (FAILED(result))
        {
            std::string message =
                std::string("D3DCompile(") + entryPoint + ") failed";
            if (errors != nullptr && errors->GetBufferSize() > 0)
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

    [[nodiscard]] bool IsFinite(const DirectX::XMFLOAT3& value) noexcept
    {
        return std::isfinite(value.x)
            && std::isfinite(value.y)
            && std::isfinite(value.z);
    }

    // LitEffect::SetEnvironmentOverrideD3D11と同じく、3軸すべてが正のとき
    // だけボックス射影を有効にします。
    [[nodiscard]] DirectX::XMFLOAT4 BoxParameters(
        const DirectX::XMFLOAT3& extents) noexcept
    {
        const bool active = extents.x > 0.0f
            && extents.y > 0.0f
            && extents.z > 0.0f;
        return { extents.x, extents.y, extents.z, active ? 1.0f : 0.0f };
    }
}

namespace LamaPon::Detail
{
    D3D12ReflectionProbeBindings ResolveD3D12ReflectionProbe(
        const D3D12Backend& backend,
        const ReflectionProbeEnvironment& probe) noexcept
    {
        // ProbeなしはSkyのIBLのままです。片方だけのProbeもD3D11と同じく
        // 使いません。
        if (!probe.specular || !probe.irradiance)
        {
            return {};
        }
        const auto resolveCube = [&backend](
            const GraphicsViewHandle& view,
            const std::uint32_t size,
            D3D12_GPU_DESCRIPTOR_HANDLE& descriptor) noexcept
        {
            const auto binding = backend.TryResolveShaderResource(view);
            if (!binding
                || binding->dimension != D3D12_SRV_DIMENSION_TEXTURECUBE
                || binding->width != size
                || binding->height != size)
            {
                return false;
            }
            descriptor = binding->descriptor;
            return true;
        };

        constexpr auto ExpectedMaximumMip =
            static_cast<float>(SpecularMipLevels - 1u);
        D3D12ReflectionProbeBindings result;
        if (!std::isfinite(probe.intensity)
            || probe.specularMaximumMip != ExpectedMaximumMip
            || !std::isfinite(probe.secondaryWeight)
            || !IsFinite(probe.boxCenter)
            || !IsFinite(probe.boxExtents)
            || !resolveCube(probe.specular, SpecularSize, result.specular)
            || !resolveCube(
                probe.irradiance,
                IrradianceSize,
                result.irradiance))
        {
            return {};
        }
        // 混ぜる指定があるのに2個目を使えないときは、D3D11と同じく
        // プローブ全体を使いません。
        if (probe.secondaryWeight > 0.0f)
        {
            if (!probe.secondarySpecular
                || !probe.secondaryIrradiance
                || probe.secondarySpecularMaximumMip != ExpectedMaximumMip
                || !IsFinite(probe.secondaryBoxCenter)
                || !IsFinite(probe.secondaryBoxExtents)
                || !resolveCube(
                    probe.secondarySpecular,
                    SpecularSize,
                    result.secondarySpecular)
                || !resolveCube(
                    probe.secondaryIrradiance,
                    IrradianceSize,
                    result.secondaryIrradiance))
            {
                return {};
            }
            result.blended = true;
            result.secondaryBoxCenter = {
                probe.secondaryBoxCenter.x,
                probe.secondaryBoxCenter.y,
                probe.secondaryBoxCenter.z,
                0.0f
            };
            result.secondaryBoxParameters =
                BoxParameters(probe.secondaryBoxExtents);
            result.blendParameters = {
                std::clamp(probe.secondaryWeight, 0.0f, 1.0f),
                probe.secondarySpecularMaximumMip,
                0.0f,
                0.0f
            };
        }
        result.active = true;
        result.environmentParameters = {
            std::max(probe.intensity, 0.0f),
            1.0f,
            probe.specularMaximumMip,
            0.0f
        };
        result.boxCenter = {
            probe.boxCenter.x,
            probe.boxCenter.y,
            probe.boxCenter.z,
            0.0f
        };
        result.boxParameters = BoxParameters(probe.boxExtents);
        return result;
    }

    D3D12EnvironmentPrefilter::D3D12EnvironmentPrefilter(
        D3D12Backend& backend)
        : m_backend(&backend)
    {
        auto* const device = backend.Device();
        if (device == nullptr)
        {
            throw std::invalid_argument(
                "The DirectX 12 environment prefilter requires an "
                "initialized backend.");
        }

        // b0の4値、t0のsource、u0の書き込み先ミップ、s0のlinear clamp
        // （D3D11のEnvironmentRendererと同じsampler）です。
        D3D12_DESCRIPTOR_RANGE sourceRange{};
        sourceRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        sourceRange.NumDescriptors = 1;
        sourceRange.BaseShaderRegister = 0;
        D3D12_DESCRIPTOR_RANGE outputRange{};
        outputRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        outputRange.NumDescriptors = 1;
        outputRange.BaseShaderRegister = 0;
        std::array<D3D12_ROOT_PARAMETER, 3> parameters{};
        parameters[0].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants.ShaderRegister = 0;
        parameters[0].Constants.Num32BitValues = 4;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable.NumDescriptorRanges = 1;
        parameters[1].DescriptorTable.pDescriptorRanges = &sourceRange;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[2].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[2].DescriptorTable.NumDescriptorRanges = 1;
        parameters[2].DescriptorTable.pDescriptorRanges = &outputRange;
        parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC description{};
        description.NumParameters = static_cast<UINT>(parameters.size());
        description.pParameters = parameters.data();
        description.NumStaticSamplers = 1;
        description.pStaticSamplers = &sampler;
        Microsoft::WRL::ComPtr<ID3DBlob> serialized;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        const HRESULT serializedResult = D3D12SerializeRootSignature(
            &description,
            D3D_ROOT_SIGNATURE_VERSION_1,
            serialized.GetAddressOf(),
            errors.GetAddressOf());
        if (FAILED(serializedResult))
        {
            std::string message =
                "D3D12SerializeRootSignature(environment prefilter) failed";
            if (errors != nullptr && errors->GetBufferSize() > 0)
            {
                message += ": ";
                message.append(
                    static_cast<const char*>(errors->GetBufferPointer()),
                    errors->GetBufferSize());
            }
            throw std::runtime_error(message);
        }
        ThrowIfFailed(
            device->CreateRootSignature(
                0,
                serialized->GetBufferPointer(),
                serialized->GetBufferSize(),
                IID_PPV_ARGS(m_rootSignature.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateRootSignature(environment prefilter)");

        const auto createPipeline = [this, device](
            const std::string_view source,
            const char* const entryPoint,
            Microsoft::WRL::ComPtr<ID3D12PipelineState>& pipeline)
        {
            const auto bytecode = CompileEnvironmentShader(source, entryPoint);
            D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDescription{};
            pipelineDescription.pRootSignature = m_rootSignature.Get();
            pipelineDescription.CS = {
                bytecode->GetBufferPointer(),
                bytecode->GetBufferSize()
            };
            ThrowIfFailed(
                device->CreateComputePipelineState(
                    &pipelineDescription,
                    IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf())),
                "ID3D12Device::CreateComputePipelineState"
                "(environment prefilter)");
        };
        const std::string_view prefilterSource{
            PrefilterShaderSource,
            sizeof(PrefilterShaderSource) - 1u
        };
        createPipeline(
            prefilterSource,
            "PrefilterEnvironmentMain",
            m_specularPipeline);
        createPipeline(prefilterSource, "IrradianceMain", m_irradiancePipeline);
        createPipeline(
            std::string_view{
                MirrorShaderSource,
                sizeof(MirrorShaderSource) - 1u },
            "CopyMirrorMain",
            m_mirrorPipeline);
    }

    D3D12EnvironmentPrefilter::~D3D12EnvironmentPrefilter() noexcept =
        default;

    PrefilteredEnvironmentViews D3D12EnvironmentPrefilter::Prefilter(
        const GraphicsViewHandle& source,
        const std::uint64_t cacheKey)
    {
        if (m_views.IsValid()
            && source == m_source
            && cacheKey == m_cacheKey)
        {
            return m_views;
        }

        auto views = CreatePrefiltered(source);
        m_source = source;
        m_cacheKey = cacheKey;
        m_views = std::move(views);
        return m_views;
    }

    PrefilteredEnvironmentViews D3D12EnvironmentPrefilter::CreatePrefiltered(
        const GraphicsViewHandle& source)
    {
        // 2本とも書き終えてから返し、途中で失敗しても片方だけの結果を
        // 公開しません。
        const auto specular = m_backend->CreateComputeCubeTarget(
            SpecularSize,
            SpecularMipLevels);
        const auto irradiance = m_backend->CreateComputeCubeTarget(
            IrradianceSize,
            IrradianceMipLevels);
        Convolve(
            m_specularPipeline.Get(),
            source,
            specular,
            SpecularSize,
            SpecularMipLevels);
        Convolve(
            m_irradiancePipeline.Get(),
            source,
            irradiance,
            IrradianceSize,
            IrradianceMipLevels);
        return PrefilteredEnvironmentViews{
            specular.view,
            irradiance.view,
            static_cast<float>(SpecularMipLevels - 1u)
        };
    }

    PrefilteredEnvironmentViews D3D12EnvironmentPrefilter::BakeReflectionProbe(
        const EnvironmentProbeFaceRenderer& renderFace)
    {
        if (!renderFace)
        {
            throw std::invalid_argument(
                "Probe bake requires a face renderer.");
        }
        if (m_probeBakeActive)
        {
            throw std::logic_error(
                "A DirectX 12 reflection probe bake is already running.");
        }
        struct BakeScope final
        {
            explicit BakeScope(bool& flag) noexcept
                : active(flag)
            {
                active = true;
            }
            ~BakeScope() noexcept
            {
                active = false;
            }
            BakeScope(const BakeScope&) = delete;
            BakeScope& operator=(const BakeScope&) = delete;
            bool& active;
        };
        const BakeScope bakeScope{ m_probeBakeActive };

        // 描画先とcubeは同じ大きさ・同じBackend世代の間は使い回します。
        auto& backend = static_cast<GraphicsBackend&>(*m_backend);
        if (m_probeFaceTarget == nullptr)
        {
            m_probeFaceTarget = std::make_unique<RenderTarget>();
        }
        backend.ResizeOffscreenTarget(
            *m_probeFaceTarget,
            ProbeFaceSize,
            ProbeFaceSize);
        if (!m_backend->TryResolveShaderResource(m_probeCube.view))
        {
            m_probeCube = m_backend->CreateComputeCubeTarget(
                ProbeFaceSize,
                1u);
        }

        // D3D11のRenderProbeCubeと同じく、各面を黒と深度1で消してから
        // 描き、左右反転してcubeの面へ写します。
        constexpr float clearColor[4]{};
        for (std::uint32_t face{}; face < FaceCount; ++face)
        {
            backend.BeginOffscreenTarget(*m_probeFaceTarget, clearColor);
            renderFace(face);
            backend.PublishOffscreenTarget(*m_probeFaceTarget);
            CopyMirroredFace(face);
        }
        return CreatePrefiltered(m_probeCube.view);
    }

    void D3D12EnvironmentPrefilter::CopyMirroredFace(const std::uint32_t face)
    {
        auto* const descriptorHeap =
            m_backend->ShaderResourceDescriptorHeap();
        if (descriptorHeap == nullptr
            || m_probeFaceTarget == nullptr
            || m_probeCube.mipAccess.empty())
        {
            throw std::logic_error(
                "The DirectX 12 probe bake requires a shader resource "
                "descriptor heap, a face target and a probe cube.");
        }

        const auto source = m_probeFaceTarget->DisplayViewHandle();
        const auto sourceBinding =
            m_backend->BeginCubeCompute(source, m_probeCube.texture);
        try
        {
            auto* const commandList = m_backend->CurrentFrameCommands();
            ID3D12DescriptorHeap* heaps[]{ descriptorHeap };
            commandList->SetDescriptorHeaps(1, heaps);
            commandList->SetComputeRootSignature(m_rootSignature.Get());
            commandList->SetPipelineState(m_mirrorPipeline.Get());
            const std::array<float, 4> parameters{
                0.0f,
                static_cast<float>(face),
                static_cast<float>(ProbeFaceSize),
                0.0f
            };
            commandList->SetComputeRoot32BitConstants(
                0,
                static_cast<UINT>(parameters.size()),
                parameters.data(),
                0);
            commandList->SetComputeRootDescriptorTable(
                1,
                sourceBinding.descriptor);
            commandList->SetComputeRootDescriptorTable(
                2,
                m_probeCube.mipAccess.front());
            const UINT groups =
                (ProbeFaceSize + ThreadGroupSize - 1u) / ThreadGroupSize;
            commandList->Dispatch(groups, groups, 1u);
        }
        catch (...)
        {
            m_backend->EndCubeCompute(source, m_probeCube.texture);
            throw;
        }
        m_backend->EndCubeCompute(source, m_probeCube.texture);
    }

    void D3D12EnvironmentPrefilter::Convolve(
        ID3D12PipelineState* const pipeline,
        const GraphicsViewHandle& source,
        const D3D12Backend::ComputeCubeTarget& target,
        const std::uint32_t size,
        const std::uint32_t mipLevels)
    {
        auto* const descriptorHeap =
            m_backend->ShaderResourceDescriptorHeap();
        if (descriptorHeap == nullptr
            || target.mipAccess.size() != mipLevels)
        {
            throw std::logic_error(
                "The DirectX 12 environment prefilter requires a shader "
                "resource descriptor heap and one output view per mip.");
        }

        const auto sourceBinding =
            m_backend->BeginCubeCompute(source, target.texture);
        try
        {
            auto* const commandList = m_backend->CurrentFrameCommands();
            ID3D12DescriptorHeap* heaps[]{ descriptorHeap };
            commandList->SetDescriptorHeaps(1, heaps);
            commandList->SetComputeRootSignature(m_rootSignature.Get());
            commandList->SetPipelineState(pipeline);
            commandList->SetComputeRootDescriptorTable(
                1,
                sourceBinding.descriptor);
            for (std::uint32_t mip{}; mip < mipLevels; ++mip)
            {
                const std::uint32_t mipSize = std::max(size >> mip, 1u);
                // D3D11と同じく、ミップ0を粗さ0、最後のミップを粗さ1に
                // します。ソース解像度はsource cubeの1面の幅です。
                const std::array<float, 4> parameters{
                    mipLevels <= 1u
                        ? 0.0f
                        : static_cast<float>(mip)
                            / static_cast<float>(mipLevels - 1u),
                    static_cast<float>(sourceBinding.width),
                    static_cast<float>(mipSize),
                    0.0f
                };
                commandList->SetComputeRoot32BitConstants(
                    0,
                    static_cast<UINT>(parameters.size()),
                    parameters.data(),
                    0);
                commandList->SetComputeRootDescriptorTable(
                    2,
                    target.mipAccess[mip]);
                const UINT groups =
                    (mipSize + ThreadGroupSize - 1u) / ThreadGroupSize;
                commandList->Dispatch(groups, groups, FaceCount);
            }
        }
        catch (...)
        {
            m_backend->EndCubeCompute(source, target.texture);
            throw;
        }
        m_backend->EndCubeCompute(source, target.texture);
    }
}
