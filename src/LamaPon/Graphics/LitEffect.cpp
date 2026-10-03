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
    // ASCIIの英大文字だけ小文字へ変換する(value: 変換する文字列)。
    [[nodiscard]] std::string FoldAsciiLower(
        const std::string_view value)
    {
        // ASCII英字を小文字にした結果
        std::string folded;
        folded.reserve(value.size());
        // 変換するASCII文字
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

    // Manifestに記載する役割名を返す(role: 描画の役割)。
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

    // Manifestの描画状態を検証して変換し、不正値なら例外を送出する(description: 合成・カリング・深度の宣言)。
    [[nodiscard]] LamaPon::ShaderRenderState
        ConvertManifestRenderState(
            const LamaPon::RenderStateDesc& description)
    {
        // 変換する描画状態
        LamaPon::ShaderRenderState state;
        state.declared = true;

        // 小文字に揃えた合成方式
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

        // 小文字に揃えたカリング方式
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

        // 小文字に揃えた深度比較
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

    // 失敗したHRESULTを処理名付きの例外に変換する(result: 処理結果, operation: 処理名)。
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

    // キャッシュを利用してコンパイルし、失敗なら例外を送出する(assets: 読み込み元, path: HLSLのパス, entryPoint: エントリー名, target: シェーダーモデル, keywords: define指定)。
    Microsoft::WRL::ComPtr<ID3DBlob> CompileShader(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& path,
        const char* entryPoint,
        const char* target,
        const std::vector<std::string>& keywords)
    {

        return LamaPon::CompileShaderCached(
            assets,
            path,
            entryPoint,
            target,
            keywords);
    }

    // 任意のエントリーをコンパイルし、失敗なら空を返す(assets: 読み込み元, path: HLSLのパス, entryPoint: エントリー名, target: シェーダーモデル, keywords: define指定)。
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

    // 16バイト境界に合う型Tの定数バッファを生成する(device: 非空のD3D11機器)。
    template<typename T>
    Microsoft::WRL::ComPtr<ID3D11Buffer> CreateConstantBuffer(
        ID3D11Device* device)
    {
        static_assert(sizeof(T) % 16 == 0);

        // 定数バッファの生成設定
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = static_cast<UINT>(sizeof(T));
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

        // 生成した定数バッファ
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

        // Manifestによる材質指定か
        const bool manifestShader = IsShaderManifestPath(shaderPath);
        if (manifestShader)
        {
            // 読み込んだ材質Manifest
            ShaderAssetDesc shaderAsset;
            // Manifest読み込みの詳細
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

            // モデルの種類に合う通常役割
            const auto primaryRole = skinned
                ? ShaderPassRole::Skinned
                : ShaderPassRole::Forward;
            // 通常描画の役割があるか調べる(pass: Manifestのパス宣言)。
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
            // 生成するManifestのパス
            for (const auto& pass : shaderAsset.passes)
            {
                // モデルの種類で使うパスか
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

                // 検証済みのパス描画状態
                ShaderRenderState renderState;
                try
                {
                    renderState = ConvertManifestRenderState(
                        pass.renderState);
                }
                // 不正な状態宣言の詳細
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

                // 生成するパスのシェーダー
                ShaderProgram program;
                // コンパイル失敗の詳細
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
            // HLSLの描画状態を読み取り、ソースを含まない配布では保存済みメタデータを使う。
            if (!assets.IsArchived() || assets.FileExists(shaderPath))
            {
                // 描画状態を読むHLSLデータ
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
                // メタデータがない旧キャッシュでは既定の描画状態を使う。
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
            // 通常PSのバイトコード
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

        // ハルシェーダーのバイトコード
        Microsoft::WRL::ComPtr<ID3DBlob>
            hullShaderByteCode;
        // ドメイン処理のバイトコード
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
        // GSへの入力は三角形のみとし、点・線入力による不正描画を生成時に拒否する。
        if (!manifestShader)
        {
            // GSのバイトコード
            if (const auto geometryShaderByteCode =
                    TryCompileShader(
                        assets,
                        shaderPath,
                        "GSMain",
                        "gs_5_0",
                        keywords))
            {
                // GS入力を確認するリフレクション
                Microsoft::WRL::ComPtr<ID3D11ShaderReflection>
                    reflection;
                // GSの入力プリミティブ情報
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
            // 輪郭VSのエントリー名
            const char* outlineVertexEntry =
                skinned
                    ? "VSSkinnedOutline"
                    : "VSOutline";
            // 輪郭VSのバイトコード
            const auto outlineVertexByteCode =
                TryCompileShader(
                    assets,
                    shaderPath,
                    outlineVertexEntry,
                    "vs_5_0",
            keywords);
            // 輪郭PSのバイトコード
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
        // 遮蔽PSのバイトコード
        Microsoft::WRL::ComPtr<ID3DBlob>
            occludedPixelByteCode;
        if (!manifestShader)
        {
            occludedPixelByteCode = TryCompileShader(
                assets,
                shaderPath,
                skinned ? "PSSkinnedOccluded" : "PSOccluded",
                "ps_5_0",
                keywords);
            if (skinned && !occludedPixelByteCode)
            {
                // スキニング専用の遮蔽入口がなければ、互換用のPSOccludedを使う。
                occludedPixelByteCode = TryCompileShader(
                    assets,
                    shaderPath,
                    "PSOccluded",
                    "ps_5_0",
                    keywords);
            }
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
            // 遮蔽用のGreater深度設定
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
            // ボーンの初期単位行列
            DirectX::XMFLOAT3X4 identity{};
            DirectX::XMStoreFloat3x4(
                &identity,
                DirectX::XMMatrixIdentity());
            m_boneConstants.transforms.fill(identity);
        }

        // 平坦法線のRGBA8値
        constexpr std::uint32_t flatNormalPixel = 0xffff8080u;
        // 代替用の1画素画像設定
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

        // 平坦法線の初期画像データ
        D3D11_SUBRESOURCE_DATA textureData{};
        textureData.pSysMem = &flatNormalPixel;
        textureData.SysMemPitch = sizeof(flatNormalPixel);

        // 保持する平坦法線画像
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


        // 白のRGBA8値
        constexpr std::uint32_t whitePixel = 0xFFFFFFFFu;
        // 白1画素の初期画像データ
        D3D11_SUBRESOURCE_DATA whiteData{};
        whiteData.pSysMem = &whitePixel;
        whiteData.SysMemPitch = sizeof(whitePixel);
        // 保持する白1画素画像
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

        // 材質用のサンプラー設定
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

        // 影用の比較サンプラー設定
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

        // 逆行列計算時の行列式
        XMVECTOR determinant{};
        // 法線変換用のワールド逆転置
        const XMMATRIX worldInverseTranspose =
            XMMatrixTranspose(XMMatrixInverse(&determinant, world));
        XMStoreFloat4x4(
            &m_objectConstants.worldInverseTranspose,
            worldInverseTranspose);

        // カメラ位置を求める逆ビュー
        const XMMATRIX inverseView =
            XMMatrixInverse(&determinant, view);
        XMStoreFloat4(
            &m_objectConstants.cameraPosition,
            inverseView.r[3]);
        XMStoreFloat4(
            &m_objectConstants.cameraForward,
            XMVector3Normalize(
                XMVectorNegate(inverseView.r[2])));

        // 長時間起動時のfloat精度を保つため、経過秒数は1時間周期で巻き戻す。
        // 経過秒数を巻き戻す周期
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
        // 法線の有効値はApply直前に実際の画像参照から確定する。
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
        // 色画像がない材質を黒にしないよう、未指定は白画像を使う。
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
        SetLightingD3D11(lighting, {});
    }

    void LitEffect::SetLightingD3D11(
        const LightingState& lighting,
        const D3D11LightingViews& views) noexcept
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

        // 設定するライトまたはボーン番号
        for (std::size_t index = 0;
            index < m_lightingConstants.lightCounts[0];
            ++index)
        {
            // 設定元のライト情報
            const auto& source = lighting.directionalLights[index];
            // GPUへ渡すライト定数
            auto& destination =
                m_lightingConstants.directionalLights[index];
            destination.directionIntensity = {
                source.direction.x,
                source.direction.y,
                source.direction.z,
                source.intensity
            };
            // 定数バッファの配置を保ち、色のwへ太陽の角半径を格納する。
            destination.color = {
                source.color.x,
                source.color.y,
                source.color.z,
                source.angularRadius
            };
        }

        // 設定するライトまたはボーン番号
        for (std::size_t index = 0;
            index < m_lightingConstants.lightCounts[1];
            ++index)
        {
            // 設定元のライト情報
            const auto& source = lighting.pointLights[index];
            // GPUへ渡すライト定数
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

        // 設定するライトまたはボーン番号
        for (std::size_t index = 0;
            index < m_lightingConstants.lightCounts[2];
            ++index)
        {
            // 設定元のライト情報
            const auto& source = lighting.spotLights[index];
            // GPUへ渡すライト定数
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

        // 平行光の影設定
        const auto& shadow = lighting.directionalShadow;
        // 平行光の影を適用できるか
        const bool directionalShadowActive =
            shadow.enabled
            && views.directionalShadow != nullptr
            && shadow.cascadeCount != 0
            && shadow.lightIndex
                < m_lightingConstants.lightCounts[0];
        m_lightingConstants.lightCounts[3] =
            directionalShadowActive
                ? m_lightingConstants.lightCounts[3]
                : 0u;
        m_lightingConstants.shadowViewProjections =
            shadow.lightViewProjections;
        m_lightingConstants.shadowCascadeSplits = {
            shadow.cascadeSplits[0],
            shadow.cascadeSplits[1],
            shadow.cascadeSplits[2],
            shadow.cascadeSplits[3]
        };
        m_lightingConstants.shadowParameters = {
            directionalShadowActive
                ? static_cast<float>(shadow.lightIndex + 1)
                : 0.0f,
            shadow.bias,
            shadow.normalBias,
            shadow.strength
        };
        m_shadowTexture = directionalShadowActive
            ? views.directionalShadow
            : nullptr;


        // 有効なスポット影があるか
        bool spotShadowActive{};
        // スポット影の配列番号
        for (std::size_t slot = 0;
            slot < MaximumSpotShadows;
            ++slot)
        {
            // 今回のスポット影設定
            const auto& spotShadow =
                lighting.spotShadows[slot];
            if (views.spotShadow == nullptr
                || !spotShadow.enabled
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
            spotShadowActive = true;
        }
        m_spotShadowTexture =
            spotShadowActive ? views.spotShadow : nullptr;

        // 点光源の影設定
        const auto& pointShadow = lighting.pointShadow;
        // 点光源の影を適用できるか
        const bool pointShadowActive =
            pointShadow.enabled
            && views.pointShadow != nullptr
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
            ? views.pointShadow
            : nullptr;


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

        // AO画像がなければ有効値を外して遮蔽を掛けない。
        // 画面空間AOの設定
        const auto& screenOcclusion =
            lighting.screenAmbientOcclusion;
        // AO画像を利用できるか
        const bool screenOcclusionActive =
            screenOcclusion.enabled
            && views.screenAmbientOcclusion != nullptr;
        m_lightingConstants
            .screenAmbientOcclusionParameters = {
            screenOcclusion.inverseWidth,
            screenOcclusion.inverseHeight,
            screenOcclusionActive ? 1.0f : 0.0f,
            0.0f
        };
        m_screenAmbientOcclusionTexture =
            screenOcclusionActive
                ? views.screenAmbientOcclusion
                : nullptr;

        // SSRは色履歴と深度画像が両方ある場合だけ有効にする。
        // 画面空間反射の設定
        const auto& screenReflection =
            lighting.screenSpaceReflection;
        // 反射の色と深度が揃うか
        const bool screenReflectionActive =
            screenReflection.enabled
            && views.screenSpaceReflection[0] != nullptr
            && views.screenSpaceReflection[1] != nullptr;
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
            static_cast<float>(
                screenReflection.depthPyramidMaximumMip),
            0.0f
        };
        m_lightingConstants
            .screenReflectionPreviousViewProjection =
                screenReflection.previousViewProjection;
        m_screenReflectionColorTexture =
            screenReflectionActive
                ? views.screenSpaceReflection[0]
                : nullptr;
        m_screenReflectionDepthTexture =
            screenReflectionActive
                ? views.screenSpaceReflection[1]
                : nullptr;

        // クラスタ用の画像3本がなければ、通常のライト配列を使う。
        // クラスタ照明の設定
        const auto& clustered = lighting.clustered;
        // クラスタ画像3本が揃うか
        const bool clusteredActive =
            clustered.enabled
            && views.clustered[0] != nullptr
            && views.clustered[1] != nullptr
            && views.clustered[2] != nullptr;
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
            clusteredActive ? views.clustered[0] : nullptr;
        m_clusterIndexList = clusteredActive
            ? views.clustered[1]
            : nullptr;
        m_clusterCounts = clusteredActive
            ? views.clustered[2]
            : nullptr;

        // 共通の環境照明設定
        const auto& environment = lighting.environment;
        // 環境の原画像を利用できるか
        const bool environmentActive =
            environment.enabled
            && views.environment[0] != nullptr;
        // 鏡面・拡散の事前畳み込み画像が両方なければ、元の環境画像を直接読む。
        // 鏡面と拡散の画像が揃うか
        const bool prefilteredActive =
            environmentActive
            && views.environment[1] != nullptr
            && views.environment[2] != nullptr;
        m_lightingConstants.environmentParameters = {
            environmentActive
                ? std::max(environment.intensity, 0.0f)
                : 0.0f,
            environmentActive ? 1.0f : 0.0f,
            prefilteredActive
                ? environment.specularMaximumMip
                : 0.0f,
            0.0f
        };
        m_environmentTexture = environmentActive
            ? (prefilteredActive
                ? views.environment[1]
                : views.environment[0])
            : nullptr;
        m_irradianceTexture = prefilteredActive
            ? views.environment[2]
            : nullptr;
        // 間接光のRGB別SH係数が揃わない場合は、通常の環境光を使う。
        // 事前計算した間接光の設定
        const auto& bakedGi = lighting.bakedGlobalIllumination;
        // 間接光のRGB画像が揃うか
        const bool bakedGiActive =
            bakedGi.enabled
            && views.bakedGlobalIllumination[0] != nullptr
            && views.bakedGlobalIllumination[1] != nullptr
            && views.bakedGlobalIllumination[2] != nullptr;
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
            ? views.bakedGlobalIllumination[0]
            : nullptr;
        m_bakedGiGreenTexture = bakedGiActive
            ? views.bakedGlobalIllumination[1]
            : nullptr;
        m_bakedGiBlueTexture = bakedGiActive
            ? views.bakedGlobalIllumination[2]
            : nullptr;

        // 前の物体の副プローブを引き継がないよう、画像参照も毎回解除する。
        m_secondaryEnvironmentTexture = nullptr;
        m_secondaryIrradianceTexture = nullptr;
    }

    void LitEffect::SetEnvironmentOverride(
        const ReflectionProbeEnvironment& probe) noexcept
    {
        SetEnvironmentOverrideD3D11(probe, {});
    }

    void LitEffect::SetEnvironmentOverrideD3D11(
        const ReflectionProbeEnvironment& probe,
        const D3D11ReflectionProbeViews& views) noexcept
    {
        // 共通の照明を設定した後、描画直前に物体用プローブを適用する。
        if (views.specular == nullptr
            || views.irradiance == nullptr)
        {
            return;
        }
        m_lightingConstants.environmentParameters = {
            std::max(probe.intensity, 0.0f),
            1.0f,
            probe.specularMaximumMip,
            0.0f
        };
        m_environmentTexture = views.specular;
        m_irradianceTexture = views.irradiance;


        // 各軸の半径が正のときだけ箱射影を有効にする(extents: 箱のxyz半径)。
        const auto boxParameters =
            [](const DirectX::XMFLOAT3& extents)
            {
                // 箱の3軸の半径が正か
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

        // 副プローブの画像が不足するか比率がゼロなら、参照と混合定数を解除する。
        if (views.secondarySpecular == nullptr
            || views.secondaryIrradiance == nullptr
            || !(probe.secondaryWeight > 0.0f))
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
            views.secondarySpecular;
        m_secondaryIrradianceTexture =
            views.secondaryIrradiance;
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
        // 上限72本以内のボーン数
        const auto safeCount = std::min(count, MaximumBones);
        // 設定するライトまたはボーン番号
        for (std::size_t index = 0;
            // 上限72本以内のボーン数
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
        // 役割に属するManifestパス列
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
            // 通常パスの先頭シェーダー
            const auto* const primary = ManifestPassAt(
                PrimaryRole(),
                0);
            return primary != nullptr ? &primary->program : nullptr;
        }

        // 参照する描画の役割
        ShaderPassRole role = PrimaryRole();
        if (m_manifestRoleOverride)
        {
            role = m_manifestOverrideRole;
        }
        else if (m_instancingEnabled)
        {
            role = ShaderPassRole::Instanced;
        }
        // 役割内で選択したパス
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
        // 役割内の選択可能なパス数
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
        // 参照するManifestパス
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
        // 参照するManifestパス
        if (const auto* const pass = SelectedManifestPass(role))
        {
            // 借用する頂点バイトコード
            if (auto* const byteCode =
                    pass->program.VertexShaderByteCode())
            {
                return byteCode;
            }
            if (role == ShaderPassRole::Occluded)
            {
                // 通常パスの先頭シェーダー
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
            // 参照するManifestパス
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
            // 参照するManifestパス
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
            // 参照するManifestパス
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
            // 参照するManifestパス
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
            // 参照する描画の役割
            const auto role = m_instancingEnabled
                ? ShaderPassRole::Instanced
                : PrimaryRole();
            // 参照するManifestパス
            if (const auto* const pass = SelectedManifestPass(role))
            {
                return pass->renderState;
            }
        }
        return m_renderState;
    }

    bool LitEffect::HasTessellation() const noexcept
    {
        // 現在のManifestシェーダー
        if (const auto* const program = ActiveManifestProgram(m_depthOnly))
        {
            return program->HullShader() != nullptr
                && program->DomainShader() != nullptr;
        }
        return m_hullShader != nullptr && m_domainShader != nullptr;
    }

    bool LitEffect::HasGeometryShader() const noexcept
    {
        // 現在のManifestシェーダー
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
        // 今回使う描画コンテキスト
        auto* context = deviceContext != nullptr
            ? deviceContext
            : m_context;
        // 今回使うManifestプログラム
        const auto* const manifestProgram =
            ActiveManifestProgram(m_depthOnly);
        // 今回使う頂点シェーダー
        auto* const activeVertexShader = manifestProgram != nullptr
            ? manifestProgram->VertexShader()
            : m_instancingEnabled
                ? m_instancedVertexShader.Get()
                : m_vertexShader.Get();
        // 今回使うピクセルシェーダー
        auto* const activePixelShader = manifestProgram != nullptr
            ? manifestProgram->PixelShader()
            : m_pixelShader.Get();
        // 今回使うハルシェーダー
        auto* const activeHullShader = manifestProgram != nullptr
            ? manifestProgram->HullShader()
            : m_hullShader.Get();
        // 今回使うドメインシェーダー
        auto* const activeDomainShader = manifestProgram != nullptr
            ? manifestProgram->DomainShader()
            : m_domainShader.Get();
        // 今回使うジオメトリシェーダー
        auto* const activeGeometryShader = manifestProgram != nullptr
            ? manifestProgram->GeometryShader()
            : m_geometryShader.Get();
        // パッチ用処理を設定するか
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
        // 深度専用パスではPSと照明を省き、頂点の変形に必要な定数だけ設定する。
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
                // 頂点処理用の定数参照
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
                // 頂点処理用の定数参照
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
            // 影も本体と同じ形にするため、パッチ描画やGSを使う深度パスには変形用の追加定数も設定する。
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
                // 変形処理用の物体定数参照
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
                // b3の追加ベクトル定数参照
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
            // 影も本体と同じ変形になるよう、深度パスのGSを設定する。
            if (activeGeometryShader != nullptr)
            {
                // GS用の物体定数参照
                ID3D11Buffer* geometryBuffers[]{
                    m_objectBuffer.Get()
                };
                context->GSSetConstantBuffers(
                    0,
                    1,
                    geometryBuffers);
                // b3の追加ベクトル定数参照
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
            // 頂点処理用の定数参照
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
            // 頂点処理用の定数参照
            ID3D11Buffer* vertexBuffers[]{
                m_objectBuffer.Get()
            };
            context->VSSetConstantBuffers(
                0,
                1,
                vertexBuffers);
        }

        // ピクセル処理用の定数参照
        ID3D11Buffer* pixelBuffers[]{
            m_objectBuffer.Get(),
            m_lightingBuffer.Get()
        };
        context->PSSetConstantBuffers(
            0,
            2,
            pixelBuffers);
        // b3の追加ベクトル定数参照
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
        // 変形処理用の物体定数参照
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
        // 三角形リストにハル処理を設定しないよう、明示したパッチ描画の場合だけ使う。
        context->HSSetShader(
            bindTessellation ? activeHullShader : nullptr,
            nullptr,
            0);
        context->DSSetShader(
            bindTessellation ? activeDomainShader : nullptr,
            nullptr,
            0);

        context->GSSetShader(
            activeGeometryShader,
            nullptr,
            0);
        context->PSSetShader(activePixelShader, nullptr, 0);
        BindMaterialAndShadowTextures(context);
        BindPbrTextures(context);

        {
            // t16～t18のクラスタ参照
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

        {
            // t19・t20の副プローブ参照
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

        {
            // t21・t22のSSR参照
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

        {
            // t23～t25の間接光参照
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
        // 未指定の追加画像は、シェーダーが分岐せず読めるよう白を使う。
        // t7以降へ渡す追加画像の参照
        std::array<
            ID3D11ShaderResourceView*,
            LitMaterial::CustomTextureCount>
            customTextures{};
        // 追加テクスチャの枠番号
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
        // 今回使うサンプラーの参照
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
        // 今回使う描画コンテキスト
        auto* context = deviceContext != nullptr
            ? deviceContext
            : m_context;
        if (m_manifestEffect)
        {
            // 変更前の役割強制指定
            const bool previousOverride = m_manifestRoleOverride;
            // 変更前の強制描画役割
            const auto previousRole = m_manifestOverrideRole;
            // 変更前の深度専用指定
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

        // 頂点処理用の定数参照
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
        // ピクセル処理用の定数参照
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
        // 直書きHLSLの輪郭VSはGS入力との一致を保証しないため、GSを解除する。
        context->GSSetShader(nullptr, nullptr, 0);
        context->PSSetShader(
            m_outlinePixelShader.Get(),
            nullptr,
            0);
        // ピクセル処理用の画像参照
        ID3D11ShaderResourceView* textures[]{
            m_albedoTexture
        };
        context->PSSetShaderResources(0, 1, textures);
        // 今回使うサンプラーの参照
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
        // 今回使う描画コンテキスト
        auto* context = deviceContext != nullptr
            ? deviceContext
            : m_context;
        if (m_manifestEffect)
        {
            // 静的・スキニング共通の遮蔽PSを使うため、通常パスの先頭で頂点を処理してPSだけ切り替える。
            // 通常パスの役割
            const auto primaryRole = PrimaryRole();
            // 通常役割の配列番号
            const auto primaryRoleIndex = RoleIndex(primaryRole);
            // 変更前の通常パス番号
            const auto previousPrimaryIndex =
                m_selectedManifestPasses[primaryRoleIndex];
            // 変更前のインスタンス指定
            const bool previousInstancing = m_instancingEnabled;
            // 変更前の深度専用指定
            const bool previousDepthOnly = m_depthOnly;
            // 変更前の役割強制指定
            const bool previousOverride = m_manifestRoleOverride;
            // 変更前の強制描画役割
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
            // 選択した遮蔽パス
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
        // 今回使う描画コンテキスト
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

        // ピクセル処理用の定数参照
        ID3D11Buffer* pixelBuffers[]{
            m_objectBuffer.Get(),
            m_lightingBuffer.Get()
        };
        context->PSSetConstantBuffers(
            0,
            static_cast<UINT>(std::size(pixelBuffers)),
            pixelBuffers);
        // DirectXTKの頂点出力を使う経路では、入力の一致を保証しないHS・DS・GSを解除する。
        context->HSSetShader(nullptr, nullptr, 0);
        context->DSSetShader(nullptr, nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0);
        // 通常パスの先頭シェーダー
        const auto* const primary = SelectedManifestPass(
            PrimaryRole());
        context->PSSetShader(
            primary != nullptr
                ? primary->program.PixelShader()
                : m_pixelShader.Get(),
            nullptr,
            0);

        BindMaterialAndShadowTextures(context);
        BindPbrTextures(context);
        // 今回使うサンプラーの参照
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

        // ピクセル処理用の画像参照
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

        // 未指定マップ用の白画像
        auto* const white = m_whiteTexture.Get();

        // ピクセル処理用の画像参照
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
        // 平坦法線以外の画像があるか
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
        // 借用する頂点バイトコード
        ID3DBlob* byteCode = m_vertexShaderByteCode.Get();
        if (m_manifestEffect)
        {
            // 現在のManifestシェーダー
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
