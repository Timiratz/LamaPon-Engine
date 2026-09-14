#include "LamaPon/Graphics/D3D12MaterialShaderRenderer.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/Time.h"
#include "LamaPon/Graphics/ClusteredLights.h"
#include "LamaPon/Graphics/D3D12Backend.h"
#include "LamaPon/Graphics/D3D12EnvironmentPrefilter.h"
#include "LamaPon/Graphics/Lighting.h"
#include "LamaPon/Graphics/LitMaterial.h"
#include "LamaPon/Graphics/ShaderCompiler.h"

#include <DirectXMath.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace
{
    using LamaPon::LitMaterial;
    using LamaPon::PrimitiveRenderVertex;
    using LamaPon::ShaderBlendMode;
    using LamaPon::ShaderCullMode;

    constexpr std::size_t ConstantBufferCount = 4u;
    constexpr std::size_t TextureSlotCount = 26u;

    // LamaPonLit.hlslのObjectBuffer（b0）と同じ432 bytesです。
    struct ObjectConstants final
    {
        DirectX::XMFLOAT4X4 world{};
        DirectX::XMFLOAT4X4 viewProjection{};
        DirectX::XMFLOAT4X4 worldInverseTranspose{};
        DirectX::XMFLOAT4 materialColor{ 1.0f, 1.0f, 1.0f, 1.0f };
        DirectX::XMFLOAT4 cameraPosition{};
        DirectX::XMFLOAT4 cameraForward{};
        DirectX::XMFLOAT4 materialParameters{ 0.5f, 1.0f, 0.0f, 0.0f };
        std::array<DirectX::XMFLOAT4, LitMaterial::CustomParameterCount>
            customParameters{};
        DirectX::XMFLOAT4 materialTextureParameters{ 0.0f, 0.0f, 0.0f, 1.0f };
        DirectX::XMFLOAT4 emissiveParameters{};
        DirectX::XMFLOAT4 timeParameters{};
    };
    static_assert(sizeof(ObjectConstants) == 432u);

    struct DirectionalConstants final
    {
        DirectX::XMFLOAT4 directionIntensity{};
        DirectX::XMFLOAT4 color{};
    };

    struct PointConstants final
    {
        DirectX::XMFLOAT4 positionRange{};
        DirectX::XMFLOAT4 colorIntensity{};
    };

    struct SpotConstants final
    {
        DirectX::XMFLOAT4 positionRange{};
        DirectX::XMFLOAT4 directionInnerCosine{};
        DirectX::XMFLOAT4 colorIntensity{};
        DirectX::XMFLOAT4 outerCosinePadding{};
    };

    // LamaPonLit.hlslのLightingBuffer（b1）と同じ2176 bytesです。
    struct LightingConstants final
    {
        DirectX::XMFLOAT4 ambient{};
        std::array<std::uint32_t, 4> lightCounts{};
        std::array<DirectionalConstants, LamaPon::MaximumDirectionalLights>
            directionalLights{};
        std::array<PointConstants, LamaPon::MaximumPointLights>
            pointLights{};
        std::array<SpotConstants, LamaPon::MaximumSpotLights> spotLights{};
        std::array<DirectX::XMFLOAT4X4, LamaPon::MaximumShadowCascades>
            shadowViewProjections{};
        DirectX::XMFLOAT4 shadowCascadeSplits{};
        DirectX::XMFLOAT4 shadowParameters{};
        DirectX::XMFLOAT4 fogColor{};
        DirectX::XMFLOAT4 fogParameters{};
        DirectX::XMFLOAT4 environmentParameters{};
        std::array<DirectX::XMFLOAT4X4, LamaPon::MaximumSpotShadows>
            spotShadowViewProjections{};
        std::array<DirectX::XMFLOAT4, LamaPon::MaximumSpotShadows>
            spotShadowParameters{};
        DirectX::XMFLOAT4 pointShadowParameters{};
        DirectX::XMFLOAT4 shadowTexelSizes{};
        DirectX::XMFLOAT4 screenAmbientOcclusionParameters{};
        DirectX::XMFLOAT4 clusteredParameters{};
        DirectX::XMFLOAT4 clusteredDepthParameters{};
        DirectX::XMFLOAT4 clusteredScreenParameters{};
        DirectX::XMFLOAT4 reflectionBoxCenter{};
        DirectX::XMFLOAT4 reflectionBoxParameters{};
        DirectX::XMFLOAT4 reflectionSecondaryBoxCenter{};
        DirectX::XMFLOAT4 reflectionSecondaryBoxParameters{};
        DirectX::XMFLOAT4 reflectionBlendParameters{};
        DirectX::XMFLOAT4 screenReflectionParameters{};
        DirectX::XMFLOAT4 screenReflectionScreen{};
        DirectX::XMFLOAT4 screenReflectionQuality{};
        DirectX::XMFLOAT4X4 screenReflectionPreviousViewProjection{};
        DirectX::XMFLOAT4 bakedGiVolumeMinimum{};
        DirectX::XMFLOAT4 bakedGiInverseSize{};
        DirectX::XMFLOAT4 bakedGiResolution{};
    };
    static_assert(sizeof(LightingConstants) == 2176u);

    // LamaPonLit.hlslのBoneBuffer（b2）と同じ72本のfloat4x3です。
    struct BoneConstants final
    {
        std::array<DirectX::XMFLOAT3X4, 72> transforms{};
    };
    static_assert(sizeof(BoneConstants) == 3456u);

    // b3の自作Shader用ベクトル枠です。
    struct CustomVectorConstants final
    {
        std::array<DirectX::XMFLOAT4, LitMaterial::CustomVectorCount>
            vectors{};
    };
    static_assert(sizeof(CustomVectorConstants) == 1024u);

    // 内蔵のスキニング頂点シェーダーが読むb4です。
    struct SkinningConstants final
    {
        DirectX::XMFLOAT4X4 world{};
        DirectX::XMFLOAT4X4 worldInverseTranspose{};
        DirectX::XMFLOAT4X4 worldViewProjection{};
        DirectX::XMFLOAT4 diffuseColor{};
        DirectX::XMFLOAT4 fogVector{};
        std::array<DirectX::XMFLOAT3X4, 72> bones{};
    };
    static_assert(sizeof(SkinningConstants) == 3680u);

    constexpr UINT SkinningParameter =
        static_cast<UINT>(ConstantBufferCount + TextureSlotCount);
    // DirectXTKのVertexPositionNormalTangentColorTextureSkinningと同じです。
    constexpr std::uint32_t SkinnedVertexStride = 60u;

    // D3D11でglTF／FBXのMaterial custom shaderと組み合わせる、DirectXTK
    // SkinnedEffectのper-pixel lighting 4 bone頂点シェーダーと同じ計算です。
    // 出力の並びはPSSkinnedMainが受け取るSkinnedPixelInputと一致します。
    // 自作Shaderのb0〜b3と重ならないよう、定数はb4へ置きます。
    constexpr char SkinnedVertexShaderSource[] = R"(
cbuffer SkinnedVertexParameters : register(b4)
{
    row_major float4x4 World;
    row_major float4x4 WorldInverseTranspose;
    row_major float4x4 WorldViewProj;
    float4 DiffuseColor;
    float4 FogVector;
    float4x3 Bones[72];
};

struct VertexInput
{
    float4 Position : SV_Position;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD0;
    uint4 Indices : BLENDINDICES0;
    float4 Weights : BLENDWEIGHT0;
};

struct VertexOutput
{
    float2 TexCoord : TEXCOORD0;
    float4 PositionWS : TEXCOORD1;
    float3 NormalWS : TEXCOORD2;
    float4 Diffuse : COLOR0;
    float4 PositionPS : SV_Position;
};

VertexOutput SkinnedVertexShader(VertexInput input)
{
    float4x3 skinning = 0;
    for (int index = 0; index < 4; index++)
    {
        skinning += Bones[input.Indices[index]] * input.Weights[index];
    }
    float4 position = input.Position;
    position.xyz = mul(input.Position, skinning);
    float3 normal = mul(input.Normal, (float3x3)skinning);

    VertexOutput output;
    output.PositionPS = mul(position, WorldViewProj);
    output.PositionWS = float4(
        mul(position, World).xyz,
        saturate(dot(position, FogVector)));
    output.NormalWS = normalize(
        mul(normal, (float3x3)WorldInverseTranspose));
    output.Diffuse = float4(1.0f, 1.0f, 1.0f, DiffuseColor.a);
    output.TexCoord = input.TexCoord;
    return output;
}

// LamaPonShaderError.hlslと同じマゼンタの代替表示です。
float4 SkinnedErrorPixelShader(VertexOutput input) : SV_Target
{
    return float4(1.0f, 0.0f, 1.0f, 1.0f);
}
)";

    [[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob> CompileEmbeddedShader(
        const char* const source,
        const std::size_t sourceSize,
        const char* const sourceName,
        const char* const entryPoint,
        const char* const target)
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
            std::string message =
                std::string("D3DCompile(") + entryPoint + ") failed";
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

    enum class BlendKind : std::uint8_t
    {
        Opaque,
        NonPremultiplied,
        AdditivePreservingAlpha,
        Premultiplied,
        // DirectXTKのCommonStates::Additiveです。
        Additive
    };

    enum class DepthKind : std::uint8_t
    {
        Default,
        Read,
        None,
        // LitEffectの遮蔽表示です。書き込まず、奥にあるときだけ通します。
        Occluded
    };

    // D3D12がviewを解決できた入力です。
    struct LightingViews final
    {
        bool directionalShadow{};
        bool spotShadow{};
        bool pointShadow{};
        bool screenAmbientOcclusion{};
        bool screenReflection{};
        bool environment{};
        bool prefilteredEnvironment{};
        bool clustered{};
        bool bakedGlobalIllumination{};
    };

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

    [[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob> TryCompileShader(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& path,
        const char* const entryPoint,
        const char* const target,
        const std::vector<std::string>& keywords) noexcept
    {
        try
        {
            return LamaPon::CompileShaderCached(
                assets,
                path,
                entryPoint,
                target,
                keywords);
        }
        catch (...)
        {
            return {};
        }
    }

    // D3D11のLitEffectと同じく、三角形以外を受け取るGSMainは拒否します。
    [[nodiscard]] bool TakesTriangles(ID3DBlob* const byteCode) noexcept
    {
        Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflection;
        D3D11_SHADER_DESC description{};
        if (FAILED(D3DReflect(
                byteCode->GetBufferPointer(),
                byteCode->GetBufferSize(),
                IID_ID3D11ShaderReflection,
                &reflection))
            || FAILED(reflection->GetDesc(&description)))
        {
            return true;
        }
        return description.InputPrimitive == D3D_PRIMITIVE_TRIANGLE;
    }

    // stageが読むb0〜b3を記録します。読まない枠へfloat bufferを
    // 毎描画送らないためで、読めないときは全枠を送ります。
    void MarkConstantBuffers(
        ID3DBlob* const byteCode,
        std::array<bool, ConstantBufferCount>& buffers) noexcept
    {
        if (byteCode == nullptr)
        {
            return;
        }
        Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflection;
        D3D11_SHADER_DESC description{};
        if (FAILED(D3DReflect(
                byteCode->GetBufferPointer(),
                byteCode->GetBufferSize(),
                IID_ID3D11ShaderReflection,
                &reflection))
            || FAILED(reflection->GetDesc(&description)))
        {
            buffers.fill(true);
            return;
        }
        for (UINT index{}; index < description.BoundResources; ++index)
        {
            D3D11_SHADER_INPUT_BIND_DESC binding{};
            if (SUCCEEDED(reflection->GetResourceBindingDesc(index, &binding))
                && binding.Type == D3D_SIT_CBUFFER
                && binding.BindPoint < buffers.size())
            {
                buffers[binding.BindPoint] = true;
            }
        }
    }

    [[nodiscard]] BlendKind ToBlendKind(const ShaderBlendMode blend) noexcept
    {
        switch (blend)
        {
        case ShaderBlendMode::Alpha:
            return BlendKind::NonPremultiplied;
        case ShaderBlendMode::Additive:
            return BlendKind::AdditivePreservingAlpha;
        case ShaderBlendMode::Premultiplied:
            return BlendKind::Premultiplied;
        case ShaderBlendMode::Opaque:
        default:
            return BlendKind::Opaque;
        }
    }

    // DirectXTKのCommonStatesと同じ係数です。加算だけはD3D11の
    // CreateAdditiveBlendPreservingAlphaと同じく書き込み先のアルファを保ちます。
    [[nodiscard]] D3D12_BLEND_DESC MakeBlendDescription(
        const BlendKind blend) noexcept
    {
        D3D12_RENDER_TARGET_BLEND_DESC target{};
        target.BlendOp = D3D12_BLEND_OP_ADD;
        target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        switch (blend)
        {
        case BlendKind::NonPremultiplied:
            target.BlendEnable = TRUE;
            target.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            target.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            target.SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
            target.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            break;
        case BlendKind::AdditivePreservingAlpha:
            target.BlendEnable = TRUE;
            target.SrcBlend = D3D12_BLEND_ONE;
            target.DestBlend = D3D12_BLEND_ONE;
            target.SrcBlendAlpha = D3D12_BLEND_ZERO;
            target.DestBlendAlpha = D3D12_BLEND_ONE;
            break;
        case BlendKind::Additive:
            target.BlendEnable = TRUE;
            target.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            target.DestBlend = D3D12_BLEND_ONE;
            target.SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
            target.DestBlendAlpha = D3D12_BLEND_ONE;
            break;
        case BlendKind::Premultiplied:
            target.BlendEnable = TRUE;
            target.SrcBlend = D3D12_BLEND_ONE;
            target.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            target.SrcBlendAlpha = D3D12_BLEND_ONE;
            target.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            break;
        case BlendKind::Opaque:
        default:
            target.BlendEnable = FALSE;
            target.SrcBlend = D3D12_BLEND_ONE;
            target.DestBlend = D3D12_BLEND_ZERO;
            target.SrcBlendAlpha = D3D12_BLEND_ONE;
            target.DestBlendAlpha = D3D12_BLEND_ZERO;
            break;
        }
        D3D12_BLEND_DESC result{};
        for (auto& renderTarget : result.RenderTarget)
        {
            renderTarget = target;
        }
        return result;
    }

    // DirectXTKのCullCounterClockwise／CullClockwise／CullNoneと同じく、
    // 時計回りを表面とします。影のbiasはD3D12の組み込み深度pipelineと
    // 同じ値です。
    [[nodiscard]] D3D12_RASTERIZER_DESC MakeRasterizerDescription(
        const ShaderCullMode cull,
        const bool shadowBias,
        const bool wireframe) noexcept
    {
        D3D12_RASTERIZER_DESC result{};
        result.FillMode = wireframe
            ? D3D12_FILL_MODE_WIREFRAME
            : D3D12_FILL_MODE_SOLID;
        result.CullMode = cull == ShaderCullMode::Front
            ? D3D12_CULL_MODE_FRONT
            : cull == ShaderCullMode::None
                ? D3D12_CULL_MODE_NONE
                : D3D12_CULL_MODE_BACK;
        result.FrontCounterClockwise = FALSE;
        result.DepthBias = shadowBias ? 1000 : D3D12_DEFAULT_DEPTH_BIAS;
        result.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        result.SlopeScaledDepthBias = shadowBias
            ? 1.0f
            : D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        result.DepthClipEnable = TRUE;
        // DirectXTKのCommonStatesと同じく、辺は四角形の線で描きます。
        result.MultisampleEnable = wireframe ? TRUE : FALSE;
        return result;
    }

    // DirectXTKのDepthDefault／DepthRead／DepthNoneと、LitEffectの遮蔽表示の
    // 深度です。
    [[nodiscard]] D3D12_DEPTH_STENCIL_DESC MakeDepthDescription(
        const DepthKind depth) noexcept
    {
        D3D12_DEPTH_STENCIL_DESC result{};
        result.DepthEnable = depth != DepthKind::None;
        result.DepthWriteMask = depth == DepthKind::Default
            ? D3D12_DEPTH_WRITE_MASK_ALL
            : D3D12_DEPTH_WRITE_MASK_ZERO;
        result.DepthFunc = depth == DepthKind::Occluded
            ? D3D12_COMPARISON_FUNC_GREATER
            : D3D12_COMPARISON_FUNC_LESS_EQUAL;
        result.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        result.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        return result;
    }

    // b0〜b3と、1つずつのdescriptor tableにしたt0〜t25です。s0はLitEffectの
    // ActiveMaterialSamplerと同じく線形か点で、s1は影の比較サンプラーです。
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D12RootSignature>
        CreateMaterialRootSignature(
            ID3D12Device* const device,
            const bool pointSampler)
    {
        std::array<D3D12_DESCRIPTOR_RANGE, TextureSlotCount> ranges{};
        for (UINT index{}; index < ranges.size(); ++index)
        {
            ranges[index].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            ranges[index].NumDescriptors = 1;
            ranges[index].BaseShaderRegister = index;
        }
        std::array<
            D3D12_ROOT_PARAMETER,
            ConstantBufferCount + TextureSlotCount + 1u> parameters{};
        for (UINT index{}; index < ConstantBufferCount; ++index)
        {
            parameters[index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            parameters[index].Descriptor.ShaderRegister = index;
            parameters[index].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        for (std::size_t index{}; index < ranges.size(); ++index)
        {
            auto& parameter = parameters[ConstantBufferCount + index];
            parameter.ParameterType =
                D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameter.DescriptorTable.NumDescriptorRanges = 1;
            parameter.DescriptorTable.pDescriptorRanges = &ranges[index];
            parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        auto& skinning = parameters[SkinningParameter];
        skinning.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        skinning.Descriptor.ShaderRegister = 4;
        skinning.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        std::array<D3D12_STATIC_SAMPLER_DESC, 2> samplers{};
        auto& material = samplers[0];
        material.Filter = pointSampler
            ? D3D12_FILTER_MIN_MAG_MIP_POINT
            : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        material.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        material.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        material.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        material.MaxAnisotropy = 1;
        material.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        material.MaxLOD = D3D12_FLOAT32_MAX;
        material.ShaderRegister = 0;
        material.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        auto& shadow = samplers[1];
        shadow.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        shadow.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        shadow.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        shadow.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        shadow.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        shadow.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        shadow.MinLOD = 0.0f;
        shadow.MaxLOD = 0.0f;
        shadow.ShaderRegister = 1;
        shadow.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC description{};
        description.NumParameters = static_cast<UINT>(parameters.size());
        description.pParameters = parameters.data();
        description.NumStaticSamplers = static_cast<UINT>(samplers.size());
        description.pStaticSamplers = samplers.data();
        description.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
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
                "D3D12SerializeRootSignature(material shader) failed";
            if (errors != nullptr && errors->GetBufferSize() > 0)
            {
                message += ": ";
                message.append(
                    static_cast<const char*>(errors->GetBufferPointer()),
                    errors->GetBufferSize());
            }
            throw std::runtime_error(message);
        }
        Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
        ThrowIfFailed(
            device->CreateRootSignature(
                0,
                serialized->GetBufferPointer(),
                serialized->GetBufferSize(),
                IID_PPV_ARGS(rootSignature.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateRootSignature(material shader)");
        return rootSignature;
    }

    // D3D11のLitEffect::SetLightingD3D11と同じ規則でLightingBufferを作り、
    // 実際に読める入力をactiveへ返します。D3D12でまだ用意していない
    // ベイクした間接光は、D3D11でviewが無いときと同じく無効として
    // 扱います。
    [[nodiscard]] LightingConstants BuildLightingConstants(
        const LamaPon::LightingState& lighting,
        const LightingViews& views,
        LightingViews& active) noexcept
    {
        LightingConstants constants;
        const float ambientIntensity =
            std::max(lighting.ambientIntensity, 0.0f);
        constants.ambient = {
            lighting.ambientColor.x * ambientIntensity,
            lighting.ambientColor.y * ambientIntensity,
            lighting.ambientColor.z * ambientIntensity,
            1.0f
        };
        constants.fogColor = {
            lighting.fog.color.x,
            lighting.fog.color.y,
            lighting.fog.color.z,
            1.0f
        };
        constants.fogParameters = {
            lighting.fog.startDistance,
            lighting.fog.endDistance,
            lighting.fog.density,
            lighting.fog.enabled ? 1.0f : 0.0f
        };
        constants.lightCounts = {
            static_cast<std::uint32_t>(std::min(
                lighting.directionalLightCount,
                LamaPon::MaximumDirectionalLights)),
            static_cast<std::uint32_t>(std::min(
                lighting.pointLightCount,
                LamaPon::MaximumPointLights)),
            static_cast<std::uint32_t>(std::min(
                lighting.spotLightCount,
                LamaPon::MaximumSpotLights)),
            static_cast<std::uint32_t>(std::min(
                lighting.directionalShadow.cascadeCount,
                LamaPon::MaximumShadowCascades))
        };
        for (std::size_t index{}; index < constants.lightCounts[0]; ++index)
        {
            const auto& source = lighting.directionalLights[index];
            constants.directionalLights[index] = {
                { source.direction.x,
                    source.direction.y,
                    source.direction.z,
                    source.intensity },
                // color.wへ太陽の角半径を格納します。
                { source.color.x,
                    source.color.y,
                    source.color.z,
                    source.angularRadius }
            };
        }
        for (std::size_t index{}; index < constants.lightCounts[1]; ++index)
        {
            const auto& source = lighting.pointLights[index];
            constants.pointLights[index] = {
                { source.position.x,
                    source.position.y,
                    source.position.z,
                    source.range },
                { source.color.x,
                    source.color.y,
                    source.color.z,
                    source.intensity }
            };
        }
        for (std::size_t index{}; index < constants.lightCounts[2]; ++index)
        {
            const auto& source = lighting.spotLights[index];
            constants.spotLights[index] = {
                { source.position.x,
                    source.position.y,
                    source.position.z,
                    source.range },
                { source.direction.x,
                    source.direction.y,
                    source.direction.z,
                    source.innerConeCosine },
                { source.color.x,
                    source.color.y,
                    source.color.z,
                    source.intensity },
                { source.outerConeCosine, 0.0f, 0.0f, 0.0f }
            };
        }

        const auto& shadow = lighting.directionalShadow;
        active.directionalShadow = shadow.enabled
            && views.directionalShadow
            && shadow.cascadeCount != 0
            && shadow.lightIndex < constants.lightCounts[0];
        if (!active.directionalShadow)
        {
            constants.lightCounts[3] = 0u;
        }
        constants.shadowViewProjections = shadow.lightViewProjections;
        constants.shadowCascadeSplits = {
            shadow.cascadeSplits[0],
            shadow.cascadeSplits[1],
            shadow.cascadeSplits[2],
            shadow.cascadeSplits[3]
        };
        constants.shadowParameters = {
            active.directionalShadow
                ? static_cast<float>(shadow.lightIndex + 1)
                : 0.0f,
            shadow.bias,
            shadow.normalBias,
            shadow.strength
        };

        active.spotShadow = false;
        for (std::size_t slot{}; slot < LamaPon::MaximumSpotShadows; ++slot)
        {
            const auto& spotShadow = lighting.spotShadows[slot];
            if (!views.spotShadow
                || !spotShadow.enabled
                || spotShadow.lightIndex < 0
                || static_cast<std::size_t>(spotShadow.lightIndex)
                    >= constants.lightCounts[2])
            {
                continue;
            }
            constants.spotShadowViewProjections[slot] =
                spotShadow.lightViewProjection;
            constants.spotShadowParameters[slot] = {
                spotShadow.bias,
                spotShadow.normalBias,
                spotShadow.strength,
                1.0f
            };
            constants.spotLights[
                static_cast<std::size_t>(spotShadow.lightIndex)]
                .outerCosinePadding.y = static_cast<float>(slot + 1);
            active.spotShadow = true;
        }

        const auto& pointShadow = lighting.pointShadow;
        active.pointShadow = pointShadow.enabled
            && views.pointShadow
            && pointShadow.lightIndex >= 0
            && static_cast<std::size_t>(pointShadow.lightIndex)
                < constants.lightCounts[1];
        constants.pointShadowParameters = {
            active.pointShadow
                ? static_cast<float>(pointShadow.lightIndex + 1)
                : 0.0f,
            pointShadow.bias,
            pointShadow.strength,
            0.0f
        };
        constants.shadowTexelSizes = {
            1.0f / std::max(lighting.directionalShadowResolution, 1.0f),
            1.0f / std::max(lighting.localShadowResolution, 1.0f),
            1.0f / std::max(lighting.localShadowResolution, 1.0f),
            0.0f
        };

        const auto& occlusion = lighting.screenAmbientOcclusion;
        active.screenAmbientOcclusion =
            occlusion.enabled && views.screenAmbientOcclusion;
        constants.screenAmbientOcclusionParameters = {
            occlusion.inverseWidth,
            occlusion.inverseHeight,
            active.screenAmbientOcclusion ? 1.0f : 0.0f,
            0.0f
        };

        const auto& reflection = lighting.screenSpaceReflection;
        active.screenReflection =
            reflection.enabled && views.screenReflection;
        constants.screenReflectionParameters = {
            std::clamp(reflection.intensity, 0.0f, 1.0f),
            active.screenReflection ? 1.0f : 0.0f,
            std::max(reflection.maximumDistance, 0.01f),
            static_cast<float>(std::clamp<std::uint32_t>(
                reflection.stepCount,
                1u,
                128u))
        };
        constants.screenReflectionScreen = {
            reflection.inverseWidth,
            reflection.inverseHeight,
            reflection.projectionZ,
            reflection.projectionW
        };
        constants.screenReflectionQuality = {
            std::max(reflection.thickness, 0.001f),
            std::clamp(reflection.roughnessCutoff, 0.0f, 1.0f),
            static_cast<float>(reflection.depthPyramidMaximumMip),
            0.0f
        };
        constants.screenReflectionPreviousViewProjection =
            reflection.previousViewProjection;

        // cubemapを読めるときだけIBLを有効にし、事前畳み込みの2本が揃えば
        // zへ最終ミップ番号を載せます（0ならcubemapを直接読みます）。
        const auto& environment = lighting.environment;
        active.environment = environment.enabled && views.environment;
        active.prefilteredEnvironment =
            active.environment && views.prefilteredEnvironment;
        constants.environmentParameters = {
            active.environment ? std::max(environment.intensity, 0.0f) : 0.0f,
            active.environment ? 1.0f : 0.0f,
            active.prefilteredEnvironment
                ? environment.specularMaximumMip
                : 0.0f,
            0.0f
        };

        // 3本のviewが揃ったときだけ、D3D11のLitEffectと同じくForward+を
        // 有効にします。
        const auto& clustered = lighting.clustered;
        active.clustered = clustered.enabled && views.clustered;
        constants.clusteredParameters = {
            static_cast<float>(LamaPon::ClusteredLights::GridWidth),
            static_cast<float>(LamaPon::ClusteredLights::GridHeight),
            static_cast<float>(LamaPon::ClusteredLights::GridDepth),
            active.clustered ? 1.0f : 0.0f
        };
        constants.clusteredDepthParameters = {
            clustered.nearPlane,
            clustered.farPlane,
            std::log(std::max(
                clustered.farPlane
                    / std::max(clustered.nearPlane, 0.0001f),
                1.0001f)),
            static_cast<float>(
                LamaPon::ClusteredLights::MaximumLightsPerCluster)
        };
        constants.clusteredScreenParameters = {
            clustered.inverseWidth,
            clustered.inverseHeight,
            static_cast<float>(clustered.lightCount),
            0.0f
        };

        // RGB別の3本のTexture3Dが揃ったときだけ、LitEffectと同じくベイク
        // した間接光を有効にします。
        const auto& bakedGi = lighting.bakedGlobalIllumination;
        active.bakedGlobalIllumination =
            bakedGi.enabled && views.bakedGlobalIllumination;
        constants.bakedGiVolumeMinimum = {
            bakedGi.volumeMinimum.x,
            bakedGi.volumeMinimum.y,
            bakedGi.volumeMinimum.z,
            active.bakedGlobalIllumination ? 1.0f : 0.0f
        };
        constants.bakedGiInverseSize = {
            1.0f / std::max(bakedGi.volumeSize.x, 0.0001f),
            1.0f / std::max(bakedGi.volumeSize.y, 0.0001f),
            1.0f / std::max(bakedGi.volumeSize.z, 0.0001f),
            std::max(bakedGi.intensity, 0.0f)
        };
        constants.bakedGiResolution = {
            std::max(bakedGi.resolution.x, 1.0f),
            std::max(bakedGi.resolution.y, 1.0f),
            std::max(bakedGi.resolution.z, 1.0f),
            0.0f
        };
        return constants;
    }
}

namespace LamaPon::Detail
{
    D3D12MaterialShaderRenderer::D3D12MaterialShaderRenderer(
        D3D12Backend& backend)
        : m_backend(&backend)
    {
        auto* const device = backend.Device();
        if (device == nullptr)
        {
            throw std::invalid_argument(
                "The DirectX 12 material shader renderer requires an "
                "initialized backend.");
        }
        m_linearRootSignature = CreateMaterialRootSignature(device, false);
        m_pointRootSignature = CreateMaterialRootSignature(device, true);
        m_skinnedVertexShader = CompileEmbeddedShader(
            SkinnedVertexShaderSource,
            sizeof(SkinnedVertexShaderSource) - 1u,
            "LamaPonD3D12SkinnedMaterial",
            "SkinnedVertexShader",
            "vs_5_0");
        m_skinnedErrorShader.vertexShader = m_skinnedVertexShader;
        m_skinnedErrorShader.pixelShader = CompileEmbeddedShader(
            SkinnedVertexShaderSource,
            sizeof(SkinnedVertexShaderSource) - 1u,
            "LamaPonD3D12SkinnedMaterial",
            "SkinnedErrorPixelShader",
            "ps_5_0");
        // LamaPonShaderError.hlslの宣言（不透明・両面・深度書き込み）です。
        m_skinnedErrorShader.renderState.declared = true;
        m_skinnedErrorShader.renderState.blend = ShaderBlendMode::Opaque;
        m_skinnedErrorShader.renderState.cull = ShaderCullMode::None;
        m_skinnedErrorShader.renderState.depthWrite = true;
        m_skinnedErrorShader.renderState.depthTest = true;
        const auto flatNormal =
            backend.CreateSolidRgba8Texture({ 128u, 128u, 255u, 255u });
        m_flatNormalView = backend.CreateShaderResourceView(flatNormal);
    }

    D3D12MaterialShaderRenderer::~D3D12MaterialShaderRenderer() noexcept =
        default;

    D3D12MaterialShaderRenderer::ShaderEntry&
        D3D12MaterialShaderRenderer::Prepare(
            AssetManager& assets,
            const MaterialShaderSource& source,
            const bool skinned)
    {
        auto& entry =
            (skinned ? m_skinnedShaders : m_shaders)[source.cacheKey];
        const auto now = std::chrono::steady_clock::now();
        if (entry.observed
            && !entry.forceReload
            && now < entry.nextCheck)
        {
            return entry;
        }
        entry.nextCheck = now + std::chrono::milliseconds(250);

        // D3D11と同じく、アーカイブ内のShaderは一度だけ読み込みます。
        const bool archived = assets.IsArchived();
        std::error_code fileError;
        const bool sourceExists = assets.FileExists(source.path);
        const auto writeTime = (sourceExists && !archived)
            ? std::filesystem::last_write_time(source.path, fileError)
            : std::filesystem::file_time_type{};
        const bool changed = !entry.observed
            || entry.forceReload
            || entry.sourceExists != sourceExists
            || (sourceExists
                && !archived
                && entry.writeTime != writeTime);
        if (!changed)
        {
            return entry;
        }
        entry.observed = true;
        entry.forceReload = false;
        entry.sourceExists = sourceExists;
        entry.writeTime = writeTime;
        entry.pipelines.clear();
        if (!sourceExists)
        {
            entry.error =
                "Shader file was not found: " + PathToUtf8(source.path);
            entry.vertexShader.Reset();
            entry.pixelShader.Reset();
            entry.geometryShader.Reset();
            entry.hullShader.Reset();
            entry.domainShader.Reset();
            entry.outlineVertexShader.Reset();
            entry.outlinePixelShader.Reset();
            entry.occludedPixelShader.Reset();
            entry.passError.clear();
            return entry;
        }

        try
        {
            const auto sourceBytes = assets.ReadFileBytes(source.path);
            const auto renderState = ParseShaderRenderState(
                std::string_view{
                    reinterpret_cast<const char*>(sourceBytes.data()),
                    sourceBytes.size() });
            // D3D11のLitEffectと同じく、スキニング用はVSSkinnedMainと
            // PSSkinnedMainを両方compileします。描画に使う頂点シェーダーは
            // DirectXTK SkinnedEffectと同じ内蔵版です。
            auto vertexShader = CompileShaderCached(
                assets,
                source.path,
                skinned ? "VSSkinnedMain" : "VSMain",
                "vs_5_0",
                source.keywords);
            auto pixelShader = CompileShaderCached(
                assets,
                source.path,
                skinned ? "PSSkinnedMain" : "PSMain",
                "ps_5_0",
                source.keywords);
            auto geometryShader = TryCompileShader(
                assets,
                source.path,
                "GSMain",
                "gs_5_0",
                source.keywords);
            if (geometryShader != nullptr
                && !TakesTriangles(geometryShader.Get()))
            {
                throw std::runtime_error(
                    "GSMain must take 'triangle' input."
                    " LamaPon only ever draws triangles, so a"
                    " point/line geometry shader cannot be used.");
            }
            // D3D11のLitEffectと同じく、HSMainとDSMainが両方あるときだけ
            // テセレーションとして使います。
            auto hullShader = TryCompileShader(
                assets,
                source.path,
                "HSMain",
                "hs_5_0",
                source.keywords);
            auto domainShader = TryCompileShader(
                assets,
                source.path,
                "DSMain",
                "ds_5_0",
                source.keywords);
            if (hullShader == nullptr || domainShader == nullptr)
            {
                hullShader.Reset();
                domainShader.Reset();
            }
            const bool hasTessellation = hullShader != nullptr;
            // D3D11のLitEffectと同じく、VSOutlineとPSOutlineは両方あるとき
            // だけ輪郭に使います。D3D12はglTF／FBXの輪郭と遮蔽表示を描かない
            // ため、スキニング用には用意しません。
            Microsoft::WRL::ComPtr<ID3DBlob> outlineVertexShader;
            Microsoft::WRL::ComPtr<ID3DBlob> outlinePixelShader;
            Microsoft::WRL::ComPtr<ID3DBlob> occludedPixelShader;
            if (!skinned)
            {
                outlineVertexShader = TryCompileShader(
                    assets,
                    source.path,
                    "VSOutline",
                    "vs_5_0",
                    source.keywords);
                outlinePixelShader = TryCompileShader(
                    assets,
                    source.path,
                    "PSOutline",
                    "ps_5_0",
                    source.keywords);
                if (outlineVertexShader == nullptr
                    || outlinePixelShader == nullptr)
                {
                    outlineVertexShader.Reset();
                    outlinePixelShader.Reset();
                }
                occludedPixelShader = TryCompileShader(
                    assets,
                    source.path,
                    "PSOccluded",
                    "ps_5_0",
                    source.keywords);
            }
            std::array<bool, ConstantBufferCount> constantBuffers{};
            MarkConstantBuffers(vertexShader.Get(), constantBuffers);
            MarkConstantBuffers(pixelShader.Get(), constantBuffers);
            MarkConstantBuffers(geometryShader.Get(), constantBuffers);
            MarkConstantBuffers(hullShader.Get(), constantBuffers);
            MarkConstantBuffers(domainShader.Get(), constantBuffers);
            MarkConstantBuffers(outlineVertexShader.Get(), constantBuffers);
            MarkConstantBuffers(outlinePixelShader.Get(), constantBuffers);
            MarkConstantBuffers(occludedPixelShader.Get(), constantBuffers);

            entry.vertexShader = std::move(vertexShader);
            entry.pixelShader = std::move(pixelShader);
            entry.geometryShader = std::move(geometryShader);
            entry.hullShader = std::move(hullShader);
            entry.domainShader = std::move(domainShader);
            entry.outlineVertexShader = std::move(outlineVertexShader);
            entry.outlinePixelShader = std::move(outlinePixelShader);
            entry.occludedPixelShader = std::move(occludedPixelShader);
            entry.renderState = renderState;
            entry.constantBuffers = constantBuffers;
            entry.hasTessellation = hasTessellation;
            entry.generation = m_nextGeneration++;
            entry.error.clear();
            entry.passError.clear();
        }
        catch (const std::exception& exception)
        {
            entry.error = source.describeFailure
                ? source.describeFailure(exception.what())
                : std::string(exception.what());
            // D3D11と同じく、再compileに失敗したら直前のShaderは残さず、
            // 壊れていることが見えるように代替表示へ切り替えます。
            entry.vertexShader.Reset();
            entry.pixelShader.Reset();
            entry.geometryShader.Reset();
            entry.hullShader.Reset();
            entry.domainShader.Reset();
            entry.outlineVertexShader.Reset();
            entry.outlinePixelShader.Reset();
            entry.occludedPixelShader.Reset();
            entry.passError.clear();
        }
        return entry;
    }

    ID3D12PipelineState* D3D12MaterialShaderRenderer::PipelineState(
        ShaderEntry& entry,
        const PipelineKey& key)
    {
        auto& pipeline = entry.pipelines[key];
        if (pipeline != nullptr)
        {
            return pipeline.Get();
        }

        // DirectXTKのVertexPositionNormalTangentColorTextureSkinningと同じ
        // 並びと意味名です。
        static const std::array<D3D12_INPUT_ELEMENT_DESC, 7> skinnedInputs{ {
            { "SV_Position", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0u,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12u,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24u,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 40u,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 44u,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 52u,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "BLENDWEIGHT", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 56u,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        } };
        // DirectXTKのVertexPositionNormalTextureと同じ意味名です。
        static const std::array<D3D12_INPUT_ELEMENT_DESC, 3> inputs{ {
            { "SV_Position", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
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
        description.pRootSignature = key.pointSampler
            ? m_pointRootSignature.Get()
            : m_linearRootSignature.Get();
        // 輪郭はVSOutline／PSOutline、遮蔽表示はVSMain／PSOccludedで描きます。
        const auto pass = static_cast<MaterialShaderPass>(key.pass);
        auto* const vertexShader = key.skinned
            ? m_skinnedVertexShader.Get()
            : pass == MaterialShaderPass::Outline
                ? entry.outlineVertexShader.Get()
                : entry.vertexShader.Get();
        auto* const pixelShader = pass == MaterialShaderPass::Outline
            ? entry.outlinePixelShader.Get()
            : pass == MaterialShaderPass::Occluded
                ? entry.occludedPixelShader.Get()
                : entry.pixelShader.Get();
        if (vertexShader == nullptr
            || (!key.depthOnly && pixelShader == nullptr))
        {
            throw std::logic_error(
                "The material shader has no entry point for the requested "
                "pass.");
        }
        description.VS = {
            vertexShader->GetBufferPointer(),
            vertexShader->GetBufferSize()
        };
        if (!key.depthOnly)
        {
            description.PS = {
                pixelShader->GetBufferPointer(),
                pixelShader->GetBufferSize()
            };
        }
        // D3D11のスキニング経路はGSMainを束ねません。LitEffect::ApplyOutlineも、
        // 輪郭用の頂点シェーダーとGSMainの入力が合う保証が無いため外します。
        if (!key.skinned
            && pass != MaterialShaderPass::Outline
            && entry.geometryShader != nullptr)
        {
            // D3D11と同じく、深度パスでもGSMainを通した形で書きます。
            description.GS = {
                entry.geometryShader->GetBufferPointer(),
                entry.geometryShader->GetBufferSize()
            };
        }
        // D3D11のLitEffectがパッチで描くときと同じく、HSMainとDSMainを
        // 束ねます（深度パスも分割後の形で書きます）。
        if (key.tessellated)
        {
            if (entry.hullShader == nullptr || entry.domainShader == nullptr)
            {
                throw std::logic_error(
                    "The tessellated material shader has no hull or domain "
                    "shader.");
            }
            description.HS = {
                entry.hullShader->GetBufferPointer(),
                entry.hullShader->GetBufferSize()
            };
            description.DS = {
                entry.domainShader->GetBufferPointer(),
                entry.domainShader->GetBufferSize()
            };
        }
        description.BlendState =
            MakeBlendDescription(static_cast<BlendKind>(key.blend));
        description.SampleMask = std::numeric_limits<UINT>::max();
        description.RasterizerState = MakeRasterizerDescription(
            static_cast<ShaderCullMode>(key.cull),
            key.shadowBias,
            key.wireframe);
        description.DepthStencilState =
            MakeDepthDescription(static_cast<DepthKind>(key.depth));
        description.InputLayout = key.skinned
            ? D3D12_INPUT_LAYOUT_DESC{
                skinnedInputs.data(),
                static_cast<UINT>(skinnedInputs.size()) }
            : D3D12_INPUT_LAYOUT_DESC{
                inputs.data(),
                static_cast<UINT>(inputs.size()) };
        description.PrimitiveTopologyType = key.tessellated
            ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH
            : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        description.NumRenderTargets = key.depthOnly ? 0u : 1u;
        if (!key.depthOnly)
        {
            description.RTVFormats[0] = key.colorFormat;
        }
        description.DSVFormat = key.depthFormat;
        description.SampleDesc.Count = 1;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> created;
        ThrowIfFailed(
            m_backend->Device()->CreateGraphicsPipelineState(
                &description,
                IID_PPV_ARGS(created.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateGraphicsPipelineState(material shader)");
        pipeline = std::move(created);
        return pipeline.Get();
    }

    MaterialShaderDrawResult D3D12MaterialShaderRenderer::Draw(
        AssetManager& assets,
        const MaterialShaderSource& shader,
        const MaterialShaderSource& placeholder,
        const bool prepass,
        const PrimitiveDrawRequest& request,
        const std::span<const PrimitiveRenderVertex> vertices,
        const std::span<const std::uint32_t> indices,
        const MaterialShaderDrawRequest& material,
        const LightingState& lighting)
    {
        MaterialShaderDrawResult result;
        const auto* const skinned = material.skinned;
        // Mesh RendererはPrimitiveRenderVertex、glTF／FBXは
        // ImportedModelVertexの列を描きます。
        const auto drawVertices = skinned != nullptr
            ? skinned->vertices
            : std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(vertices.data()),
                vertices.size_bytes());
        const auto drawIndices = skinned != nullptr
            ? skinned->indices
            : indices;
        const std::uint32_t vertexStride = skinned != nullptr
            ? skinned->vertexStride
            : static_cast<std::uint32_t>(sizeof(PrimitiveRenderVertex));
        if (material.material == nullptr
            || drawVertices.empty()
            || drawIndices.empty())
        {
            return result;
        }
        if (skinned != nullptr)
        {
            const auto vertexCount = drawVertices.size() / SkinnedVertexStride;
            if (vertexStride != SkinnedVertexStride
                || drawVertices.size() % SkinnedVertexStride != 0u
                || drawIndices.size() % 3u != 0u
                || std::ranges::any_of(
                    drawIndices,
                    [vertexCount](const std::uint32_t index)
                    {
                        return index >= vertexCount;
                    }))
            {
                return result;
            }
        }
        const auto& litMaterial = *material.material;
        const bool depthOnly = request.depthOnly;
        if (depthOnly && !m_backend->IsDepthOnlyPassActive())
        {
            return result;
        }

        const auto usable = [](const ShaderEntry& entry) noexcept
        {
            return entry.vertexShader != nullptr
                && entry.pixelShader != nullptr;
        };
        auto* active = &Prepare(assets, shader, skinned != nullptr);
        result.generation = active->generation;
        result.error = active->error;
        result.passError = active->passError;
        // D3D11のMesh Rendererと同じく、テセレーションはPlaneとCubeの四角
        // パッチでだけ描き、パッチへ分けられない形とModel Rendererは同じ説明で
        // 代替表示へ切り替えます。
        if (usable(*active)
            && active->hasTessellation
            && (skinned != nullptr
                || material.directXTKPart != nullptr
                || material.tessellationPatches.empty()
                || material.tessellationPatches.size() % 4u != 0u))
        {
            result.error = skinned != nullptr
                    || material.directXTKPart != nullptr
                ? "This shader uses tessellation (HSMain/DSMain),"
                    " which only works on a Mesh Renderer whose"
                    " shape can be split into quad patches"
                    " (Plane and Cube)."
                : "This shader uses tessellation (HSMain/DSMain),"
                    " which only works on shapes that can be split"
                    " into quad patches (Plane and Cube).";
        }
        const auto usePlaceholder = [&]()
        {
            if (skinned != nullptr)
            {
                active = &m_skinnedErrorShader;
                result.placeholder = true;
                return true;
            }
            auto& fallback = Prepare(assets, placeholder, false);
            if (!usable(fallback))
            {
                return false;
            }
            active = &fallback;
            result.placeholder = true;
            return true;
        };
        if ((!usable(*active) || !result.error.empty())
            && (result.error.empty() || !usePlaceholder()))
        {
            return result;
        }

        // D3D11のDrawCommonLitと同じく、輪郭と遮蔽表示はShaderがその入口を
        // 持つときだけ、通常のパスでCMO／SDKMESH／VBOのpartへ重ねます。
        // 代替表示のShaderにはどちらもありません。
        if (material.pass != MaterialShaderPass::Main
            && (result.placeholder
                || skinned != nullptr
                || material.directXTKPart == nullptr
                || depthOnly
                || (material.pass == MaterialShaderPass::Outline
                    ? active->outlineVertexShader == nullptr
                    : active->occludedPixelShader == nullptr)))
        {
            return result;
        }

        // D3D11のSkeletalModelは深度プリパスでもこの判定を行いません。
        if (depthOnly
            && prepass
            && skinned == nullptr
            && active->renderState.declared
            && (active->renderState.blend != ShaderBlendMode::Opaque
                || !active->renderState.depthWrite))
        {
            // D3D11と同じく、深度プリパスへはメインパスと同じ深度を書く
            // Shaderだけを出します。
            result.renderState = active->renderState;
            return result;
        }

        const auto makeKey = [&](const ShaderEntry& entry)
        {
            PipelineKey key;
            key.depthOnly = depthOnly;
            // LitEffect::ActiveMaterialSamplerと同じ切り替えです。
            key.pointSampler = litMaterial.CustomParameters()[7].w >= 0.5f;
            key.colorFormat = depthOnly
                ? DXGI_FORMAT_UNKNOWN
                : m_backend->ActiveColorFormat();
            key.depthFormat = m_backend->ActiveDepthFormat();
            key.shadowBias = depthOnly
                && key.depthFormat == D3D12Backend::ShadowDepthFormat;
            // D3D11のMesh Rendererは、GeometricPrimitive::Drawの既定
            // （不透明ならOpaque／DepthDefault、半透明ならAlphaBlend／
            // DepthRead、CullCounterClockwise）の後に、World Overlayか
            // Shaderの宣言を上書きします。深度パスは常に既定です。
            key.skinned = skinned != nullptr;
            // 代替表示のShaderはテセレーションを持たないため、通常の三角形で
            // 描きます。
            key.tessellated = entry.hasTessellation
                && entry.hullShader != nullptr
                && skinned == nullptr
                && !material.tessellationPatches.empty();
            auto blend = BlendKind::Opaque;
            auto depth = DepthKind::Default;
            auto cull = ShaderCullMode::Back;
            bool wireframe{};
            if (skinned != nullptr)
            {
                // D3D11のSkeletalModel::DrawD3D11と同じく、深度パスでも宣言を
                // 適用し、glTF／FBXの表面はCullClockwise側として扱います。
                // ワイヤーフレーム表示はデバッグ用なので、D3D11と同じく宣言
                // より優先し、半透明passの既定とカリングなしの辺で描きます。
                const auto& state = entry.renderState;
                if (state.declared && !request.wireframe)
                {
                    blend = ToBlendKind(state.blend);
                    depth = state.depthTest
                        ? (state.depthWrite
                            ? DepthKind::Default
                            : DepthKind::Read)
                        : DepthKind::None;
                    cull = state.cull == ShaderCullMode::Front
                        ? ShaderCullMode::Back
                        : state.cull == ShaderCullMode::None
                                || skinned->doubleSided
                            ? ShaderCullMode::None
                            : ShaderCullMode::Front;
                }
                else
                {
                    blend = skinned->alphaPass
                        ? BlendKind::NonPremultiplied
                        : BlendKind::Opaque;
                    depth = skinned->alphaPass
                        ? DepthKind::Read
                        : DepthKind::Default;
                    cull = request.wireframe || skinned->doubleSided
                        ? ShaderCullMode::None
                        : ShaderCullMode::Front;
                }
                wireframe = request.wireframe;
            }
            else if (key.tessellated)
            {
                // D3D11のMeshRendererComponentがパッチで描くときと同じです。
                // 深度パスと宣言の無いShaderはカリングせず、宣言の無い半透明は
                // 非プレマルチプライド、World Overlayは深度テストだけを外します。
                const auto& state = entry.renderState;
                cull = ShaderCullMode::None;
                if (!depthOnly && state.declared)
                {
                    blend = ToBlendKind(state.blend);
                    depth = state.depthTest
                        ? (state.depthWrite
                            ? DepthKind::Default
                            : DepthKind::Read)
                        : DepthKind::None;
                    cull = state.cull;
                }
                else if (!depthOnly && litMaterial.BaseColor().w < 1.0f)
                {
                    blend = BlendKind::NonPremultiplied;
                    depth = DepthKind::Read;
                }
                if (!depthOnly && material.worldOverlay)
                {
                    depth = DepthKind::None;
                }
            }
            else if (material.directXTKPart != nullptr)
            {
                // D3D11のModelRendererComponent::DrawCommonLitと同じく、
                // ModelMesh::PrepareForRenderingの既定を置いてからShaderの
                // 宣言で上書きします。深度パスも同じ順です。
                const auto& part = *material.directXTKPart;
                blend = part.alphaPass
                    ? (part.premultipliedAlpha
                        ? BlendKind::Premultiplied
                        : BlendKind::NonPremultiplied)
                    : BlendKind::Opaque;
                depth = part.alphaPass
                    ? DepthKind::Read
                    : DepthKind::Default;
                // ワイヤーフレームはCommonStates::Wireframe（カリングなし）
                // です。Shaderの宣言があると、D3D11のApplyShaderRenderStateと
                // 同じく宣言の塗りつぶしへ戻ります。
                cull = request.wireframe
                    ? ShaderCullMode::None
                    : part.counterClockwise
                        ? ShaderCullMode::Back
                        : ShaderCullMode::Front;
                const auto& state = entry.renderState;
                wireframe = request.wireframe && !state.declared;
                if (state.declared)
                {
                    // ModelRendererComponent::ApplyShaderRenderStateの加算は
                    // DirectXTKのAdditiveです。
                    blend = state.blend == ShaderBlendMode::Additive
                        ? BlendKind::Additive
                        : ToBlendKind(state.blend);
                    depth = state.depthTest
                        ? (state.depthWrite
                            ? DepthKind::Default
                            : DepthKind::Read)
                        : DepthKind::None;
                    cull = state.cull;
                }
            }
            else if (!depthOnly)
            {
                const auto& state = entry.renderState;
                if (material.worldOverlay)
                {
                    blend = BlendKind::NonPremultiplied;
                    depth = DepthKind::None;
                }
                else if (state.declared)
                {
                    blend = ToBlendKind(state.blend);
                    depth = state.depthTest
                        ? (state.depthWrite
                            ? DepthKind::Default
                            : DepthKind::Read)
                        : DepthKind::None;
                    cull = state.cull;
                }
                else
                {
                    const bool translucent =
                        litMaterial.BaseColor().w < 1.0f;
                    blend = translucent
                        ? BlendKind::Premultiplied
                        : BlendKind::Opaque;
                    depth = translucent
                        ? DepthKind::Read
                        : DepthKind::Default;
                }
            }
            key.pass = static_cast<std::uint8_t>(material.pass);
            if (material.pass == MaterialShaderPass::Outline)
            {
                // D3D11のDrawCommonLitが輪郭の描画で置く状態です
                // （NonPremultiplied／DepthRead／CullClockwise）。Shaderの宣言は
                // 通常の描画にだけ効きます。
                blend = BlendKind::NonPremultiplied;
                depth = DepthKind::Read;
                cull = ShaderCullMode::Front;
                wireframe = false;
            }
            else if (material.pass == MaterialShaderPass::Occluded)
            {
                // D3D11のDrawCommonLitのNonPremultiplied／CullCounterClockwiseと、
                // LitEffect::ApplyOccludedの奥だけを通す深度です。
                blend = BlendKind::NonPremultiplied;
                depth = DepthKind::Occluded;
                cull = ShaderCullMode::Back;
                wireframe = false;
            }
            else if (material.cullOverride.has_value())
            {
                cull = *material.cullOverride;
            }
            key.wireframe = wireframe;
            if (key.depthFormat == DXGI_FORMAT_UNKNOWN)
            {
                depth = DepthKind::None;
            }
            key.blend = static_cast<std::uint8_t>(blend);
            key.depth = static_cast<std::uint8_t>(depth);
            key.cull = static_cast<std::uint8_t>(cull);
            return key;
        };

        auto key = makeKey(*active);
        ID3D12PipelineState* pipeline{};
        try
        {
            pipeline = PipelineState(*active, key);
        }
        catch (const std::exception& exception)
        {
            if (result.placeholder)
            {
                throw;
            }
            const auto failure = shader.describeFailure
                ? shader.describeFailure(exception.what())
                : std::string(exception.what());
            if (material.pass != MaterialShaderPass::Main)
            {
                // 輪郭／遮蔽表示だけのpipelineを作れないときは、通常の描画を
                // 残してそのpassを止め、Shaderの説明に出します。
                if (material.pass == MaterialShaderPass::Outline)
                {
                    active->outlineVertexShader.Reset();
                    active->outlinePixelShader.Reset();
                }
                else
                {
                    active->occludedPixelShader.Reset();
                }
                active->passError = failure;
                result.passError = failure;
                return result;
            }
            // 段間の入出力が合わないなど、pipelineを作れないShaderも
            // compile失敗と同じく説明を出して代替表示で描きます。
            active->error = failure;
            active->vertexShader.Reset();
            active->pixelShader.Reset();
            active->geometryShader.Reset();
            active->hullShader.Reset();
            active->domainShader.Reset();
            active->outlineVertexShader.Reset();
            active->outlinePixelShader.Reset();
            active->occludedPixelShader.Reset();
            active->pipelines.clear();
            result.error = active->error;
            if (!usePlaceholder())
            {
                return result;
            }
            key = makeKey(*active);
            pipeline = PipelineState(*active, key);
        }
        result.renderState = active->renderState;

        std::array<D3D12_GPU_DESCRIPTOR_HANDLE, TextureSlotCount> textures{};
        ID3D12DescriptorHeap* descriptorHeap{};
        std::optional<D3D12Backend::ShaderResourceBinding> directionalShadow;
        std::optional<D3D12Backend::ShaderResourceBinding> spotShadow;
        std::optional<D3D12Backend::ShaderResourceBinding> pointShadow;
        std::optional<D3D12Backend::ShaderResourceBinding> occlusion;
        std::optional<D3D12Backend::ShaderResourceBinding> reflectionColor;
        std::optional<D3D12Backend::ShaderResourceBinding> reflectionDepth;
        std::optional<D3D12Backend::ShaderResourceBinding> environment;
        std::optional<D3D12Backend::ShaderResourceBinding> environmentSpecular;
        std::optional<D3D12Backend::ShaderResourceBinding> environmentIrradiance;
        std::optional<D3D12Backend::ShaderResourceBinding> clusterLights;
        std::optional<D3D12Backend::ShaderResourceBinding> clusterIndices;
        std::optional<D3D12Backend::ShaderResourceBinding> clusterCounts;
        std::optional<D3D12Backend::ShaderResourceBinding> bakedGiRed;
        std::optional<D3D12Backend::ShaderResourceBinding> bakedGiGreen;
        std::optional<D3D12Backend::ShaderResourceBinding> bakedGiBlue;
        std::optional<D3D12Backend::ShaderResourceBinding> white;
        LightingViews views;
        if (!depthOnly)
        {
            descriptorHeap = m_backend->ShaderResourceDescriptorHeap();
            white = m_backend->TryResolveShaderResource(
                request.fallbackTexture);
            if (descriptorHeap == nullptr || !white)
            {
                return result;
            }
            const auto resolve = [this](
                const GraphicsViewHandle& view,
                const GraphicsViewHandle& fallback,
                D3D12_GPU_DESCRIPTOR_HANDLE& descriptor)
            {
                const auto binding = m_backend->TryResolveShaderResource(
                    view ? view : fallback);
                if (!binding)
                {
                    return false;
                }
                descriptor = binding->descriptor;
                return true;
            };
            // D3D11のTrySetLitEffectTexturesと同じく、指定したtextureを解決
            // できない描画は行いません。未指定の枠は白とフラット法線です。
            bool valid = resolve(request.albedo, request.fallbackTexture, textures[0])
                && resolve(request.normalTexture, m_flatNormalView, textures[1])
                && resolve(request.roughnessTexture, request.fallbackTexture, textures[11])
                && resolve(request.metallicTexture, request.fallbackTexture, textures[12])
                && resolve(request.occlusionTexture, request.fallbackTexture, textures[13])
                && resolve(request.emissiveTexture, request.fallbackTexture, textures[14]);
            for (std::size_t index{};
                valid && index < material.customTextures.size();
                ++index)
            {
                valid = resolve(
                    material.customTextures[index],
                    request.fallbackTexture,
                    textures[LitMaterial::CustomTextureFirstSlot + index]);
            }
            if (!valid)
            {
                return result;
            }
            directionalShadow = m_backend->TryResolveShaderResource(
                request.directionalShadow.texture);
            spotShadow = m_backend->TryResolveShaderResource(
                request.spotShadowTexture);
            pointShadow = m_backend->TryResolveShaderResource(
                request.pointShadow.texture);
            occlusion = m_backend->TryResolveShaderResource(
                request.screenAmbientOcclusion.texture);
            reflectionColor = m_backend->TryResolveShaderResource(
                request.screenSpaceReflection.texture);
            reflectionDepth = m_backend->TryResolveShaderResource(
                request.screenSpaceReflection.depth);
            views.directionalShadow = directionalShadow.has_value();
            views.spotShadow = spotShadow.has_value();
            views.pointShadow = pointShadow.has_value();
            views.screenAmbientOcclusion = occlusion.has_value();
            views.screenReflection =
                reflectionColor.has_value() && reflectionDepth.has_value();
            environment = m_backend->TryResolveShaderResource(
                lighting.environment.texture);
            environmentSpecular = m_backend->TryResolveShaderResource(
                lighting.environment.specular);
            environmentIrradiance = m_backend->TryResolveShaderResource(
                lighting.environment.irradiance);
            const auto isCube = [](
                const std::optional<D3D12Backend::ShaderResourceBinding>&
                    binding)
            {
                return binding.has_value()
                    && binding->dimension == D3D12_SRV_DIMENSION_TEXTURECUBE;
            };
            views.environment = isCube(environment);
            views.prefilteredEnvironment =
                isCube(environmentSpecular) && isCube(environmentIrradiance);
            clusterLights = m_backend->TryResolveShaderResource(
                lighting.clustered.lights);
            clusterIndices = m_backend->TryResolveShaderResource(
                lighting.clustered.lightIndices);
            clusterCounts = m_backend->TryResolveShaderResource(
                lighting.clustered.clusterCounts);
            const auto isBuffer = [](
                const std::optional<D3D12Backend::ShaderResourceBinding>&
                    binding)
            {
                return binding.has_value()
                    && binding->dimension == D3D12_SRV_DIMENSION_BUFFER;
            };
            views.clustered = isBuffer(clusterLights)
                && isBuffer(clusterIndices)
                && isBuffer(clusterCounts);
            bakedGiRed = m_backend->TryResolveShaderResource(
                lighting.bakedGlobalIllumination.redCoefficients);
            bakedGiGreen = m_backend->TryResolveShaderResource(
                lighting.bakedGlobalIllumination.greenCoefficients);
            bakedGiBlue = m_backend->TryResolveShaderResource(
                lighting.bakedGlobalIllumination.blueCoefficients);
            const auto isVolume = [](
                const std::optional<D3D12Backend::ShaderResourceBinding>&
                    binding)
            {
                return binding.has_value()
                    && binding->dimension == D3D12_SRV_DIMENSION_TEXTURE3D;
            };
            views.bakedGlobalIllumination = isVolume(bakedGiRed)
                && isVolume(bakedGiGreen)
                && isVolume(bakedGiBlue);
        }
        LightingViews activeLighting;
        auto lightingConstants =
            BuildLightingConstants(lighting, views, activeLighting);
        if (!depthOnly)
        {
            // D3D11がnullptrをbindする枠は、同じ次元のnull SRVにします。
            const auto null2D = m_backend->NullShaderResourceDescriptor(
                D3D12_SRV_DIMENSION_TEXTURE2D);
            const auto nullArray = m_backend->NullShaderResourceDescriptor(
                D3D12_SRV_DIMENSION_TEXTURE2DARRAY);
            const auto nullCube = m_backend->NullShaderResourceDescriptor(
                D3D12_SRV_DIMENSION_TEXTURECUBE);
            const auto null3D = m_backend->NullShaderResourceDescriptor(
                D3D12_SRV_DIMENSION_TEXTURE3D);
            const auto nullBuffer = m_backend->NullShaderResourceDescriptor(
                D3D12_SRV_DIMENSION_BUFFER);
            textures[2] = activeLighting.directionalShadow
                ? directionalShadow->descriptor
                : nullArray;
            // LitEffectと同じく、t3は事前畳み込み済みスペキュラ（無ければ
            // cubemap本体）、t6は放射照度です。
            textures[3] = activeLighting.environment
                ? (activeLighting.prefilteredEnvironment
                    ? environmentSpecular->descriptor
                    : environment->descriptor)
                : nullCube;
            textures[4] = activeLighting.spotShadow
                ? spotShadow->descriptor
                : nullArray;
            textures[5] = activeLighting.pointShadow
                ? pointShadow->descriptor
                : nullCube;
            textures[6] = activeLighting.prefilteredEnvironment
                ? environmentIrradiance->descriptor
                : nullCube;
            // t15のSSAOだけは、D3D11でも未設定を白（遮蔽なし）にします。
            textures[15] = activeLighting.screenAmbientOcclusion
                ? occlusion->descriptor
                : white->descriptor;
            textures[16] = activeLighting.clustered
                ? clusterLights->descriptor
                : nullBuffer;
            textures[17] = activeLighting.clustered
                ? clusterIndices->descriptor
                : nullBuffer;
            textures[18] = activeLighting.clustered
                ? clusterCounts->descriptor
                : nullBuffer;
            // D3D11のTrySetLitEffectReflectionProbeと同じく、範囲に入った
            // プローブを解決できたときはSkyのIBLを差し替え、混ぜる2個目を
            // t19／t20へ置きます。
            const auto probe = ResolveD3D12ReflectionProbe(
                *m_backend,
                request.reflectionProbe);
            if (probe.active)
            {
                textures[3] = probe.specular;
                textures[6] = probe.irradiance;
                lightingConstants.environmentParameters =
                    probe.environmentParameters;
                lightingConstants.reflectionBoxCenter = probe.boxCenter;
                lightingConstants.reflectionBoxParameters =
                    probe.boxParameters;
                lightingConstants.reflectionSecondaryBoxCenter =
                    probe.secondaryBoxCenter;
                lightingConstants.reflectionSecondaryBoxParameters =
                    probe.secondaryBoxParameters;
                lightingConstants.reflectionBlendParameters =
                    probe.blendParameters;
            }
            textures[19] = probe.blended
                ? probe.secondarySpecular
                : nullCube;
            textures[20] = probe.blended
                ? probe.secondaryIrradiance
                : nullCube;
            textures[21] = activeLighting.screenReflection
                ? reflectionColor->descriptor
                : null2D;
            textures[22] = activeLighting.screenReflection
                ? reflectionDepth->descriptor
                : null2D;
            // LitEffectと同じく、t23〜t25はベイクした間接光のRGB別L1係数です。
            textures[23] = activeLighting.bakedGlobalIllumination
                ? bakedGiRed->descriptor
                : null3D;
            textures[24] = activeLighting.bakedGlobalIllumination
                ? bakedGiGreen->descriptor
                : null3D;
            textures[25] = activeLighting.bakedGlobalIllumination
                ? bakedGiBlue->descriptor
                : null3D;
        }

        // LitEffect::SetMatrices／SetMaterial／ResolveTextureFlagsと同じ値です。
        ObjectConstants object;
        object.world = request.world;
        const auto world = DirectX::XMLoadFloat4x4(&request.world);
        const auto view = DirectX::XMLoadFloat4x4(&request.view);
        const auto projection = DirectX::XMLoadFloat4x4(&request.projection);
        DirectX::XMStoreFloat4x4(&object.viewProjection, view * projection);
        DirectX::XMVECTOR determinant{};
        DirectX::XMStoreFloat4x4(
            &object.worldInverseTranspose,
            DirectX::XMMatrixTranspose(
                DirectX::XMMatrixInverse(&determinant, world)));
        const auto inverseView = DirectX::XMMatrixInverse(&determinant, view);
        DirectX::XMStoreFloat4(&object.cameraPosition, inverseView.r[3]);
        DirectX::XMStoreFloat4(
            &object.cameraForward,
            DirectX::XMVector3Normalize(
                DirectX::XMVectorNegate(inverseView.r[2])));
        constexpr double TimeWrapSeconds = 3600.0;
        object.timeParameters = {
            static_cast<float>(
                std::fmod(Time::TimeSinceStartup(), TimeWrapSeconds)),
            Time::DeltaTime(),
            static_cast<float>(Time::FrameCount() & 0xFFFFFFull),
            0.0f
        };
        object.materialColor = litMaterial.BaseColor();
        object.materialParameters = {
            litMaterial.Roughness(),
            litMaterial.NormalStrength(),
            request.normalTexture ? 1.0f : 0.0f,
            litMaterial.Metallic()
        };
        object.customParameters = litMaterial.CustomParameters();
        object.materialTextureParameters = {
            request.roughnessTexture ? 1.0f : 0.0f,
            request.metallicTexture ? 1.0f : 0.0f,
            request.occlusionTexture ? 1.0f : 0.0f,
            request.occlusionStrength
        };
        object.emissiveParameters = {
            request.emissiveFactor.x,
            request.emissiveFactor.y,
            request.emissiveFactor.z,
            request.emissiveTexture ? 1.0f : 0.0f
        };

        // パッチで描くときは、D3D11のDrawTessellatedPatchと同じ制御点だけを
        // 索引なしで流します。
        const std::span<const std::uint8_t> patchVertices(
            reinterpret_cast<const std::uint8_t*>(
                material.tessellationPatches.data()),
            material.tessellationPatches.size_bytes());
        const auto uploadVertices = key.tessellated
            ? patchVertices
            : drawVertices;
        const auto vertexBytes =
            static_cast<std::uint64_t>(uploadVertices.size());
        const auto indexBytes = key.tessellated
            ? std::uint64_t{}
            : static_cast<std::uint64_t>(drawIndices.size_bytes());
        if (vertexBytes > std::numeric_limits<UINT>::max()
            || indexBytes > std::numeric_limits<UINT>::max())
        {
            throw std::length_error(
                "The DirectX 12 material shader primitive is too large.");
        }
        auto* const commandList = depthOnly
            ? m_backend->CurrentFrameCommands()
            : m_backend->BeginFrameCommands();
        const auto vertexUpload = m_backend->AllocateFrameUpload(
            vertexBytes,
            alignof(float));
        std::memcpy(
            vertexUpload.data,
            uploadVertices.data(),
            uploadVertices.size());
        D3D12Backend::FrameUploadAllocation indexUpload{};
        if (!key.tessellated)
        {
            indexUpload = m_backend->AllocateFrameUpload(
                indexBytes,
                alignof(std::uint32_t));
            std::memcpy(
                indexUpload.data,
                drawIndices.data(),
                drawIndices.size_bytes());
        }
        const auto uploadConstants = [this](const auto& value)
        {
            const auto allocation = m_backend->AllocateFrameUpload(
                sizeof(value),
                D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            std::memcpy(allocation.data, &value, sizeof(value));
            return allocation.gpuAddress;
        };
        std::array<D3D12_GPU_VIRTUAL_ADDRESS, ConstantBufferCount>
            constantBuffers{};
        constantBuffers[0] = uploadConstants(object);
        if (active->constantBuffers[1])
        {
            constantBuffers[1] = uploadConstants(lightingConstants);
        }
        if (active->constantBuffers[2])
        {
            // Mesh Rendererは骨を持たないため、単位行列を渡します。
            BoneConstants bones;
            DirectX::XMFLOAT3X4 identity{};
            DirectX::XMStoreFloat3x4(&identity, DirectX::XMMatrixIdentity());
            bones.transforms.fill(identity);
            constantBuffers[2] = uploadConstants(bones);
        }
        if (active->constantBuffers[3])
        {
            CustomVectorConstants vectors;
            vectors.vectors = litMaterial.CustomVectors();
            constantBuffers[3] = uploadConstants(vectors);
        }
        D3D12_GPU_VIRTUAL_ADDRESS skinningBuffer{};
        if (skinned != nullptr)
        {
            // DirectXTK SkinnedEffectのSetMatrices／SetAlpha／
            // SetBoneTransformsと同じ値です。霧は使いません。
            SkinningConstants skinning;
            skinning.world = request.world;
            DirectX::XMStoreFloat4x4(
                &skinning.worldInverseTranspose,
                DirectX::XMMatrixTranspose(
                    DirectX::XMMatrixInverse(&determinant, world)));
            DirectX::XMStoreFloat4x4(
                &skinning.worldViewProjection,
                DirectX::XMMatrixMultiply(
                    DirectX::XMMatrixMultiply(world, view),
                    projection));
            skinning.diffuseColor = {
                1.0f,
                1.0f,
                1.0f,
                litMaterial.BaseColor().w
            };
            DirectX::XMFLOAT3X4 identity{};
            DirectX::XMStoreFloat3x4(&identity, DirectX::XMMatrixIdentity());
            skinning.bones.fill(identity);
            const auto boneCount =
                std::min(skinned->bones.size(), skinning.bones.size());
            for (std::size_t bone{}; bone < boneCount; ++bone)
            {
                DirectX::XMStoreFloat3x4(
                    &skinning.bones[bone],
                    DirectX::XMLoadFloat4x4(&skinned->bones[bone]));
            }
            skinningBuffer = uploadConstants(skinning);
        }

        commandList->SetGraphicsRootSignature(
            key.pointSampler
                ? m_pointRootSignature.Get()
                : m_linearRootSignature.Get());
        commandList->SetPipelineState(pipeline);
        for (UINT index{}; index < ConstantBufferCount; ++index)
        {
            if (constantBuffers[index] != 0u)
            {
                commandList->SetGraphicsRootConstantBufferView(
                    index,
                    constantBuffers[index]);
            }
        }
        if (skinningBuffer != 0u)
        {
            commandList->SetGraphicsRootConstantBufferView(
                SkinningParameter,
                skinningBuffer);
        }
        if (!depthOnly)
        {
            ID3D12DescriptorHeap* heaps[]{ descriptorHeap };
            commandList->SetDescriptorHeaps(1, heaps);
            for (UINT index{}; index < TextureSlotCount; ++index)
            {
                commandList->SetGraphicsRootDescriptorTable(
                    static_cast<UINT>(ConstantBufferCount) + index,
                    textures[index]);
            }
        }
        const D3D12_VERTEX_BUFFER_VIEW vertexView{
            vertexUpload.gpuAddress,
            static_cast<UINT>(vertexBytes),
            vertexStride
        };
        commandList->IASetVertexBuffers(0, 1, &vertexView);
        if (key.tessellated)
        {
            commandList->IASetPrimitiveTopology(
                D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
        }
        else
        {
            const D3D12_INDEX_BUFFER_VIEW indexView{
                indexUpload.gpuAddress,
                static_cast<UINT>(indexBytes),
                DXGI_FORMAT_R32_UINT
            };
            commandList->IASetPrimitiveTopology(
                D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commandList->IASetIndexBuffer(&indexView);
        }
        if (!depthOnly)
        {
            const auto& viewport = m_backend->ActiveViewport();
            const auto& scissor = m_backend->ActiveScissorRectangle();
            commandList->RSSetViewports(1, &viewport);
            commandList->RSSetScissorRects(1, &scissor);
        }
        if (key.tessellated)
        {
            commandList->DrawInstanced(
                static_cast<UINT>(material.tessellationPatches.size()),
                1,
                0,
                0);
        }
        else
        {
            commandList->DrawIndexedInstanced(
                static_cast<UINT>(drawIndices.size()),
                1,
                0,
                0,
                0);
        }
        result.drawn = true;
        return result;
    }

    void D3D12MaterialShaderRenderer::Invalidate(
        const std::filesystem::path& shaderPath) noexcept
    {
        try
        {
            // D3D11と同じく、そのHLSLから作ったkeyword variantを全部
            // 立て直します（キーは「パス?キーワード」）。
            const auto prefix = shaderPath.wstring();
            for (auto* const shaders : { &m_shaders, &m_skinnedShaders })
            {
                for (auto& [key, entry] : *shaders)
                {
                    const auto text = key.wstring();
                    if (text == prefix
                        || (text.size() > prefix.size()
                            && text.rfind(prefix, 0) == 0
                            && text[prefix.size()] == L'?'))
                    {
                        entry.forceReload = true;
                    }
                }
            }
        }
        catch (...)
        {
        }
    }

    MaterialShaderPasses D3D12MaterialShaderRenderer::PreparePasses(
        AssetManager& assets,
        const MaterialShaderSource& shader)
    {
        const auto& entry = Prepare(assets, shader, false);
        MaterialShaderPasses passes;
        // テセレーションのShaderはCMO／SDKMESH／VBOで代替表示になるため、
        // どちらのpassも描きません。
        if (entry.vertexShader == nullptr
            || entry.pixelShader == nullptr
            || entry.hasTessellation)
        {
            return passes;
        }
        passes.outline = entry.outlineVertexShader != nullptr;
        passes.occluded = entry.occludedPixelShader != nullptr;
        return passes;
    }

    bool D3D12MaterialShaderRenderer::TryGetRenderState(
        const std::filesystem::path& cacheKey,
        ShaderRenderState& state) const noexcept
    {
        try
        {
            const auto found = m_shaders.find(cacheKey);
            if (found == m_shaders.end()
                || found->second.vertexShader == nullptr
                || found->second.pixelShader == nullptr)
            {
                return false;
            }
            state = found->second.renderState;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }
}
