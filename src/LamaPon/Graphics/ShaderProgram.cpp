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

        // 完成するまで現在のProgramを変更しません。hot reload中に必須
        // stageが失敗しても、呼び出し側が直前の正常なProgramを維持できます。
        Microsoft::WRL::ComPtr<ID3D11VertexShader>
            vertexShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            pixelShader;
        Microsoft::WRL::ComPtr<ID3D11GeometryShader>
            geometryShader;
        Microsoft::WRL::ComPtr<ID3D11HullShader>
            hullShader;
        Microsoft::WRL::ComPtr<ID3D11DomainShader>
            domainShader;
        Microsoft::WRL::ComPtr<ID3D11ComputeShader>
            computeShader;
        Microsoft::WRL::ComPtr<ID3DBlob>
            vertexShaderByteCode;

        for (const auto& stage : pass.stages)
        {
            try
            {
                const auto byteCode = CompileShaderCached(
                    assets,
                    hlslPath,
                    stage.entryPoint.c_str(),
                    stage.target.c_str(),
                    defines);
                const auto* data = byteCode->GetBufferPointer();
                const auto size = byteCode->GetBufferSize();

                switch (stage.stage)
                {
                case ShaderStage::Vertex:
                {
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
                    Microsoft::WRL::ComPtr<ID3D11ShaderReflection>
                        reflection;
                    ThrowIfFailed(
                        D3DReflect(
                            data,
                            size,
                            IID_ID3D11ShaderReflection,
                            &reflection),
                        "D3DReflect(geometry shader)");
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
