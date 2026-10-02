#include "LamaPon/Graphics/ScreenEffect.h"
#include "LamaPon/Graphics/ShaderManifest.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/PathUtils.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
    // 失敗したHRESULTを例外として伝える(result: 操作結果, operation: 診断に表示する操作名)。
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
    ScreenEffect::ScreenEffect(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        AssetManager& assets,
        const std::filesystem::path& shaderPath)
        : m_context(context)
    {
        if (device == nullptr || context == nullptr)
        {
            throw std::invalid_argument(
                "ScreenEffect requires a Direct3D device and context.");
        }

        // コンパイルするHLSLのパス
        std::filesystem::path hlslPath = shaderPath;
        // 適用する先頭の描画パス
        ShaderPassDesc pass;
        if (IsShaderManifestPath(shaderPath))
        {
            // 画面効果のマニフェスト
            ShaderAssetDesc asset;
            // マニフェスト読込の診断
            std::string manifestError;
            if (!LoadShaderAssetDesc(
                    assets,
                    shaderPath,
                    asset,
                    manifestError))
            {
                throw std::runtime_error(manifestError);
            }
            if (asset.type != ShaderAssetType::ScreenEffect)
            {
                throw std::runtime_error(
                    "ScreenEffect requires a shader manifest"
                    " whose type is 'screenEffect': "
                    + PathToUtf8(shaderPath));
            }

            hlslPath = asset.source;
            pass = asset.passes.front();
        }
        else
        {

            // 直接指定の頂点ステージ宣言
            ShaderStageDesc vertex;
            vertex.stage = ShaderStage::Vertex;
            vertex.entryPoint = "VSMain";
            vertex.target = "vs_5_0";
            pass.stages.emplace_back(std::move(vertex));

            // 直接指定の画素ステージ宣言
            ShaderStageDesc pixel;
            pixel.stage = ShaderStage::Pixel;
            pixel.entryPoint = "PSMain";
            pixel.target = "ps_5_0";
            pass.stages.emplace_back(std::move(pixel));
        }

        // シェーダーコンパイルの診断
        std::string programError;
        if (!m_program.Compile(
                device,
                assets,
                hlslPath,
                pass,
                programError))
        {
            throw std::runtime_error(programError);
        }
        if (m_program.VertexShader() == nullptr
            || m_program.PixelShader() == nullptr)
        {
            throw std::runtime_error(
                "ScreenEffect requires vertex and pixel shader stages.");
        }

        // 定数バッファーの作成設定
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
            "ID3D11Device::CreateBuffer(screen effect)");

        // 線形リピートのサンプラー設定
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        // ノイズを繰り返し、シーン画像のUV制限はHLSL側で行う。
        sampler.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler.MaxLOD = std::numeric_limits<float>::max();
        ThrowIfFailed(
            device->CreateSamplerState(
                &sampler,
                m_sampler.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateSamplerState(screen effect)");

        // 深度を無効化する状態設定
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = FALSE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        ThrowIfFailed(
            device->CreateDepthStencilState(
                &depth,
                m_depthDisabled.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateDepthStencilState(screen effect)");

        // カリングしない状態の設定
        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = TRUE;
        ThrowIfFailed(
            device->CreateRasterizerState(
                &rasterizer,
                m_rasterizer.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateRasterizerState(screen effect)");
    }

    void ScreenEffect::Apply(
        ID3D11ShaderResourceView* source,
        const std::array<ID3D11ShaderResourceView*, 2>&
            auxiliaryTextures,
        ID3D11ShaderResourceView* depth,
        const DirectX::XMFLOAT4& depthParameters,
        const DirectX::XMFLOAT4& depthUnprojection,
        ID3D11RenderTargetView* destination,
        const std::uint32_t width,
        const std::uint32_t height,
        const CustomParameters& parameters)
    {
        if (source == nullptr || destination == nullptr)
        {
            return;
        }

        // GPUへ送る画面効果の定数
        Constants constants{};
        constants.parameters = parameters;
        constants.screenSize = {
            static_cast<float>(std::max(width, 1u)),
            static_cast<float>(std::max(height, 1u)),
            1.0f / static_cast<float>(std::max(width, 1u)),
            1.0f / static_cast<float>(std::max(height, 1u))
        };
        // 深度不在時は有効印をゼロにして深度読込を抑制する。
        constants.depthParameters = depth != nullptr
            ? depthParameters
            : DirectX::XMFLOAT4{ 0.0f, 0.0f, 0.0f, 0.0f };
        constants.depthUnprojection = depthUnprojection;
        m_context->UpdateSubresource(
            m_constantBuffer.Get(),
            0,
            nullptr,
            &constants,
            0,
            0);

        // 出力先の描画ターゲット
        ID3D11RenderTargetView* targets[]{ destination };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        // 出力サイズのビューポート
        const D3D11_VIEWPORT viewport{
            0.0f,
            0.0f,
            static_cast<float>(std::max(width, 1u)),
            static_cast<float>(std::max(height, 1u)),
            0.0f,
            1.0f
        };
        m_context->RSSetViewports(1, &viewport);
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(
            m_program.VertexShader(), nullptr, 0);
        m_context->PSSetShader(
            m_program.PixelShader(), nullptr, 0);
        // VS・PSのb0に結合する定数
        ID3D11Buffer* buffers[]{
            m_constantBuffer.Get()
        };
        m_context->VSSetConstantBuffers(0, 1, buffers);
        m_context->PSSetConstantBuffers(0, 1, buffers);
        // 元画像・補助画像・深度の入力
        ID3D11ShaderResourceView* resources[]{
            source,
            auxiliaryTextures[0],
            auxiliaryTextures[1],
            depth
        };
        m_context->PSSetShaderResources(0, 4, resources);
        // PSのs0に結合するサンプラー
        ID3D11SamplerState* samplers[]{
            m_sampler.Get()
        };
        m_context->PSSetSamplers(0, 1, samplers);
        // 合成状態に渡すゼロ係数
        constexpr float blendFactor[4]{};
        m_context->OMSetBlendState(
            nullptr,
            blendFactor,
            0xffffffffu);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(),
            0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);

        // 深度をDSVへ再結合できるよう、入力SRVの結合を解除する。
        // SRVの結合を解除するヌル配列
        ID3D11ShaderResourceView* nullResources[]{
            nullptr,
            nullptr,
            nullptr,
            nullptr
        };
        m_context->PSSetShaderResources(
            0,
            4,
            nullResources);
    }
}
