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
    // スペキュラ画像の一辺（画素）
    constexpr std::uint32_t SpecularSize = 128u;
    // スペキュラのミップ数
    constexpr std::uint32_t SpecularMipLevels = 8u;
    // 放射照度画像の一辺（画素）
    constexpr std::uint32_t IrradianceSize = 16u;
    // 放射照度のミップ数
    constexpr std::uint32_t IrradianceMipLevels = 1u;
    // プローブ1面の一辺（画素）
    constexpr std::uint32_t ProbeFaceSize =
        LamaPon::EnvironmentProbeBakeFaceSize;
    // 計算スレッド群のxy寸法
    constexpr std::uint32_t ThreadGroupSize = 8u;
    // キューブの面数
    constexpr UINT FaceCount = 6u;

    // 画素中心からD3D11と同じ方向・GGX畳み込み・放射照度を求めるHLSLです。
    // 環境光の畳み込みHLSL
    constexpr char PrefilterShaderSource[] = R"(
// 粗さ・入力寸法・出力寸法のb0
cbuffer PrefilterPass : register(b0)
{
    // x=粗さ, y=ソース解像度, z=書き込むミップの一辺, w=予約
    float4 PrefilterParameters;
};

// 畳み込む空のキューブ画像
TextureCube SkyCubemap : register(t0);
// 畳み込みを書き込むミップ
RWTexture2DArray<float4> PrefilterOutput : register(u0);
// 線形補間のクランプサンプラー
SamplerState LinearSampler : register(s0);

// 面のUVから単位方向を求めます(face: D3D11順の面番号, uv: 面の0～1座標)。
float3 CubeDirection(uint face, float2 uv)
{
    // 面の-1～1座標
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

// ビットを逆順にして0～1の低偏差値へ変換します(bits: サンプル番号)。
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

// 2次元の低偏差サンプルを求めます(index: サンプル番号, count: 正の総サンプル数)。
float2 Hammersley(uint index, uint count)
{
    return float2(
        (float)index / (float)count,
        RadicalInverseVdC(index));
}

// GGX分布の単位ハーフベクトルを求めます(xi: 0～1の2次元サンプル, roughness: 粗さ, normal: 単位法線)。
float3 ImportanceSampleGGX(
    float2 xi,
    float roughness,
    float3 normal)
{
    // 粗さの2乗
    const float alpha = roughness * roughness;
    // 半球の方位角
    const float phi = 6.2831853f * xi.x;
    // 極角の余弦
    const float cosTheta = sqrt(
        (1.0f - xi.y)
        / (1.0f + (alpha * alpha - 1.0f) * xi.y));
    // 極角の正弦
    const float sinTheta =
        sqrt(1.0f - cosTheta * cosTheta);
    // GGXのハーフベクトル
    const float3 halfVector = float3(
        sinTheta * cos(phi),
        sinTheta * sin(phi),
        cosTheta);
    // 法線に平行でない基準軸
    const float3 up =
        abs(normal.z) < 0.999f
            ? float3(0.0f, 0.0f, 1.0f)
            : float3(1.0f, 0.0f, 0.0f);
    // 法線に垂直な接線
    const float3 tangent =
        normalize(cross(up, normal));
    // 法線と接線に垂直な軸
    const float3 bitangent = cross(normal, tangent);
    return normalize(
        tangent * halfVector.x
        + bitangent * halfVector.y
        + normal * halfVector.z);
}

// 粗さに応じてGGX畳み込みを求めます(face: キューブの面番号, uv: 面の0～1座標)。
float3 PrefilterEnvironment(uint face, float2 uv)
{
    // 生成ミップの粗さ
    const float roughness = PrefilterParameters.x;
    // 入力キューブの一辺
    const float sourceResolution =
        max(PrefilterParameters.y, 1.0f);
    // キューブ面の単位方向
    const float3 normal = CubeDirection(face, uv);

    if (roughness <= 0.001f)
    {
        return SkyCubemap.SampleLevel(
            LinearSampler,
            normal,
            0.0f).rgb;
    }

    // GGXの採取数
    const uint SampleCount = 64u;
    // 入力1画素の立体角
    const float saTexel =
        4.0f * 3.14159265f
        / (6.0f * sourceResolution
            * sourceResolution);
    // 重み付きの積算色
    float3 color = 0.0f.xxx;
    // 積算したサンプル重み
    float weight = 0.0f;
    // GGXのサンプル番号
    [loop]
    for (uint index = 0u; index < SampleCount; ++index)
    {
        // GGXのハーフベクトル
        const float3 halfVector = ImportanceSampleGGX(
            Hammersley(index, SampleCount),
            roughness,
            normal);
        // 反射先の採取方向
        const float3 lightDirection = normalize(
            2.0f * dot(normal, halfVector) * halfVector
            - normal);
        // 法線と採取方向の内積
        const float normalDotLight =
            saturate(dot(normal, lightDirection));
        if (normalDotLight <= 0.0f)
        {
            continue;
        }
        // pdfからソースミップを選び、ちらつきを抑えます。
        // 法線とハーフベクトルの内積
        const float normalDotHalf =
            saturate(dot(normal, halfVector));
        // 粗さの2乗
        const float alpha = roughness * roughness;
        // GGX分布の分母係数
        const float denominator =
            normalDotHalf * normalDotHalf
                * (alpha * alpha - 1.0f)
            + 1.0f;
        // GGXの法線分布値
        const float distribution =
            alpha * alpha
            / (3.14159265f
                * denominator * denominator);
        // 採取方向の確率密度
        const float pdf =
            distribution * normalDotHalf
                / (4.0f * max(normalDotHalf, 0.0001f))
            + 0.0001f;
        // 1サンプルの立体角
        const float saSample =
            1.0f / ((float)SampleCount * pdf);
        // 採取する入力ミップ
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

// 半球の余弦重みで放射照度を求めます(face: キューブの面番号, uv: 面の0～1座標)。
float3 Irradiance(uint face, float2 uv)
{
    // キューブ面の単位方向
    const float3 normal = CubeDirection(face, uv);
    // 法線に平行でない基準軸
    const float3 up =
        abs(normal.z) < 0.999f
            ? float3(0.0f, 0.0f, 1.0f)
            : float3(1.0f, 0.0f, 0.0f);
    // 法線に垂直な接線
    const float3 tangent =
        normalize(cross(up, normal));
    // 法線と接線に垂直な軸
    const float3 bitangent = cross(normal, tangent);

    // 重み付きの放射照度
    float3 irradiance = 0.0f.xxx;
    // 積算したサンプル重み
    float weight = 0.0f;
    // 方位角の採取間隔
    const float PhiStep = 6.2831853f / 32.0f;
    // 極角の採取間隔
    const float ThetaStep = 1.5707963f / 8.0f;
    // 方位角の採取番号
    [loop]
    for (uint phiIndex = 0u; phiIndex < 32u; ++phiIndex)
    {
        // 半球の方位角
        const float phi = (float)phiIndex * PhiStep;
        // 極角の採取番号
        [loop]
        for (uint thetaIndex = 0u;
            thetaIndex < 8u;
            ++thetaIndex)
        {
            // 半球の極角
            const float theta =
                ((float)thetaIndex + 0.5f) * ThetaStep;
            // 半球の単位採取方向
            const float3 direction =
                tangent * (sin(theta) * cos(phi))
                + bitangent * (sin(theta) * sin(phi))
                + normal * cos(theta);
            // 余弦と極角の重み
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

// 画素中心のUVを返し、範囲外ならfalseです(id: xy=画素、z=面番号, uv: 画素中心の座標出力)。
bool TryOutputUv(uint3 id, out float2 uv)
{
    // 出力ミップの一辺（画素）
    const uint size = (uint)PrefilterParameters.z;
    uv = (float2(id.xy) + 0.5f) / max(PrefilterParameters.z, 1.0f);
    return id.x < size && id.y < size;
}

// GGX畳み込みを1画素へ書きます(id: xy=画素、z=面番号)。
[numthreads(8, 8, 1)]
void PrefilterEnvironmentMain(uint3 id : SV_DispatchThreadID)
{
    // 画素中心のUV座標
    float2 uv;
    if (TryOutputUv(id, uv))
    {
        PrefilterOutput[id] = float4(PrefilterEnvironment(id.z, uv), 1.0f);
    }
}

// 放射照度を1画素へ書きます(id: xy=画素、z=面番号)。
[numthreads(8, 8, 1)]
void IrradianceMain(uint3 id : SV_DispatchThreadID)
{
    // 画素中心のUV座標
    float2 uv;
    if (TryOutputUv(id, uv))
    {
        PrefilterOutput[id] = float4(Irradiance(id.z, uv), 1.0f);
    }
}
)";

    // 左右反転してキューブ面へ写すHLSLです。
    // キューブ面反転のHLSL
    constexpr char MirrorShaderSource[] = R"(
// 出力面と寸法のb0
cbuffer MirrorPass : register(b0)
{
    // x=予約, y=書き込む面, z=面の一辺, w=予約
    float4 MirrorParameters;
};

// 左右反転する面の画像
Texture2D<float4> MirrorSource : register(t0);
// 反転画像を書き込む面配列
RWTexture2DArray<float4> MirrorOutput : register(u0);

// 左右反転した画素を指定面へ写します(id: xy=画素位置)。
[numthreads(8, 8, 1)]
void CopyMirrorMain(uint3 id : SV_DispatchThreadID)
{
    // 出力ミップの一辺（画素）
    const uint size = (uint)MirrorParameters.z;
    if (id.x >= size || id.y >= size)
    {
        return;
    }
    // 書き込むキューブ面番号
    const uint face = (uint)MirrorParameters.y;
    MirrorOutput[uint3(id.xy, face)] =
        MirrorSource.Load(int3((int)(size - 1u - id.x), (int)id.y, 0));
}
)";

    // 失敗HRESULTを例外へ変換します(result: APIの成否, operation: 操作名)。
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

    // 計算シェーダーをコンパイルします(source: HLSL文字列, entryPoint: 入口の名前)。
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob> CompileEnvironmentShader(
        const std::string_view source,
        const char* const entryPoint)
    {
        // 計算シェーダーのバイトコード
        Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
        // シェーダー処理の診断内容
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        // HLSLコンパイルの成否
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
            // 投げる失敗の説明
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

    // 3成分が全て有限か返します(value: 確認する座標)。
    [[nodiscard]] bool IsFinite(const DirectX::XMFLOAT3& value) noexcept
    {
        return std::isfinite(value.x)
            && std::isfinite(value.y)
            && std::isfinite(value.z);
    }


    // 全軸が正のときだけ射影を有効にします(extents: ボックスの半径)。
    [[nodiscard]] DirectX::XMFLOAT4 BoxParameters(
        const DirectX::XMFLOAT3& extents) noexcept
    {
        // 全軸正値のボックス射影有無
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
        // スペキュラと放射照度が両方有効なプローブだけを使います。
        if (!probe.specular || !probe.irradiance)
        {
            return {};
        }
        // キューブ寸法と世代を確認します(view: 入力ビュー, size: 必須の一辺寸法, descriptor: 成功時の記述子出力)。
        const auto resolveCube = [&backend](
            const GraphicsViewHandle& view,
            const std::uint32_t size,
            D3D12_GPU_DESCRIPTOR_HANDLE& descriptor) noexcept
        {
            // 解決したキューブの入力
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

        // 必須の最終ミップ番号
        constexpr auto ExpectedMaximumMip =
            static_cast<float>(SpecularMipLevels - 1u);
        // 検証済みプローブの描画入力
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
        // 混合する第2プローブも無効なら全体を無効にします。
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
        // 初期化済みD3D12デバイス
        auto* const device = backend.Device();
        if (device == nullptr)
        {
            throw std::invalid_argument(
                "The DirectX 12 environment prefilter requires an "
                "initialized backend.");
        }

        // b0の4値・t0の入力・u0の出力に、線形クランプのs0を設定します。
        // t0の入力記述子範囲
        D3D12_DESCRIPTOR_RANGE sourceRange{};
        sourceRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        sourceRange.NumDescriptors = 1;
        sourceRange.BaseShaderRegister = 0;
        // u0の出力記述子範囲
        D3D12_DESCRIPTOR_RANGE outputRange{};
        outputRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        outputRange.NumDescriptors = 1;
        outputRange.BaseShaderRegister = 0;
        // b0・t0・u0のルート引数
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
        // s0の線形クランプ設定
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        // ルート署名の構成
        D3D12_ROOT_SIGNATURE_DESC description{};
        description.NumParameters = static_cast<UINT>(parameters.size());
        description.pParameters = parameters.data();
        description.NumStaticSamplers = 1;
        description.pStaticSamplers = &sampler;
        // 直列化されたルート署名
        Microsoft::WRL::ComPtr<ID3DBlob> serialized;
        // シェーダー処理の診断内容
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        // ルート署名の直列化結果
        const HRESULT serializedResult = D3D12SerializeRootSignature(
            &description,
            D3D_ROOT_SIGNATURE_VERSION_1,
            serialized.GetAddressOf(),
            errors.GetAddressOf());
        if (FAILED(serializedResult))
        {
            // 投げる失敗の説明
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

        // 計算PSOを生成します(source: HLSL文字列, entryPoint: 入口の名前, pipeline: PSOの出力先)。
        const auto createPipeline = [this, device](
            const std::string_view source,
            const char* const entryPoint,
            Microsoft::WRL::ComPtr<ID3D12PipelineState>& pipeline)
        {
            // 計算シェーダーのバイトコード
            const auto bytecode = CompileEnvironmentShader(source, entryPoint);
            // 計算シェーダーPSOの設定
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
        // 畳み込みHLSLの参照範囲
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

        // 今回生成した環境光ビュー
        auto views = CreatePrefiltered(source);
        m_source = source;
        m_cacheKey = cacheKey;
        m_views = std::move(views);
        return m_views;
    }

    PrefilteredEnvironmentViews D3D12EnvironmentPrefilter::CreatePrefiltered(
        const GraphicsViewHandle& source)
    {
        // 2本とも書き終えてから返し、途中で失敗しても片方だけの結果を公開しません。
        // 生成するスペキュラ画像
        const auto specular = m_backend->CreateComputeCubeTarget(
            SpecularSize,
            SpecularMipLevels);
        // 生成する放射照度画像
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
            // ベイク中フラグを立てます(flag: 終了時に解除するフラグ)。
            explicit BakeScope(bool& flag) noexcept
                : active(flag)
            {
                active = true;
            }
            // ベイク中フラグを解除します。
            ~BakeScope() noexcept
            {
                active = false;
            }
            // ベイクガードのコピーを禁止します。
            BakeScope(const BakeScope&) = delete;
            // ベイクガードのコピー代入を禁止します。
            BakeScope& operator=(const BakeScope&) = delete;
            // 借用するベイク中フラグ
            // 全軸正値のボックス射影有無
            bool& active;
        };
        // ベイク中フラグの解除ガード
        const BakeScope bakeScope{ m_probeBakeActive };

        // 同じ寸法・資源世代のプローブ描画先とキューブを再利用します。
        // API共通の描画先操作
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

        // 6面を黒と深度1で消去して描き、左右反転してキューブへ写します。
        // HDRの黒い消去色
        constexpr float clearColor[4]{};
        // 描画するキューブ面番号
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
        // 入力と出力の記述子ヒープ
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

        // 反転元の表示画像ビュー
        const auto source = m_probeFaceTarget->DisplayViewHandle();
        // 計算処理で使用する入力
        const auto sourceBinding =
            m_backend->BeginCubeCompute(source, m_probeCube.texture);
        try
        {
            // 現在の計算命令リスト
            auto* const commandList = m_backend->CurrentFrameCommands();
            // 計算処理で使用するヒープ列
            ID3D12DescriptorHeap* heaps[]{ descriptorHeap };
            commandList->SetDescriptorHeaps(1, heaps);
            commandList->SetComputeRootSignature(m_rootSignature.Get());
            commandList->SetPipelineState(m_mirrorPipeline.Get());
            // b0の処理定数
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
            // xy方向のスレッド群数
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
        // 入力と出力の記述子ヒープ
        auto* const descriptorHeap =
            m_backend->ShaderResourceDescriptorHeap();
        if (descriptorHeap == nullptr
            || target.mipAccess.size() != mipLevels)
        {
            throw std::logic_error(
                "The DirectX 12 environment prefilter requires a shader "
                "resource descriptor heap and one output view per mip.");
        }

        // 計算処理で使用する入力
        const auto sourceBinding =
            m_backend->BeginCubeCompute(source, target.texture);
        try
        {
            // 現在の計算命令リスト
            auto* const commandList = m_backend->CurrentFrameCommands();
            // 計算処理で使用するヒープ列
            ID3D12DescriptorHeap* heaps[]{ descriptorHeap };
            commandList->SetDescriptorHeaps(1, heaps);
            commandList->SetComputeRootSignature(m_rootSignature.Get());
            commandList->SetPipelineState(pipeline);
            commandList->SetComputeRootDescriptorTable(
                1,
                sourceBinding.descriptor);
            // 生成するミップ番号
            for (std::uint32_t mip{}; mip < mipLevels; ++mip)
            {
                // 生成ミップの一辺（画素）
                const std::uint32_t mipSize = std::max(size >> mip, 1u);
                // 粗さをミップ0の0から最終ミップの1まで変化させ、入力1面の解像度で採取します。
                // b0の処理定数
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
                // xy方向のスレッド群数
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
