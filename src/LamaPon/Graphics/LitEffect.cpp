#include "LamaPon/Graphics/LitEffect.h"
#include "LamaPon/Graphics/ShaderCompiler.h"
#include "LamaPon/Graphics/ShaderManifest.h"
#include "LamaPon/Graphics/ShaderProgram.h"

#include "LamaPon/Graphics/ClusteredLights.h"
#include "LamaPon/Graphics/ShaderRenderState.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/Time.h"

#include <d3d11shader.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    [[nodiscard]] std::string FoldAsciiLower(
        const std::string_view value)
    {
        std::string folded;
        folded.reserve(value.size());
        for (const char character : value)
        {
            if (character >= 'A' && character <= 'Z')
            {
                folded.push_back(static_cast<char>(
                    character + ('a' - 'A')));
            }
            else
            {
                folded.push_back(character);
            }
        }
        return folded;
    }

    [[nodiscard]] const char* ManifestRoleName(
        const LamaPon::ShaderPassRole role) noexcept
    {
        switch (role)
        {
        case LamaPon::ShaderPassRole::Forward:
            return "forward";
        case LamaPon::ShaderPassRole::Skinned:
            return "skinned";
        case LamaPon::ShaderPassRole::Instanced:
            return "instanced";
        case LamaPon::ShaderPassRole::Outline:
            return "outline";
        case LamaPon::ShaderPassRole::SkinnedOutline:
            return "skinnedOutline";
        case LamaPon::ShaderPassRole::Occluded:
            return "occluded";
        }
        return "unknown";
    }

    [[nodiscard]] LamaPon::ShaderRenderState
        ConvertManifestRenderState(
            const LamaPon::RenderStateDesc& description)
    {
        LamaPon::ShaderRenderState state;
        state.declared = true;

        const auto blend = FoldAsciiLower(description.blend);
        if (blend == "opaque")
        {
            state.blend = LamaPon::ShaderBlendMode::Opaque;
        }
        else if (blend == "alpha")
        {
            state.blend = LamaPon::ShaderBlendMode::Alpha;
        }
        else if (blend == "additive")
        {
            state.blend = LamaPon::ShaderBlendMode::Additive;
        }
        else if (blend == "premultiplied")
        {
            state.blend = LamaPon::ShaderBlendMode::Premultiplied;
        }
        else
        {
            throw std::invalid_argument(
                "renderState.blend must be Opaque, Alpha, Additive, or "
                "Premultiplied (received '" + description.blend + "').");
        }

        const auto cull = FoldAsciiLower(description.cull);
        if (cull == "back")
        {
            state.cull = LamaPon::ShaderCullMode::Back;
        }
        else if (cull == "front")
        {
            state.cull = LamaPon::ShaderCullMode::Front;
        }
        else if (cull == "off" || cull == "none")
        {
            state.cull = LamaPon::ShaderCullMode::None;
        }
        else
        {
            throw std::invalid_argument(
                "renderState.cull must be Back, Front, Off, or None "
                "(received '" + description.cull + "').");
        }

        const auto zTest = FoldAsciiLower(description.zTest);
        if (zTest == "lessequal")
        {
            state.depthTest = true;
        }
        else if (zTest == "always")
        {
            state.depthTest = false;
        }
        else
        {
            throw std::invalid_argument(
                "renderState.zTest must be LessEqual or Always "
                "(received '" + description.zTest + "').");
        }
        state.depthWrite = description.zWrite;
        if (!state.depthTest && state.depthWrite)
        {
            throw std::invalid_argument(
                "renderState zTest=Always requires zWrite=false; the "
                "current LitEffect state model cannot safely apply "
                "Always with depth writes enabled.");
        }
        return state;
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

    Microsoft::WRL::ComPtr<ID3DBlob> CompileShader(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& path,
        const char* entryPoint,
        const char* target,
        const std::vector<std::string>& keywords)
    {
        // ShaderCompilerを通してコンパイル結果をディスクへキャッシュします。
        return LamaPon::CompileShaderCached(
            assets,
            path,
            entryPoint,
            target,
            keywords);
    }

    Microsoft::WRL::ComPtr<ID3DBlob> TryCompileShader(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& path,
        const char* entryPoint,
        const char* target,
        const std::vector<std::string>& keywords) noexcept
    {
        try
        {
            return CompileShader(
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

    template<typename T>
    Microsoft::WRL::ComPtr<ID3D11Buffer> CreateConstantBuffer(
        ID3D11Device* device)
    {
        static_assert(sizeof(T) % 16 == 0);

        D3D11_BUFFER_DESC description{};
        description.ByteWidth = static_cast<UINT>(sizeof(T));
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
        ThrowIfFailed(
            device->CreateBuffer(
                &description,
                nullptr,
                buffer.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateBuffer");
        return buffer;
    }
}

namespace LamaPon
{
    LitEffect::LitEffect(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        AssetManager& assets,
        const std::filesystem::path& shaderPath,
        const bool skinned,
        const std::vector<std::string>& keywords)
        : m_context(context)
        , m_skinned(skinned)
    {
        if (device == nullptr || context == nullptr)
        {
            throw std::invalid_argument(
                "LitEffect requires a Direct3D device and context.");
        }

        const bool manifestShader = IsShaderManifestPath(shaderPath);
        if (manifestShader)
        {
            ShaderAssetDesc shaderAsset;
            std::string manifestError;
            if (!LoadShaderAssetDesc(
                    assets,
                    shaderPath,
                    shaderAsset,
                    manifestError))
            {
                throw std::runtime_error(manifestError);
            }
            if (shaderAsset.type != ShaderAssetType::Material)
            {
                throw std::invalid_argument(
                    "Shader manifest '" + PathToUtf8(shaderPath)
                    + "' cannot be used by LitEffect because its type "
                    "is not 'material'.");
            }

            const auto primaryRole = skinned
                ? ShaderPassRole::Skinned
                : ShaderPassRole::Forward;
            const auto hasPrimaryRole = std::ranges::any_of(
                shaderAsset.passes,
                [primaryRole](const ShaderPassDesc& pass)
                {
                    return pass.role == primaryRole;
                });
            if (!hasPrimaryRole)
            {
                throw std::invalid_argument(
                    "Material shader manifest '"
                    + PathToUtf8(shaderPath)
                    + "' cannot be used by this "
                    + (skinned ? "skinned" : "static")
                    + " LitEffect because it has no pass with role '"
                    + ManifestRoleName(primaryRole) + "'.");
            }

            m_manifestEffect = true;
            for (const auto& pass : shaderAsset.passes)
            {
                const bool relevant = skinned
                    ? pass.role == ShaderPassRole::Skinned
                        || pass.role
                            == ShaderPassRole::SkinnedOutline
                        || pass.role == ShaderPassRole::Occluded
                    : pass.role == ShaderPassRole::Forward
                        || pass.role == ShaderPassRole::Instanced
                        || pass.role == ShaderPassRole::Outline
                        || pass.role == ShaderPassRole::Occluded;
                if (!relevant)
                {
                    continue;
                }

                ShaderRenderState renderState;
                try
                {
                    renderState = ConvertManifestRenderState(
                        pass.renderState);
                }
                catch (const std::exception& exception)
                {
                    throw std::invalid_argument(
                        "Material shader manifest '"
                        + PathToUtf8(shaderPath) + "', pass '"
                        + (pass.name.empty()
                            ? std::string{ "<unnamed>" }
                            : pass.name)
                        + "' (role "
                        + ManifestRoleName(pass.role) + "): "
                        + exception.what());
                }

                ShaderProgram program;
                std::string compileError;
                if (!program.Compile(
                        device,
                        assets,
                        shaderAsset.source,
                        pass,
                        compileError,
                        keywords))
                {
                    throw std::runtime_error(
                        "Material shader manifest '"
                        + PathToUtf8(shaderPath) + "', pass '"
                        + (pass.name.empty()
                            ? std::string{ "<unnamed>" }
                            : pass.name)
                        + "' (role "
                        + ManifestRoleName(pass.role) + "): "
                        + compileError);
                }
                m_manifestPasses[RoleIndex(pass.role)].push_back({
                    pass.name,
                    std::move(program),
                    renderState
                });
            }
            m_renderState = SelectedPassRenderState(primaryRole);
        }
        else
        {
            // 実行時にも必要なため、Shaderが宣言した描画状態をLitEffectで解釈します。
            // sourceを除いた配布archiveでは、export時に保存したmetadata
            // から同じ描画状態を復元します。loose assetの欠落は従来どおり
            // ReadFileBytesで明示的に失敗します。
            if (!assets.IsArchived() || assets.FileExists(shaderPath))
            {
                const auto sourceBytes =
                    assets.ReadFileBytesFresh(shaderPath);
                m_renderState = ParseShaderRenderState(
                    std::string_view{
                        reinterpret_cast<const char*>(
                            sourceBytes.data()),
                        sourceBytes.size()
                    });
            }
            else
            {
                // 新しいexport cacheならsource解析済みmetadataから
                // 宣言を復元します。旧cacheにmetadataが無い場合だけ
                // 後方互換の既定状態を使います。
                static_cast<void>(LoadPrecompiledShaderMetadata(
                    assets,
                    shaderPath,
                    &m_renderState,
                    nullptr));
            }

            m_vertexShaderByteCode = CompileShader(
                assets,
                shaderPath,
                skinned ? "VSSkinnedMain" : "VSMain",
                "vs_5_0",
                keywords);
            const auto pixelShaderByteCode = CompileShader(
                assets,
                shaderPath,
                skinned ? "PSSkinnedMain" : "PSMain",
                "ps_5_0",
                keywords);

            ThrowIfFailed(
                device->CreateVertexShader(
                    m_vertexShaderByteCode->GetBufferPointer(),
                    m_vertexShaderByteCode->GetBufferSize(),
                    nullptr,
                    m_vertexShader.ReleaseAndGetAddressOf()),
                "ID3D11Device::CreateVertexShader");
            ThrowIfFailed(
                device->CreatePixelShader(
                    pixelShaderByteCode->GetBufferPointer(),
                    pixelShaderByteCode->GetBufferSize(),
                    nullptr,
                    m_pixelShader.ReleaseAndGetAddressOf()),
                "ID3D11Device::CreatePixelShader");
        }

        if (!manifestShader && !skinned)
        {
            // インスタンス描画用VS（定義があるシェーダーのみ）。
            m_instancedVertexShaderByteCode =
                TryCompileShader(
                    assets,
                    shaderPath,
                    "VSInstancedMain",
                    "vs_5_0",
            keywords);
            if (m_instancedVertexShaderByteCode)
            {
                ThrowIfFailed(
                    device->CreateVertexShader(
                        m_instancedVertexShaderByteCode
                            ->GetBufferPointer(),
                        m_instancedVertexShaderByteCode
                            ->GetBufferSize(),
                        nullptr,
                        m_instancedVertexShader
                            .ReleaseAndGetAddressOf()),
                    "ID3D11Device::CreateVertexShader(instanced)");
            }
        }

        Microsoft::WRL::ComPtr<ID3DBlob>
            hullShaderByteCode;
        Microsoft::WRL::ComPtr<ID3DBlob>
            domainShaderByteCode;
        if (!manifestShader)
        {
            hullShaderByteCode = TryCompileShader(
                assets,
                shaderPath,
                "HSMain",
                "hs_5_0",
                keywords);
            domainShaderByteCode = TryCompileShader(
                assets,
                shaderPath,
                "DSMain",
                "ds_5_0",
                keywords);
        }
        // ジオメトリシェーダー（定義があるシェーダーのみ）。
        //
        // 入力プリミティブを必ず確かめます。エンジンが流すのは
        // 三角形（通常の描画も、テセレーションのドメイン出力も）だけ
        // なので、point/line を宣言したGSを束ねるとD3D11では不正な
        // 描画になります。WARPではプロセス終了につながるため、
        // ドライバーへ渡す前に拒否します。
        if (!manifestShader)
        {
            if (const auto geometryShaderByteCode =
                    TryCompileShader(
                        assets,
                        shaderPath,
                        "GSMain",
                        "gs_5_0",
                        keywords))
            {
                Microsoft::WRL::ComPtr<ID3D11ShaderReflection>
                    reflection;
                D3D11_SHADER_DESC shaderDescription{};
                if (SUCCEEDED(D3DReflect(
                        geometryShaderByteCode->GetBufferPointer(),
                        geometryShaderByteCode->GetBufferSize(),
                        IID_ID3D11ShaderReflection,
                        &reflection))
                    && SUCCEEDED(
                        reflection->GetDesc(&shaderDescription))
                    && shaderDescription.InputPrimitive
                        != D3D_PRIMITIVE_TRIANGLE)
                {
                    throw std::runtime_error(
                        "GSMain must take 'triangle' input."
                        " LamaPon only ever draws triangles, so a"
                        " point/line geometry shader cannot be used.");
                }
                ThrowIfFailed(
                    device->CreateGeometryShader(
                        geometryShaderByteCode->GetBufferPointer(),
                        geometryShaderByteCode->GetBufferSize(),
                        nullptr,
                        m_geometryShader.ReleaseAndGetAddressOf()),
                    "ID3D11Device::CreateGeometryShader");
            }
        }

        if (!manifestShader
            && hullShaderByteCode
            && domainShaderByteCode)
        {
            ThrowIfFailed(
                device->CreateHullShader(
                    hullShaderByteCode->GetBufferPointer(),
                    hullShaderByteCode->GetBufferSize(),
                    nullptr,
                    m_hullShader.ReleaseAndGetAddressOf()),
                "ID3D11Device::CreateHullShader");
            ThrowIfFailed(
                device->CreateDomainShader(
                    domainShaderByteCode->GetBufferPointer(),
                    domainShaderByteCode->GetBufferSize(),
                    nullptr,
                    m_domainShader.ReleaseAndGetAddressOf()),
                "ID3D11Device::CreateDomainShader");
        }

        if (!manifestShader)
        {
            const char* outlineVertexEntry =
                skinned
                    ? "VSSkinnedOutline"
                    : "VSOutline";
            const auto outlineVertexByteCode =
                TryCompileShader(
                    assets,
                    shaderPath,
                    outlineVertexEntry,
                    "vs_5_0",
            keywords);
            const auto outlinePixelByteCode =
                TryCompileShader(
                    assets,
                    shaderPath,
                    "PSOutline",
                    "ps_5_0",
            keywords);
            if (outlineVertexByteCode && outlinePixelByteCode)
            {
                ThrowIfFailed(
                    device->CreateVertexShader(
                        outlineVertexByteCode->GetBufferPointer(),
                        outlineVertexByteCode->GetBufferSize(),
                        nullptr,
                        m_outlineVertexShader.
                            ReleaseAndGetAddressOf()),
                    "ID3D11Device::CreateVertexShader(outline)");
                ThrowIfFailed(
                    device->CreatePixelShader(
                        outlinePixelByteCode->GetBufferPointer(),
                        outlinePixelByteCode->GetBufferSize(),
                        nullptr,
                        m_outlinePixelShader.
                            ReleaseAndGetAddressOf()),
                    "ID3D11Device::CreatePixelShader(outline)");
            }
        }
        Microsoft::WRL::ComPtr<ID3DBlob>
            occludedPixelByteCode;
        if (!manifestShader)
        {
            occludedPixelByteCode = TryCompileShader(
                assets,
                shaderPath,
                "PSOccluded",
                "ps_5_0",
                keywords);
        }
        if (occludedPixelByteCode)
        {
            ThrowIfFailed(
                device->CreatePixelShader(
                    occludedPixelByteCode->GetBufferPointer(),
                    occludedPixelByteCode->GetBufferSize(),
                    nullptr,
                    m_occludedPixelShader.
                        ReleaseAndGetAddressOf()),
                "ID3D11Device::CreatePixelShader(occluded)");
        }

        if (occludedPixelByteCode
            || (m_manifestEffect
                && PassCount(ShaderPassRole::Occluded) != 0))
        {
            D3D11_DEPTH_STENCIL_DESC depthDescription{};
            depthDescription.DepthEnable = TRUE;
            depthDescription.DepthWriteMask =
                D3D11_DEPTH_WRITE_MASK_ZERO;
            depthDescription.DepthFunc =
                D3D11_COMPARISON_GREATER;
            ThrowIfFailed(
                device->CreateDepthStencilState(
                    &depthDescription,
                    m_occludedDepthState.
                        ReleaseAndGetAddressOf()),
                "ID3D11Device::CreateDepthStencilState(occluded)");
        }

        m_objectBuffer =
            CreateConstantBuffer<ObjectConstants>(device);
        m_lightingBuffer =
            CreateConstantBuffer<LightingConstants>(device);
        m_customVectorBuffer =
            CreateConstantBuffer<CustomVectorConstants>(device);
        if (skinned)
        {
            m_boneBuffer =
                CreateConstantBuffer<BoneConstants>(device);
            DirectX::XMFLOAT3X4 identity{};
            DirectX::XMStoreFloat3x4(
                &identity,
                DirectX::XMMatrixIdentity());
            m_boneConstants.transforms.fill(identity);
        }

        constexpr std::uint32_t flatNormalPixel = 0xffff8080u;
        D3D11_TEXTURE2D_DESC textureDescription{};
        textureDescription.Width = 1;
        textureDescription.Height = 1;
        textureDescription.MipLevels = 1;
        textureDescription.ArraySize = 1;
        textureDescription.Format =
            DXGI_FORMAT_R8G8B8A8_UNORM;
        textureDescription.SampleDesc.Count = 1;
        textureDescription.Usage = D3D11_USAGE_IMMUTABLE;
        textureDescription.BindFlags =
            D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA textureData{};
        textureData.pSysMem = &flatNormalPixel;
        textureData.SysMemPitch = sizeof(flatNormalPixel);

        Microsoft::WRL::ComPtr<ID3D11Texture2D> flatNormal;
        ThrowIfFailed(
            device->CreateTexture2D(
                &textureDescription,
                &textureData,
                flatNormal.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(flat normal)");
        ThrowIfFailed(
            device->CreateShaderResourceView(
                flatNormal.Get(),
                nullptr,
                m_flatNormalTexture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateShaderResourceView(flat normal)");

        // カスタムShaderの未設定テクスチャ枠へ渡す1x1の白。
        constexpr std::uint32_t whitePixel = 0xFFFFFFFFu;
        D3D11_SUBRESOURCE_DATA whiteData{};
        whiteData.pSysMem = &whitePixel;
        whiteData.SysMemPitch = sizeof(whitePixel);
        Microsoft::WRL::ComPtr<ID3D11Texture2D> white;
        ThrowIfFailed(
            device->CreateTexture2D(
                &textureDescription,
                &whiteData,
                white.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(white)");
        ThrowIfFailed(
            device->CreateShaderResourceView(
                white.Get(),
                nullptr,
                m_whiteTexture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateShaderResourceView(white)");

        D3D11_SAMPLER_DESC samplerDescription{};
        samplerDescription.Filter =
            D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDescription.AddressU =
            D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDescription.AddressV =
            D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDescription.AddressW =
            D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDescription.MaxAnisotropy = 1;
        samplerDescription.ComparisonFunc =
            D3D11_COMPARISON_NEVER;
        samplerDescription.MaxLOD =
            std::numeric_limits<float>::max();
        ThrowIfFailed(
            device->CreateSamplerState(
                &samplerDescription,
                m_sampler.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateSamplerState");

        samplerDescription.Filter =
            D3D11_FILTER_MIN_MAG_MIP_POINT;
        ThrowIfFailed(
            device->CreateSamplerState(
                &samplerDescription,
                m_pointSampler.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateSamplerState(point)");

        D3D11_SAMPLER_DESC shadowSamplerDescription{};
        shadowSamplerDescription.Filter =
            D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        shadowSamplerDescription.AddressU =
            D3D11_TEXTURE_ADDRESS_BORDER;
        shadowSamplerDescription.AddressV =
            D3D11_TEXTURE_ADDRESS_BORDER;
        shadowSamplerDescription.AddressW =
            D3D11_TEXTURE_ADDRESS_BORDER;
        shadowSamplerDescription.BorderColor[0] = 1.0f;
        shadowSamplerDescription.BorderColor[1] = 1.0f;
        shadowSamplerDescription.BorderColor[2] = 1.0f;
        shadowSamplerDescription.BorderColor[3] = 1.0f;
        shadowSamplerDescription.ComparisonFunc =
            D3D11_COMPARISON_LESS_EQUAL;
        shadowSamplerDescription.MinLOD = 0.0f;
        shadowSamplerDescription.MaxLOD = 0.0f;
        ThrowIfFailed(
            device->CreateSamplerState(
                &shadowSamplerDescription,
                m_shadowSampler.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateSamplerState(shadow)");
    }

    void LitEffect::SetMatrices(
        DirectX::FXMMATRIX world,
        DirectX::CXMMATRIX view,
        DirectX::CXMMATRIX projection) noexcept
    {
        using namespace DirectX;

        XMStoreFloat4x4(&m_objectConstants.world, world);
        XMStoreFloat4x4(
            &m_objectConstants.viewProjection,
            view * projection);

        XMVECTOR determinant{};
        const XMMATRIX worldInverseTranspose =
            XMMatrixTranspose(XMMatrixInverse(&determinant, world));
        XMStoreFloat4x4(
            &m_objectConstants.worldInverseTranspose,
            worldInverseTranspose);

        const XMMATRIX inverseView =
            XMMatrixInverse(&determinant, view);
        XMStoreFloat4(
            &m_objectConstants.cameraPosition,
            inverseView.r[3]);
        XMStoreFloat4(
            &m_objectConstants.cameraForward,
            XMVector3Normalize(
                XMVectorNegate(inverseView.r[2])));

        // カスタムシェーダーが揺れや流れを計算できるよう、
        // 経過時間をフレーム単位の定数として渡します。
        //
        // 秒は1時間で巻き戻します。floatの仮数は24ビットしかなく、
        // 起動から数時間そのまま渡すと下位が丸められて、波のような
        // 高い周波数の動きがカクつき始めるためです。巻き戻す周期を
        // 整数秒にしておくと、sin/cosを直接使う表現でも段差が
        // 出にくくなります。
        constexpr double TimeWrapSeconds = 3600.0;
        m_objectConstants.timeParameters = {
            static_cast<float>(
                std::fmod(
                    Time::TimeSinceStartup(),
                    TimeWrapSeconds)),
            Time::DeltaTime(),
            static_cast<float>(
                Time::FrameCount() & 0xFFFFFFull),
            0.0f
        };
    }

    void LitEffect::SetMaterial(
        const LitMaterial& material) noexcept
    {
        m_objectConstants.materialColor =
            material.BaseColor();
        // zはApply直前に、バインド済み法線テクスチャの有無で上書きします。
        m_objectConstants.materialParameters = {
            material.Roughness(),
            material.NormalStrength(),
            material.NormalTexture().empty() ? 0.0f : 1.0f,
            material.Metallic()
        };
        m_objectConstants.customParameters =
            material.CustomParameters();
        m_customVectorConstants.vectors =
            material.CustomVectors();
    }

    void LitEffect::SetTextures(
        ID3D11ShaderResourceView* albedoTexture,
        ID3D11ShaderResourceView* normalTexture,
        const PbrTextures& pbrTextures) noexcept
    {
        // アルベド未設定は白へ落とします。nullのSRVをバインドすると
        // サンプル結果が0になり、モデルが真っ黒になります
        // （baseColorTextureを持たないglTFにも適用します）。
        m_albedoTexture = albedoTexture != nullptr
            ? albedoTexture
            : m_whiteTexture.Get();
        m_normalTexture = normalTexture != nullptr
            ? normalTexture
            : m_flatNormalTexture.Get();
        m_roughnessTexture = pbrTextures.roughness;
        m_metallicTexture = pbrTextures.metallic;
        m_occlusionTexture = pbrTextures.occlusion;
        m_emissiveTexture = pbrTextures.emissive;
        m_occlusionStrength = pbrTextures.occlusionStrength;
        m_emissiveFactor = pbrTextures.emissiveFactor;
    }

    void LitEffect::SetCustomTextures(
        const std::array<
            ID3D11ShaderResourceView*,
            LitMaterial::CustomTextureCount>&
            textures) noexcept
    {
        m_customTextures = textures;
    }

    void LitEffect::SetLighting(
        const LightingState& lighting) noexcept
    {
        m_lightingConstants = {};
        m_lightingConstants.ambient = {
            lighting.ambientColor.x
                * std::max(lighting.ambientIntensity, 0.0f),
            lighting.ambientColor.y
                * std::max(lighting.ambientIntensity, 0.0f),
            lighting.ambientColor.z
                * std::max(lighting.ambientIntensity, 0.0f),
            1.0f
        };
        m_lightingConstants.fogColor = {
            lighting.fog.color.x,
            lighting.fog.color.y,
            lighting.fog.color.z,
            1.0f
        };
        m_lightingConstants.fogParameters = {
            lighting.fog.startDistance,
            lighting.fog.endDistance,
            lighting.fog.density,
            lighting.fog.enabled ? 1.0f : 0.0f
        };
        m_lightingConstants.lightCounts = {
            static_cast<std::uint32_t>(
                std::min(
                    lighting.directionalLightCount,
                    MaximumDirectionalLights)),
            static_cast<std::uint32_t>(
                std::min(
                    lighting.pointLightCount,
                    MaximumPointLights)),
            static_cast<std::uint32_t>(
                std::min(
                    lighting.spotLightCount,
                    MaximumSpotLights)),
            static_cast<std::uint32_t>(
                std::min(
                    lighting.directionalShadow.cascadeCount,
                    MaximumShadowCascades))
        };

        for (std::size_t index = 0;
            index < m_lightingConstants.lightCounts[0];
            ++index)
        {
            const auto& source = lighting.directionalLights[index];
            auto& destination =
                m_lightingConstants.directionalLights[index];
            destination.directionIntensity = {
                source.direction.x,
                source.direction.y,
                source.direction.z,
                source.intensity
            };
            // cbufferのレイアウトを維持するため、color.wへ太陽の角半径を格納します。
            destination.color = {
                source.color.x,
                source.color.y,
                source.color.z,
                source.angularRadius
            };
        }

        for (std::size_t index = 0;
            index < m_lightingConstants.lightCounts[1];
            ++index)
        {
            const auto& source = lighting.pointLights[index];
            auto& destination =
                m_lightingConstants.pointLights[index];
            destination.positionRange = {
                source.position.x,
                source.position.y,
                source.position.z,
                source.range
            };
            destination.colorIntensity = {
                source.color.x,
                source.color.y,
                source.color.z,
                source.intensity
            };
        }

        for (std::size_t index = 0;
            index < m_lightingConstants.lightCounts[2];
            ++index)
        {
            const auto& source = lighting.spotLights[index];
            auto& destination =
                m_lightingConstants.spotLights[index];
            destination.positionRange = {
                source.position.x,
                source.position.y,
                source.position.z,
                source.range
            };
            destination.directionInnerCosine = {
                source.direction.x,
                source.direction.y,
                source.direction.z,
                source.innerConeCosine
            };
            destination.colorIntensity = {
                source.color.x,
                source.color.y,
                source.color.z,
                source.intensity
            };
            destination.outerCosinePadding = {
                source.outerConeCosine,
                0.0f,
                0.0f,
                0.0f
            };
        }

        const auto& shadow = lighting.directionalShadow;
        m_lightingConstants.shadowViewProjections =
            shadow.lightViewProjections;
        m_lightingConstants.shadowCascadeSplits = {
            shadow.cascadeSplits[0],
            shadow.cascadeSplits[1],
            shadow.cascadeSplits[2],
            shadow.cascadeSplits[3]
        };
        m_lightingConstants.shadowParameters = {
            shadow.enabled
                ? static_cast<float>(shadow.lightIndex + 1)
                : 0.0f,
            shadow.bias,
            shadow.normalBias,
            shadow.strength
        };
        m_shadowTexture = shadow.enabled
            ? shadow.texture
            : nullptr;

        // スポットライトの影スロットを対応するライトへ紐付けます。
        for (std::size_t slot = 0;
            slot < MaximumSpotShadows;
            ++slot)
        {
            const auto& spotShadow =
                lighting.spotShadows[slot];
            if (!spotShadow.enabled
                || spotShadow.lightIndex < 0
                || static_cast<std::size_t>(
                    spotShadow.lightIndex)
                    >= m_lightingConstants.lightCounts[2])
            {
                continue;
            }
            m_lightingConstants
                .spotShadowViewProjections[slot] =
                spotShadow.lightViewProjection;
            m_lightingConstants
                .spotShadowParameters[slot] = {
                spotShadow.bias,
                spotShadow.normalBias,
                spotShadow.strength,
                1.0f
            };
            m_lightingConstants
                .spotLights[static_cast<std::size_t>(
                    spotShadow.lightIndex)]
                .outerCosinePadding.y =
                static_cast<float>(slot + 1);
        }
        m_spotShadowTexture =
            lighting.spotShadowTexture;

        const auto& pointShadow = lighting.pointShadow;
        const bool pointShadowActive =
            pointShadow.enabled
            && pointShadow.texture != nullptr
            && pointShadow.lightIndex >= 0
            && static_cast<std::size_t>(
                pointShadow.lightIndex)
                < m_lightingConstants.lightCounts[1];
        m_lightingConstants.pointShadowParameters = {
            pointShadowActive
                ? static_cast<float>(
                    pointShadow.lightIndex + 1)
                : 0.0f,
            pointShadow.bias,
            pointShadow.strength,
            0.0f
        };
        m_pointShadowTexture = pointShadowActive
            ? pointShadow.texture
            : nullptr;

        // PCF用のテクセルサイズ
        // （x=カスケード, y=スポット, z=ポイント）。
        m_lightingConstants.shadowTexelSizes = {
            1.0f / std::max(
                lighting.directionalShadowResolution,
                1.0f),
            1.0f / std::max(
                lighting.localShadowResolution,
                1.0f),
            1.0f / std::max(
                lighting.localShadowResolution,
                1.0f),
            0.0f
        };

        // 画面空間AO。テクスチャが無ければ無効にして、シェーダー側の
        // 掛け算を1.0に固定します。
        const auto& screenOcclusion =
            lighting.screenAmbientOcclusion;
        const bool screenOcclusionActive =
            screenOcclusion.enabled
            && screenOcclusion.texture != nullptr;
        m_lightingConstants
            .screenAmbientOcclusionParameters = {
            screenOcclusion.inverseWidth,
            screenOcclusion.inverseHeight,
            screenOcclusionActive ? 1.0f : 0.0f,
            0.0f
        };
        m_screenAmbientOcclusionTexture =
            screenOcclusionActive
                ? screenOcclusion.texture
                : nullptr;

        // SSR（画面空間反射）。前フレームのカラーと深度が揃って
        // いなければ無効にします（最初のフレームは履歴が無い）。
        const auto& screenReflection =
            lighting.screenSpaceReflection;
        const bool screenReflectionActive =
            screenReflection.enabled
            && screenReflection.texture != nullptr
            && screenReflection.depth != nullptr;
        m_lightingConstants.screenReflectionParameters = {
            std::clamp(screenReflection.intensity, 0.0f, 1.0f),
            screenReflectionActive ? 1.0f : 0.0f,
            std::max(screenReflection.maximumDistance, 0.01f),
            static_cast<float>(
                std::clamp<std::uint32_t>(
                    screenReflection.stepCount,
                    1u,
                    128u))
        };
        m_lightingConstants.screenReflectionScreen = {
            screenReflection.inverseWidth,
            screenReflection.inverseHeight,
            screenReflection.projectionZ,
            screenReflection.projectionW
        };
        m_lightingConstants.screenReflectionQuality = {
            std::max(screenReflection.thickness, 0.001f),
            std::clamp(
                screenReflection.roughnessCutoff,
                0.0f,
                1.0f),
            // z=Hi-Z深度ピラミッドの最終ミップ番号。
            static_cast<float>(
                screenReflection.depthPyramidMaximumMip),
            0.0f
        };
        m_lightingConstants
            .screenReflectionPreviousViewProjection =
                screenReflection.previousViewProjection;
        m_screenReflectionColorTexture =
            screenReflectionActive
                ? screenReflection.texture
                : nullptr;
        m_screenReflectionDepthTexture =
            screenReflectionActive
                ? screenReflection.depth
                : nullptr;

        // クラスタライトカリング（Forward+）。SRVが揃っていなければ
        // 無効にして従来の16灯経路へ落とします。
        const auto& clustered = lighting.clustered;
        const bool clusteredActive =
            clustered.enabled
            && clustered.lights != nullptr
            && clustered.lightIndices != nullptr
            && clustered.clusterCounts != nullptr;
        m_lightingConstants.clusteredParameters = {
            static_cast<float>(
                ClusteredLights::GridWidth),
            static_cast<float>(
                ClusteredLights::GridHeight),
            static_cast<float>(
                ClusteredLights::GridDepth),
            clusteredActive ? 1.0f : 0.0f
        };
        m_lightingConstants.clusteredDepthParameters = {
            clustered.nearPlane,
            clustered.farPlane,
            std::log(
                std::max(
                    clustered.farPlane
                        / std::max(
                            clustered.nearPlane,
                            0.0001f),
                    1.0001f)),
            static_cast<float>(
                ClusteredLights::MaximumLightsPerCluster)
        };
        m_lightingConstants.clusteredScreenParameters = {
            clustered.inverseWidth,
            clustered.inverseHeight,
            static_cast<float>(clustered.lightCount),
            0.0f
        };
        m_clusterLights =
            clusteredActive ? clustered.lights : nullptr;
        m_clusterIndexList = clusteredActive
            ? clustered.lightIndices
            : nullptr;
        m_clusterCounts = clusteredActive
            ? clustered.clusterCounts
            : nullptr;

        const auto& environment = lighting.environment;
        const bool environmentActive =
            environment.enabled
            && environment.texture != nullptr;
        // 事前フィルタ済みがあればそれを使い、z へ最終ミップ番号を
        // 載せます（0なら旧来のソース直接サンプリング）。
        const bool prefilteredActive =
            environmentActive
            && environment.specular != nullptr
            && environment.irradiance != nullptr;
        m_lightingConstants.environmentParameters = {
            std::max(environment.intensity, 0.0f),
            environmentActive ? 1.0f : 0.0f,
            prefilteredActive
                ? environment.specularMaximumMip
                : 0.0f,
            0.0f
        };
        m_environmentTexture = environmentActive
            ? (prefilteredActive
                ? environment.specular
                : environment.texture)
            : nullptr;
        m_irradianceTexture = prefilteredActive
            ? environment.irradiance
            : nullptr;
        // ベイクした間接光（照度ボリューム）。3枚のSH係数
        // テクスチャが揃っていなければ無効にして、シェーダーは
        // SH係数が不足している場合はフラットな環境光を使います。
        const auto& bakedGi = lighting.bakedGlobalIllumination;
        const bool bakedGiActive =
            bakedGi.enabled
            && bakedGi.redCoefficients != nullptr
            && bakedGi.greenCoefficients != nullptr
            && bakedGi.blueCoefficients != nullptr;
        m_lightingConstants.bakedGiVolumeMinimum = {
            bakedGi.volumeMinimum.x,
            bakedGi.volumeMinimum.y,
            bakedGi.volumeMinimum.z,
            bakedGiActive ? 1.0f : 0.0f
        };
        m_lightingConstants.bakedGiInverseSize = {
            1.0f / std::max(bakedGi.volumeSize.x, 0.0001f),
            1.0f / std::max(bakedGi.volumeSize.y, 0.0001f),
            1.0f / std::max(bakedGi.volumeSize.z, 0.0001f),
            std::max(bakedGi.intensity, 0.0f)
        };
        m_lightingConstants.bakedGiResolution = {
            std::max(bakedGi.resolution.x, 1.0f),
            std::max(bakedGi.resolution.y, 1.0f),
            std::max(bakedGi.resolution.z, 1.0f),
            0.0f
        };
        m_bakedGiRedTexture = bakedGiActive
            ? bakedGi.redCoefficients
            : nullptr;
        m_bakedGiGreenTexture = bakedGiActive
            ? bakedGi.greenCoefficients
            : nullptr;
        m_bakedGiBlueTexture = bakedGiActive
            ? bakedGi.blueCoefficients
            : nullptr;

        // プローブの2個目は前のオブジェクトの分が残らないよう
        // 毎回外します（m_lightingConstantsは先頭で丸ごと0に
        // していますが、テクスチャは別に持っているためです）。
        m_secondaryEnvironmentTexture = nullptr;
        m_secondaryIrradianceTexture = nullptr;
    }

    void LitEffect::SetEnvironmentOverride(
        const ReflectionProbeEnvironment& probe) noexcept
    {
        // リフレクションプローブによる、オブジェクト単位のIBL
        // 差し替えです。SetLightingがシーン共通の環境を設定した後、
        // SetLighting後、描画直前にプローブ設定を適用します。
        if (!probe.IsValid())
        {
            return;
        }
        m_lightingConstants.environmentParameters = {
            std::max(probe.intensity, 0.0f),
            1.0f,
            probe.specularMaximumMip,
            0.0f
        };
        m_environmentTexture = probe.specular;
        m_irradianceTexture = probe.irradiance;

        // ボックス射影。3軸すべてが正のときだけ有効にします
        // （0を含むと箱の内側が定義できず、除算で破綻します）。
        const auto boxParameters =
            [](const DirectX::XMFLOAT3& extents)
            {
                const bool active =
                    extents.x > 0.0f
                    && extents.y > 0.0f
                    && extents.z > 0.0f;
                return DirectX::XMFLOAT4{
                    extents.x,
                    extents.y,
                    extents.z,
                    active ? 1.0f : 0.0f
                };
            };
        m_lightingConstants.reflectionBoxCenter = {
            probe.boxCenter.x,
            probe.boxCenter.y,
            probe.boxCenter.z,
            0.0f
        };
        m_lightingConstants.reflectionBoxParameters =
            boxParameters(probe.boxExtents);

        // 2個目のプローブ。混ぜないときは比率0にしておけば、
        // Shader側は1個目だけを読みます（テクスチャも外します）。
        if (!probe.IsBlended())
        {
            m_secondaryEnvironmentTexture = nullptr;
            m_secondaryIrradianceTexture = nullptr;
            m_lightingConstants
                .reflectionSecondaryBoxCenter = {};
            m_lightingConstants
                .reflectionSecondaryBoxParameters = {};
            m_lightingConstants
                .reflectionBlendParameters = {};
            return;
        }
        m_secondaryEnvironmentTexture =
            probe.secondarySpecular;
        m_secondaryIrradianceTexture =
            probe.secondaryIrradiance;
        m_lightingConstants
            .reflectionSecondaryBoxCenter = {
                probe.secondaryBoxCenter.x,
                probe.secondaryBoxCenter.y,
                probe.secondaryBoxCenter.z,
                0.0f
            };
        m_lightingConstants
            .reflectionSecondaryBoxParameters =
                boxParameters(probe.secondaryBoxExtents);
        m_lightingConstants.reflectionBlendParameters = {
            std::clamp(probe.secondaryWeight, 0.0f, 1.0f),
            probe.secondarySpecularMaximumMip,
            0.0f,
            0.0f
        };
    }

    void LitEffect::SetBoneTransforms(
        const DirectX::XMMATRIX* transforms,
        const std::size_t count) noexcept
    {
        if (!m_skinned || transforms == nullptr)
        {
            return;
        }
        const auto safeCount = std::min(count, MaximumBones);
        for (std::size_t index = 0;
            index < safeCount;
            ++index)
        {
            DirectX::XMStoreFloat3x4(
                &m_boneConstants.transforms[index],
                transforms[index]);
        }
    }

    std::size_t LitEffect::RoleIndex(
        const ShaderPassRole role) noexcept
    {
        return static_cast<std::size_t>(role);
    }

    ShaderPassRole LitEffect::PrimaryRole() const noexcept
    {
        return m_skinned
            ? ShaderPassRole::Skinned
            : ShaderPassRole::Forward;
    }

    ShaderPassRole LitEffect::OutlineRole() const noexcept
    {
        return m_skinned
            ? ShaderPassRole::SkinnedOutline
            : ShaderPassRole::Outline;
    }

    const LitEffect::ManifestPass* LitEffect::ManifestPassAt(
        const ShaderPassRole role,
        const std::size_t index) const noexcept
    {
        if (!m_manifestEffect)
        {
            return nullptr;
        }
        const auto& passes = m_manifestPasses[RoleIndex(role)];
        return index < passes.size() ? &passes[index] : nullptr;
    }

    const LitEffect::ManifestPass* LitEffect::SelectedManifestPass(
        const ShaderPassRole role) const noexcept
    {
        return ManifestPassAt(
            role,
            m_selectedManifestPasses[RoleIndex(role)]);
    }

    const ShaderProgram* LitEffect::ActiveManifestProgram(
        const bool primaryOnly) const noexcept
    {
        if (!m_manifestEffect)
        {
            return nullptr;
        }

        if (primaryOnly)
        {
            const auto* const primary = ManifestPassAt(
                PrimaryRole(),
                0);
            return primary != nullptr ? &primary->program : nullptr;
        }

        ShaderPassRole role = PrimaryRole();
        if (m_manifestRoleOverride)
        {
            role = m_manifestOverrideRole;
        }
        else if (m_instancingEnabled)
        {
            role = ShaderPassRole::Instanced;
        }
        const auto* const selected = SelectedManifestPass(role);
        return selected != nullptr ? &selected->program : nullptr;
    }

    std::size_t LitEffect::PassCount(
        const ShaderPassRole role) const noexcept
    {
        if (m_manifestEffect)
        {
            return m_manifestPasses[RoleIndex(role)].size();
        }

        switch (role)
        {
        case ShaderPassRole::Forward:
            return m_skinned ? 0u : 1u;
        case ShaderPassRole::Skinned:
            return m_skinned ? 1u : 0u;
        case ShaderPassRole::Instanced:
            return m_instancedVertexShader != nullptr ? 1u : 0u;
        case ShaderPassRole::Outline:
            return !m_skinned
                    && m_outlineVertexShader != nullptr
                    && m_outlinePixelShader != nullptr
                ? 1u
                : 0u;
        case ShaderPassRole::SkinnedOutline:
            return m_skinned
                    && m_outlineVertexShader != nullptr
                    && m_outlinePixelShader != nullptr
                ? 1u
                : 0u;
        case ShaderPassRole::Occluded:
            return m_occludedPixelShader != nullptr
                    && m_occludedDepthState != nullptr
                ? 1u
                : 0u;
        }
        return 0;
    }

    void LitEffect::SelectPass(
        const ShaderPassRole role,
        const std::size_t index)
    {
        const auto count = PassCount(role);
        if (index >= count)
        {
            throw std::out_of_range(
                "Material shader role '"
                + std::string{ ManifestRoleName(role) }
                + "' pass index " + std::to_string(index)
                + " is out of range (count "
                + std::to_string(count) + ").");
        }
        if (m_manifestEffect)
        {
            m_selectedManifestPasses[RoleIndex(role)] = index;
        }
    }

    const ShaderRenderState& LitEffect::SelectedPassRenderState(
        const ShaderPassRole role) const
    {
        if (const auto* const pass = SelectedManifestPass(role))
        {
            return pass->renderState;
        }
        if (PassCount(role) != 0)
        {
            return m_renderState;
        }
        throw std::out_of_range(
            "Material shader has no pass with role '"
            + std::string{ ManifestRoleName(role) } + "'.");
    }

    ID3DBlob* LitEffect::SelectedPassVertexShaderByteCode(
        const ShaderPassRole role) const noexcept
    {
        if (const auto* const pass = SelectedManifestPass(role))
        {
            if (auto* const byteCode =
                    pass->program.VertexShaderByteCode())
            {
                return byteCode;
            }
            if (role == ShaderPassRole::Occluded)
            {
                const auto* const primary = ManifestPassAt(
                    PrimaryRole(),
                    0);
                return primary != nullptr
                    ? primary->program.VertexShaderByteCode()
                    : nullptr;
            }
            return nullptr;
        }
        if (PassCount(role) == 0)
        {
            return nullptr;
        }
        if (role == ShaderPassRole::Instanced)
        {
            return m_instancedVertexShaderByteCode.Get();
        }
        return m_vertexShaderByteCode.Get();
    }

    bool LitEffect::SelectedPassHasTessellation(
        const ShaderPassRole role) const noexcept
    {
        if (m_manifestEffect)
        {
            const auto* pass = SelectedManifestPass(role);
            if (role == ShaderPassRole::Occluded)
            {
                pass = ManifestPassAt(PrimaryRole(), 0);
            }
            return pass != nullptr
                && pass->program.HullShader() != nullptr
                && pass->program.DomainShader() != nullptr;
        }
        if (PassCount(role) == 0
            || role == ShaderPassRole::Outline
            || role == ShaderPassRole::SkinnedOutline)
        {
            return false;
        }
        return m_hullShader != nullptr && m_domainShader != nullptr;
    }

    bool LitEffect::SelectedPassHasGeometryShader(
        const ShaderPassRole role) const noexcept
    {
        if (m_manifestEffect)
        {
            const auto* pass = SelectedManifestPass(role);
            if (role == ShaderPassRole::Occluded)
            {
                pass = ManifestPassAt(PrimaryRole(), 0);
            }
            return pass != nullptr
                && pass->program.GeometryShader() != nullptr;
        }
        if (PassCount(role) == 0
            || role == ShaderPassRole::Outline
            || role == ShaderPassRole::SkinnedOutline)
        {
            return false;
        }
        return m_geometryShader != nullptr;
    }

    std::size_t LitEffect::ColorPassCount() const noexcept
    {
        return PassCount(PrimaryRole());
    }

    void LitEffect::SelectColorPass(const std::size_t index)
    {
        SelectPass(PrimaryRole(), index);
    }

    const ShaderRenderState& LitEffect::ColorPassRenderState(
        const std::size_t index) const
    {
        if (m_manifestEffect)
        {
            if (const auto* const pass = ManifestPassAt(
                    PrimaryRole(),
                    index))
            {
                return pass->renderState;
            }
        }
        else if (index == 0)
        {
            return m_renderState;
        }
        throw std::out_of_range(
            "Material color pass index " + std::to_string(index)
            + " is out of range (count "
            + std::to_string(ColorPassCount()) + ").");
    }

    ID3DBlob* LitEffect::ColorPassVertexShaderByteCode(
        const std::size_t index) const noexcept
    {
        if (m_manifestEffect)
        {
            const auto* const pass = ManifestPassAt(
                PrimaryRole(),
                index);
            return pass != nullptr
                ? pass->program.VertexShaderByteCode()
                : nullptr;
        }
        return index == 0 ? m_vertexShaderByteCode.Get() : nullptr;
    }

    const ShaderRenderState& LitEffect::RenderState() const noexcept
    {
        if (m_manifestEffect)
        {
            const auto role = m_instancingEnabled
                ? ShaderPassRole::Instanced
                : PrimaryRole();
            if (const auto* const pass = SelectedManifestPass(role))
            {
                return pass->renderState;
            }
        }
        return m_renderState;
    }

    bool LitEffect::HasTessellation() const noexcept
    {
        if (const auto* const program = ActiveManifestProgram(m_depthOnly))
        {
            return program->HullShader() != nullptr
                && program->DomainShader() != nullptr;
        }
        return m_hullShader != nullptr && m_domainShader != nullptr;
    }

    bool LitEffect::HasGeometryShader() const noexcept
    {
        if (const auto* const program = ActiveManifestProgram(m_depthOnly))
        {
            return program->GeometryShader() != nullptr;
        }
        return m_geometryShader != nullptr;
    }

    bool LitEffect::HasOutline() const noexcept
    {
        return PassCount(OutlineRole()) != 0;
    }

    bool LitEffect::HasOccludedPass() const noexcept
    {
        return PassCount(ShaderPassRole::Occluded) != 0;
    }

    bool LitEffect::SupportsInstancing() const noexcept
    {
        return PassCount(ShaderPassRole::Instanced) != 0;
    }

    ID3DBlob* LitEffect::InstancedVertexShaderByteCode() const noexcept
    {
        return SelectedPassVertexShaderByteCode(
            ShaderPassRole::Instanced);
    }

    void LitEffect::Apply(ID3D11DeviceContext* deviceContext)
    {
        auto* context = deviceContext != nullptr
            ? deviceContext
            : m_context;
        const auto* const manifestProgram =
            ActiveManifestProgram(m_depthOnly);
        auto* const activeVertexShader = manifestProgram != nullptr
            ? manifestProgram->VertexShader()
            : m_instancingEnabled
                ? m_instancedVertexShader.Get()
                : m_vertexShader.Get();
        auto* const activePixelShader = manifestProgram != nullptr
            ? manifestProgram->PixelShader()
            : m_pixelShader.Get();
        auto* const activeHullShader = manifestProgram != nullptr
            ? manifestProgram->HullShader()
            : m_hullShader.Get();
        auto* const activeDomainShader = manifestProgram != nullptr
            ? manifestProgram->DomainShader()
            : m_domainShader.Get();
        auto* const activeGeometryShader = manifestProgram != nullptr
            ? manifestProgram->GeometryShader()
            : m_geometryShader.Get();
        const bool bindTessellation = m_tessellationDraw
            && activeHullShader != nullptr
            && activeDomainShader != nullptr;
        ResolveTextureFlags();
        context->UpdateSubresource(
            m_objectBuffer.Get(),
            0,
            nullptr,
            &m_objectConstants,
            0,
            0);
        // 深度専用（シャドウ）パスはピクセルシェーダーを外し、
        // ライティング関連の更新とバインドを省略します。
        if (m_depthOnly)
        {
            if (m_skinned && m_boneBuffer)
            {
                context->UpdateSubresource(
                    m_boneBuffer.Get(),
                    0,
                    nullptr,
                    &m_boneConstants,
                    0,
                    0);
                ID3D11Buffer* vertexBuffers[]{
                    m_objectBuffer.Get(),
                    nullptr,
                    m_boneBuffer.Get()
                };
                context->VSSetConstantBuffers(
                    0,
                    static_cast<UINT>(
                        std::size(vertexBuffers)),
                    vertexBuffers);
            }
            else
            {
                ID3D11Buffer* vertexBuffers[]{
                    m_objectBuffer.Get()
                };
                context->VSSetConstantBuffers(
                    0,
                    1,
                    vertexBuffers);
            }
            context->VSSetShader(
                activeVertexShader,
                nullptr,
                0);
            // 深度だけのパスでも、パッチで描くなら束ねます。外すと
            // 位置を出す段（ドメイン）が無くなるので、影の形が
            // 分割前の板にならず、描画自体が失敗します。
            // 描く側が明示したときだけ束ねるのは通常のApplyと同じで、
            // 「持っているか」ではなく「今それで描くか」で決めます。
            // b3（自作Shaderのベクトル枠）は深度パスでは更新して
            // いなかったので、頂点より後ろの段を使うときだけ揃えます。
            // 揃えないと前の描画の値で形が決まり、影だけ形が違う
            // という追いにくい絵になります。
            if (bindTessellation || activeGeometryShader != nullptr)
            {
                context->UpdateSubresource(
                    m_customVectorBuffer.Get(),
                    0,
                    nullptr,
                    &m_customVectorConstants,
                    0,
                    0);
            }
            if (bindTessellation)
            {
                ID3D11Buffer* tessellationBuffers[]{
                    m_objectBuffer.Get()
                };
                context->HSSetConstantBuffers(
                    0,
                    1,
                    tessellationBuffers);
                context->DSSetConstantBuffers(
                    0,
                    1,
                    tessellationBuffers);
                ID3D11Buffer* customVectorBuffer[]{
                    m_customVectorBuffer.Get()
                };
                context->HSSetConstantBuffers(
                    3,
                    1,
                    customVectorBuffer);
                context->DSSetConstantBuffers(
                    3,
                    1,
                    customVectorBuffer);
            }
            context->HSSetShader(
                bindTessellation ? activeHullShader : nullptr,
                nullptr,
                0);
            context->DSSetShader(
                bindTessellation ? activeDomainShader : nullptr,
                nullptr,
                0);
            // 深度パスでもジオメトリシェーダーは束ねます。外すと
            // 影だけGSの前の形になり、本体と影がずれます。
            if (activeGeometryShader != nullptr)
            {
                ID3D11Buffer* geometryBuffers[]{
                    m_objectBuffer.Get()
                };
                context->GSSetConstantBuffers(
                    0,
                    1,
                    geometryBuffers);
                ID3D11Buffer* customVectorBuffer[]{
                    m_customVectorBuffer.Get()
                };
                context->GSSetConstantBuffers(
                    3,
                    1,
                    customVectorBuffer);
            }
            context->GSSetShader(
                activeGeometryShader,
                nullptr,
                0);
            context->PSSetShader(nullptr, nullptr, 0);
            return;
        }
        context->UpdateSubresource(
            m_lightingBuffer.Get(),
            0,
            nullptr,
            &m_lightingConstants,
            0,
            0);
        context->UpdateSubresource(
            m_customVectorBuffer.Get(),
            0,
            nullptr,
            &m_customVectorConstants,
            0,
            0);
        if (m_skinned && m_boneBuffer)
        {
            context->UpdateSubresource(
                m_boneBuffer.Get(),
                0,
                nullptr,
                &m_boneConstants,
                0,
                0);
        }

        if (m_skinned)
        {
            ID3D11Buffer* vertexBuffers[]{
                m_objectBuffer.Get(),
                nullptr,
                m_boneBuffer.Get()
            };
            context->VSSetConstantBuffers(
                0,
                static_cast<UINT>(std::size(vertexBuffers)),
                vertexBuffers);
        }
        else
        {
            ID3D11Buffer* vertexBuffers[]{
                m_objectBuffer.Get()
            };
            context->VSSetConstantBuffers(
                0,
                1,
                vertexBuffers);
        }

        ID3D11Buffer* pixelBuffers[]{
            m_objectBuffer.Get(),
            m_lightingBuffer.Get()
        };
        context->PSSetConstantBuffers(
            0,
            2,
            pixelBuffers);
        ID3D11Buffer* customVectorBuffer[]{
            m_customVectorBuffer.Get()
        };
        context->VSSetConstantBuffers(
            3,
            1,
            customVectorBuffer);
        context->HSSetConstantBuffers(
            3,
            1,
            customVectorBuffer);
        context->DSSetConstantBuffers(
            3,
            1,
            customVectorBuffer);
        context->PSSetConstantBuffers(
            3,
            1,
            customVectorBuffer);
        ID3D11Buffer* tessellationBuffers[]{
            m_objectBuffer.Get()
        };
        context->HSSetConstantBuffers(
            0,
            1,
            tessellationBuffers);
        context->DSSetConstantBuffers(
            0,
            1,
            tessellationBuffers);
        context->GSSetConstantBuffers(
            0,
            1,
            tessellationBuffers);
        context->GSSetConstantBuffers(
            3,
            1,
            customVectorBuffer);
        context->VSSetShader(
            activeVertexShader,
            nullptr,
            0);
        // パッチで描くときだけ束ねます。三角形リストのままハル
        // シェーダーが刺さっていると描画そのものが不正になります。
        context->HSSetShader(
            bindTessellation ? activeHullShader : nullptr,
            nullptr,
            0);
        context->DSSetShader(
            bindTessellation ? activeDomainShader : nullptr,
            nullptr,
            0);
        // 入力が三角形以外のGSはコンパイル時に拒否されるため、
        // 有効なジオメトリシェーダーをそのまま設定します。
        context->GSSetShader(
            activeGeometryShader,
            nullptr,
            0);
        context->PSSetShader(activePixelShader, nullptr, 0);
        BindMaterialAndShadowTextures(context);
        BindPbrTextures(context);
        // クラスタライトカリング（t16〜t18）。無効時はnullptrのまま
        // 渡します（シェーダーは有効フラグを見てから読むので、
        // nullを読むことはありません）。
        {
            ID3D11ShaderResourceView* clusterViews[]{
                m_clusterLights,
                m_clusterIndexList,
                m_clusterCounts
            };
            context->PSSetShaderResources(
                16,
                static_cast<UINT>(
                    std::size(clusterViews)),
                clusterViews);
        }
        // 2個目のリフレクションプローブ（t19/t20）。t7〜t10は下の
        // カスタムテクスチャ枠なので、そこは使えません。混ぜない
        // フレームはnullptrのままで、シェーダーは比率0を見て
        // 読みに行きません。
        {
            ID3D11ShaderResourceView* secondaryProbe[]{
                m_secondaryEnvironmentTexture,
                m_secondaryIrradianceTexture
            };
            context->PSSetShaderResources(
                19,
                static_cast<UINT>(
                    std::size(secondaryProbe)),
                secondaryProbe);
        }
        // SSR（t21=前フレームのカラー, t22=深度）。無効なフレームは
        // nullptrのままで、シェーダーは有効フラグを見てから読みます。
        {
            ID3D11ShaderResourceView* reflectionViews[]{
                m_screenReflectionColorTexture,
                m_screenReflectionDepthTexture
            };
            context->PSSetShaderResources(
                21,
                static_cast<UINT>(
                    std::size(reflectionViews)),
                reflectionViews);
        }
        // ベイクした間接光のSH係数（t23〜t25）。無効時はnullptrの
        // ままで、シェーダーは有効フラグを見てから読みます。
        {
            ID3D11ShaderResourceView* bakedGiViews[]{
                m_bakedGiRedTexture,
                m_bakedGiGreenTexture,
                m_bakedGiBlueTexture
            };
            context->PSSetShaderResources(
                23,
                static_cast<UINT>(
                    std::size(bakedGiViews)),
                bakedGiViews);
        }
        // カスタムShader用の追加テクスチャ（t7以降）。未設定の枠は
        // 白テクスチャにして、シェーダー側の分岐を不要にします。
        std::array<
            ID3D11ShaderResourceView*,
            LitMaterial::CustomTextureCount>
            customTextures{};
        for (std::size_t index = 0;
            index < customTextures.size();
            ++index)
        {
            customTextures[index] =
                m_customTextures[index] != nullptr
                    ? m_customTextures[index]
                    : m_whiteTexture.Get();
        }
        context->PSSetShaderResources(
            static_cast<UINT>(
                LitMaterial::CustomTextureFirstSlot),
            static_cast<UINT>(customTextures.size()),
            customTextures.data());
        ID3D11SamplerState* samplers[]{
            ActiveMaterialSampler(),
            m_shadowSampler.Get()
        };
        context->PSSetSamplers(0, 2, samplers);
    }

    void LitEffect::ApplyOutline(
        ID3D11DeviceContext* deviceContext)
    {
        if (!HasOutline())
        {
            return;
        }
        auto* context = deviceContext != nullptr
            ? deviceContext
            : m_context;
        if (m_manifestEffect)
        {
            const bool previousOverride = m_manifestRoleOverride;
            const auto previousRole = m_manifestOverrideRole;
            const bool previousDepthOnly = m_depthOnly;
            m_manifestRoleOverride = true;
            m_manifestOverrideRole = OutlineRole();
            m_depthOnly = false;
            try
            {
                Apply(context);
            }
            catch (...)
            {
                m_manifestRoleOverride = previousOverride;
                m_manifestOverrideRole = previousRole;
                m_depthOnly = previousDepthOnly;
                throw;
            }
            m_manifestRoleOverride = previousOverride;
            m_manifestOverrideRole = previousRole;
            m_depthOnly = previousDepthOnly;
            return;
        }
        context->UpdateSubresource(
            m_objectBuffer.Get(),
            0,
            nullptr,
            &m_objectConstants,
            0,
            0);
        if (m_skinned)
        {
            context->UpdateSubresource(
                m_boneBuffer.Get(),
                0,
                nullptr,
                &m_boneConstants,
                0,
                0);
        }

        ID3D11Buffer* vertexBuffers[]{
            m_objectBuffer.Get(),
            nullptr,
            m_boneBuffer.Get()
        };
        context->VSSetConstantBuffers(
            0,
            m_skinned
                ? static_cast<UINT>(
                    std::size(vertexBuffers))
                : 1u,
            vertexBuffers);
        ID3D11Buffer* pixelBuffers[]{
            m_objectBuffer.Get()
        };
        context->PSSetConstantBuffers(0, 1, pixelBuffers);
        context->VSSetShader(
            m_outlineVertexShader.Get(),
            nullptr,
            0);
        context->HSSetShader(nullptr, nullptr, 0);
        context->DSSetShader(nullptr, nullptr, 0);
        // 輪郭用頂点シェーダーはGSMainの入力と一致する保証がないため、
        // ジオメトリシェーダーを設定しません。
        context->GSSetShader(nullptr, nullptr, 0);
        context->PSSetShader(
            m_outlinePixelShader.Get(),
            nullptr,
            0);
        ID3D11ShaderResourceView* textures[]{
            m_albedoTexture
        };
        context->PSSetShaderResources(0, 1, textures);
        ID3D11SamplerState* samplers[]{
            ActiveMaterialSampler()
        };
        context->PSSetSamplers(0, 1, samplers);
    }

    void LitEffect::ApplyOccluded(
        ID3D11DeviceContext* deviceContext)
    {
        if (!HasOccludedPass())
        {
            return;
        }
        auto* context = deviceContext != nullptr
            ? deviceContext
            : m_context;
        if (m_manifestEffect)
        {
            // Occludedはstatic/skinnedいずれの入力にも同じ定義を
            // 使えるようpixel-onlyです。primary pipelineを適用後、
            // 選択されたrole passのPSだけを差し替えます。
            const auto primaryRole = PrimaryRole();
            const auto primaryRoleIndex = RoleIndex(primaryRole);
            const auto previousPrimaryIndex =
                m_selectedManifestPasses[primaryRoleIndex];
            const bool previousInstancing = m_instancingEnabled;
            const bool previousDepthOnly = m_depthOnly;
            const bool previousOverride = m_manifestRoleOverride;
            const auto previousOverrideRole = m_manifestOverrideRole;
            m_selectedManifestPasses[primaryRoleIndex] = 0;
            m_instancingEnabled = false;
            m_depthOnly = false;
            m_manifestRoleOverride = false;
            try
            {
                Apply(context);
            }
            catch (...)
            {
                m_selectedManifestPasses[primaryRoleIndex] =
                    previousPrimaryIndex;
                m_instancingEnabled = previousInstancing;
                m_depthOnly = previousDepthOnly;
                m_manifestRoleOverride = previousOverride;
                m_manifestOverrideRole = previousOverrideRole;
                throw;
            }
            m_selectedManifestPasses[primaryRoleIndex] =
                previousPrimaryIndex;
            m_instancingEnabled = previousInstancing;
            m_depthOnly = previousDepthOnly;
            m_manifestRoleOverride = previousOverride;
            m_manifestOverrideRole = previousOverrideRole;
            const auto* const occluded = SelectedManifestPass(
                ShaderPassRole::Occluded);
            context->PSSetShader(
                occluded->program.PixelShader(),
                nullptr,
                0);
            context->OMSetDepthStencilState(
                m_occludedDepthState.Get(),
                0);
            return;
        }
        Apply(context);
        context->PSSetShader(
            m_occludedPixelShader.Get(),
            nullptr,
            0);
        context->OMSetDepthStencilState(
            m_occludedDepthState.Get(),
            0);
    }

    void LitEffect::ApplyPixelOnly(
        ID3D11DeviceContext* deviceContext)
    {
        auto* context = deviceContext != nullptr
            ? deviceContext
            : m_context;
        ResolveTextureFlags();
        context->UpdateSubresource(
            m_objectBuffer.Get(),
            0,
            nullptr,
            &m_objectConstants,
            0,
            0);
        context->UpdateSubresource(
            m_lightingBuffer.Get(),
            0,
            nullptr,
            &m_lightingConstants,
            0,
            0);

        ID3D11Buffer* pixelBuffers[]{
            m_objectBuffer.Get(),
            m_lightingBuffer.Get()
        };
        context->PSSetConstantBuffers(
            0,
            static_cast<UINT>(std::size(pixelBuffers)),
            pixelBuffers);
        // この経路はDirectXTKの頂点シェーダーと組み合わせて使うので、
        // programmable geometry stagesが期待する入力とは限りません。
        // 前のdrawのHS/DS/GSも含め、すべて明示的に外します。
        context->HSSetShader(nullptr, nullptr, 0);
        context->DSSetShader(nullptr, nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0);
        const auto* const primary = SelectedManifestPass(
            PrimaryRole());
        context->PSSetShader(
            primary != nullptr
                ? primary->program.PixelShader()
                : m_pixelShader.Get(),
            nullptr,
            0);
        // スキニング経路でもIBLとスポット／ポイント影を使えるよう、
        // アルベドや法線を含むt0〜t6をすべてバインドします。
        BindMaterialAndShadowTextures(context);
        BindPbrTextures(context);
        ID3D11SamplerState* samplers[]{
            ActiveMaterialSampler(),
            m_shadowSampler.Get()
        };
        context->PSSetSamplers(
            0,
            static_cast<UINT>(std::size(samplers)),
            samplers);
    }

    void LitEffect::BindMaterialAndShadowTextures(
        ID3D11DeviceContext* context) const noexcept
    {
        // SetTextures未呼び出し時もnullを設定しないよう、
        // 白テクスチャとフラット法線へフォールバックします。
        ID3D11ShaderResourceView* textures[]{
            m_albedoTexture != nullptr
                ? m_albedoTexture
                : m_whiteTexture.Get(),
            m_normalTexture != nullptr
                ? m_normalTexture
                : m_flatNormalTexture.Get(),
            m_shadowTexture,
            m_environmentTexture,
            m_spotShadowTexture,
            m_pointShadowTexture,
            m_irradianceTexture
        };
        context->PSSetShaderResources(
            0,
            static_cast<UINT>(std::size(textures)),
            textures);
    }

    void LitEffect::BindPbrTextures(
        ID3D11DeviceContext* context) const noexcept
    {
        auto* const white = m_whiteTexture.Get();
        ID3D11ShaderResourceView* textures[]{
            m_roughnessTexture != nullptr
                ? m_roughnessTexture
                : white,
            m_metallicTexture != nullptr
                ? m_metallicTexture
                : white,
            m_occlusionTexture != nullptr
                ? m_occlusionTexture
                : white,
            m_emissiveTexture != nullptr
                ? m_emissiveTexture
                : white,
            // t15＝画面空間AO。未設定なら白＝遮蔽なしになります。
            m_screenAmbientOcclusionTexture != nullptr
                ? m_screenAmbientOcclusionTexture
                : white
        };
        context->PSSetShaderResources(
            11,
            static_cast<UINT>(std::size(textures)),
            textures);
    }

    void LitEffect::ResolveTextureFlags() noexcept
    {
        const bool hasNormalMap =
            m_normalTexture != nullptr
            && m_normalTexture != m_flatNormalTexture.Get();
        m_objectConstants.materialParameters.z =
            hasNormalMap ? 1.0f : 0.0f;
        m_objectConstants.materialTextureParameters = {
            m_roughnessTexture != nullptr ? 1.0f : 0.0f,
            m_metallicTexture != nullptr ? 1.0f : 0.0f,
            m_occlusionTexture != nullptr ? 1.0f : 0.0f,
            m_occlusionStrength
        };
        m_objectConstants.emissiveParameters = {
            m_emissiveFactor.x,
            m_emissiveFactor.y,
            m_emissiveFactor.z,
            m_emissiveTexture != nullptr ? 1.0f : 0.0f
        };
    }

    ID3D11SamplerState*
        LitEffect::ActiveMaterialSampler() const noexcept
    {
        return m_objectConstants.customParameters[7].w >= 0.5f
            ? m_pointSampler.Get()
            : m_sampler.Get();
    }

    void LitEffect::GetVertexShaderBytecode(
        const void** shaderByteCode,
        std::size_t* byteCodeLength)
    {
        if (shaderByteCode == nullptr || byteCodeLength == nullptr)
        {
            throw std::invalid_argument(
                "Shader bytecode output pointers cannot be null.");
        }
        ID3DBlob* byteCode = m_vertexShaderByteCode.Get();
        if (m_manifestEffect)
        {
            const auto* const program = ActiveManifestProgram(
                m_depthOnly);
            byteCode = program != nullptr
                ? program->VertexShaderByteCode()
                : nullptr;
        }
        if (byteCode == nullptr)
        {
            throw std::runtime_error(
                "The active material pass has no vertex shader bytecode.");
        }
        *shaderByteCode = byteCode->GetBufferPointer();
        *byteCodeLength = byteCode->GetBufferSize();
    }
}
