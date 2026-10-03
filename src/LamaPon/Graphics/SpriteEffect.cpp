#include "LamaPon/Graphics/SpriteEffect.h"
#include "LamaPon/Graphics/ShaderCompiler.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/PathUtils.h"

#include <d3dcompiler.h>

#include <cstdint>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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

    // スプライトのPSMainを取得し、失敗時は例外を送出する(assets: アセット管理, path: HLSLソースパス)。
    Microsoft::WRL::ComPtr<ID3DBlob> CompilePixelShader(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& path)
    {
        if (!assets.FileExists(path) && !assets.IsArchived())
        {
            throw std::runtime_error(
                "Sprite shader file was not found: "
                + LamaPon::PathToUtf8(path));
        }

        return LamaPon::CompileShaderCached(
            assets,
            path,
            "PSMain",
            "ps_5_0");
    }
}

namespace LamaPon
{
    SpriteEffect::SpriteEffect(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        AssetManager& assets,
        const std::filesystem::path& shaderPath)
        : m_context(context)
    {
        if (device == nullptr || context == nullptr)
        {
            throw std::invalid_argument(
                "SpriteEffect requires a Direct3D device and context.");
        }

        // PSMainのバイトコード
        const auto byteCode =
            CompilePixelShader(assets, shaderPath);
        ThrowIfFailed(
            device->CreatePixelShader(
                byteCode->GetBufferPointer(),
                byteCode->GetBufferSize(),
                nullptr,
                m_pixelShader.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(sprite)");

        // 追加定数バッファーの作成設定
        D3D11_BUFFER_DESC description{};
        description.ByteWidth =
            static_cast<UINT>(sizeof(Constants));
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags =
            D3D11_BIND_CONSTANT_BUFFER;
        ThrowIfFailed(
            device->CreateBuffer(
                &description,
                nullptr,
                m_constantBuffer.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateBuffer(sprite constants)");

        // 2D照明バッファーの作成設定
        D3D11_BUFFER_DESC lightDescription{};
        lightDescription.ByteWidth =
            static_cast<UINT>(sizeof(Sprite2DLighting));
        lightDescription.Usage = D3D11_USAGE_DEFAULT;
        lightDescription.BindFlags =
            D3D11_BIND_CONSTANT_BUFFER;
        ThrowIfFailed(
            device->CreateBuffer(
                &lightDescription,
                nullptr,
                m_lightBuffer.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateBuffer(sprite lights)");
    }

    void SpriteEffect::SetParameters(
        const CustomParameters& parameters) noexcept
    {
        m_constants.parameters = parameters;
    }

    void SpriteEffect::SetLights(
        const Sprite2DLighting& lighting) noexcept
    {
        m_lighting = lighting;
    }

    void SpriteEffect::Apply()
    {
        m_context->UpdateSubresource(
            m_constantBuffer.Get(),
            0,
            nullptr,
            &m_constants,
            0,
            0);
        m_context->UpdateSubresource(
            m_lightBuffer.Get(),
            0,
            nullptr,
            &m_lighting,
            0,
            0);
        // PSのb0・b1へ結合する定数
        ID3D11Buffer* buffers[]{
            m_constantBuffer.Get(),
            m_lightBuffer.Get()
        };
        m_context->PSSetConstantBuffers(
            0,
            static_cast<UINT>(std::size(buffers)),
            buffers);
        m_context->PSSetShader(
            m_pixelShader.Get(),
            nullptr,
            0);
    }
}
