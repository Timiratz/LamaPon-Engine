#include "LamaPon/Graphics/ShaderProgram.h"

#include "LamaPon/Graphics/ShaderCompiler.h"
#include "LamaPon/Graphics/ShaderManifest.h"

#include <d3d11shader.h>
#include <d3dcompiler.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
    // ステージ種別の診断用名称を返す(stage: ステージ種別)。
    [[nodiscard]] const char* StageName(
        const LamaPon::ShaderStage stage) noexcept
    {
        switch (stage)
        {
        case LamaPon::ShaderStage::Vertex:
            return "vertex";
        case LamaPon::ShaderStage::Pixel:
            return "pixel";
        case LamaPon::ShaderStage::Geometry:
            return "geometry";
        case LamaPon::ShaderStage::Hull:
            return "hull";
        case LamaPon::ShaderStage::Domain:
            return "domain";
        case LamaPon::ShaderStage::Compute:
            return "compute";
        }
        return "unknown";
    }

    // 失敗結果を操作名付きの例外へ変換する(result: HRESULTの処理結果, operation: 診断用の操作名)。
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
                    static_cast<std::uint32_t>(result)));
        }
    }
}

namespace LamaPon
{
    bool ShaderProgram::Compile(
        ID3D11Device* const device,
        AssetManager& assets,
        const std::filesystem::path& hlslPath,
        const ShaderPassDesc& pass,
        std::string& error,
        const std::vector<std::string>& defines)
    {
        error.clear();
        if (device == nullptr)
        {
            error = "ShaderProgram requires a Direct3D device.";
            return false;
        }

        // 全ステージの処理が終わるまで旧プログラムを保持する。
        // 新しく生成する頂点シェーダー
        Microsoft::WRL::ComPtr<ID3D11VertexShader>
            vertexShader;
        // 新しく生成するピクセルシェーダー
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            pixelShader;
        // 新しく生成するジオメトリシェーダー
        Microsoft::WRL::ComPtr<ID3D11GeometryShader>
            geometryShader;
        // 新しく生成するハルシェーダー
        Microsoft::WRL::ComPtr<ID3D11HullShader>
            hullShader;
        // 新しく生成するドメインシェーダー
        Microsoft::WRL::ComPtr<ID3D11DomainShader>
            domainShader;
        // 新しく生成するcomputeシェーダー
        Microsoft::WRL::ComPtr<ID3D11ComputeShader>
            computeShader;
        // 新しく生成する頂点シェーダーのバイト列
        Microsoft::WRL::ComPtr<ID3DBlob>
            vertexShaderByteCode;

        // コンパイルするステージ記述
        for (const auto& stage : pass.stages)
        {
            try
            {
                // コンパイル済みのバイトコード
                const auto byteCode = CompileShaderCached(
                    assets,
                    hlslPath,
                    stage.entryPoint.c_str(),
                    stage.target.c_str(),
                    defines);
                // 借用するシェーダーのバイト列
                const auto* data = byteCode->GetBufferPointer();
                // シェーダーのバイト数
                const auto size = byteCode->GetBufferSize();

                switch (stage.stage)
                {
                case ShaderStage::Vertex:
                {
                    // 作成中の頂点シェーダー
                    Microsoft::WRL::ComPtr<ID3D11VertexShader>
                        shader;
                    ThrowIfFailed(
                        device->CreateVertexShader(
                            data,
                            size,
                            nullptr,
                            shader.ReleaseAndGetAddressOf()),
                        "ID3D11Device::CreateVertexShader");
                    vertexShader = std::move(shader);
                    vertexShaderByteCode = byteCode;
                    break;
                }
                case ShaderStage::Pixel:
                {
                    // 作成中のピクセルシェーダー
                    Microsoft::WRL::ComPtr<ID3D11PixelShader>
                        shader;
                    ThrowIfFailed(
                        device->CreatePixelShader(
                            data,
                            size,
                            nullptr,
                            shader.ReleaseAndGetAddressOf()),
                        "ID3D11Device::CreatePixelShader");
                    pixelShader = std::move(shader);
                    break;
                }
                case ShaderStage::Geometry:
                {
                    // 入力形式を調べるリフレクション
                    Microsoft::WRL::ComPtr<ID3D11ShaderReflection>
                        reflection;
                    ThrowIfFailed(
                        D3DReflect(
                            data,
                            size,
                            IID_ID3D11ShaderReflection,
                            &reflection),
                        "D3DReflect(geometry shader)");
                    // ジオメトリシェーダーの構成
                    D3D11_SHADER_DESC shaderDescription{};
                    ThrowIfFailed(
                        reflection->GetDesc(&shaderDescription),
                        "ID3D11ShaderReflection::GetDesc(geometry shader)");
                    if (shaderDescription.InputPrimitive
                        != D3D_PRIMITIVE_TRIANGLE)
                    {
                        throw std::invalid_argument(
                            "Geometry shader entry '"
                            + stage.entryPoint
                            + "' must take triangle input; LamaPon material "
                            "renderers do not submit point or line input.");
                    }
                    // 作成中のジオメトリシェーダー
                    Microsoft::WRL::ComPtr<ID3D11GeometryShader>
                        shader;
                    ThrowIfFailed(
                        device->CreateGeometryShader(
                            data,
                            size,
                            nullptr,
                            shader.ReleaseAndGetAddressOf()),
                        "ID3D11Device::CreateGeometryShader");
                    geometryShader = std::move(shader);
                    break;
                }
                case ShaderStage::Hull:
                {
                    // 作成中のハルシェーダー
                    Microsoft::WRL::ComPtr<ID3D11HullShader>
                        shader;
                    ThrowIfFailed(
                        device->CreateHullShader(
                            data,
                            size,
                            nullptr,
                            shader.ReleaseAndGetAddressOf()),
                        "ID3D11Device::CreateHullShader");
                    hullShader = std::move(shader);
                    break;
                }
                case ShaderStage::Domain:
                {
                    // 作成中のドメインシェーダー
                    Microsoft::WRL::ComPtr<ID3D11DomainShader>
                        shader;
                    ThrowIfFailed(
                        device->CreateDomainShader(
                            data,
                            size,
                            nullptr,
                            shader.ReleaseAndGetAddressOf()),
                        "ID3D11Device::CreateDomainShader");
                    domainShader = std::move(shader);
                    break;
                }
                case ShaderStage::Compute:
                {
                    // 作成中のcomputeシェーダー
                    Microsoft::WRL::ComPtr<ID3D11ComputeShader>
                        shader;
                    ThrowIfFailed(
                        device->CreateComputeShader(
                            data,
                            size,
                            nullptr,
                            shader.ReleaseAndGetAddressOf()),
                        "ID3D11Device::CreateComputeShader");
                    computeShader = std::move(shader);
                    break;
                }
                }
            }
            // ステージ生成で発生した例外
            catch (const std::exception& exception)
            {
                if (stage.optional)
                {
                    continue;
                }
                error = "Failed to build required ";
                error += StageName(stage.stage);
                error += " shader stage '";
                error += stage.entryPoint;
                error += "' (";
                error += stage.target;
                error += "): ";
                error += exception.what();
                return false;
            }
        }

        m_vertexShader = std::move(vertexShader);
        m_pixelShader = std::move(pixelShader);
        m_geometryShader = std::move(geometryShader);
        m_hullShader = std::move(hullShader);
        m_domainShader = std::move(domainShader);
        m_computeShader = std::move(computeShader);
        m_vertexShaderByteCode =
            std::move(vertexShaderByteCode);
        return true;
    }
}
