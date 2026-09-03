#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    using Microsoft::WRL::ComPtr;

    constexpr std::uint32_t Width = 8;
    constexpr std::uint32_t Height = 8;

    void Require(
        const bool condition,
        const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    void ThrowIfFailed(
        const HRESULT result,
        const char* operation)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(
                std::string(operation)
                + " failed with HRESULT "
                + std::to_string(
                    static_cast<unsigned long>(
                        result)));
        }
    }

    ComPtr<ID3DBlob> CompileShader(
        const char* source,
        const char* entryPoint,
        const char* target)
    {
        ComPtr<ID3DBlob> shader;
        ComPtr<ID3DBlob> errors;
        const HRESULT result = D3DCompile(
            source,
            std::char_traits<char>::length(source),
            "RenderRegression",
            nullptr,
            nullptr,
            entryPoint,
            target,
            D3DCOMPILE_ENABLE_STRICTNESS
                | D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0,
            shader.ReleaseAndGetAddressOf(),
            errors.ReleaseAndGetAddressOf());
        if (FAILED(result))
        {
            const std::string message =
                errors
                    ? std::string(
                        static_cast<const char*>(
                            errors->GetBufferPointer()),
                        errors->GetBufferSize())
                    : "Unknown shader compiler error.";
            throw std::runtime_error(message);
        }
        return shader;
    }

    std::vector<std::uint8_t> LoadBaseline(
        const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error(
                "Render baseline was not found.");
        }

        std::string magic;
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint32_t maximum{};
        input >> magic >> width >> height >> maximum;
        Require(
            magic == "P3"
                && width == Width
                && height == Height
                && maximum == 255,
            "Render baseline has an unexpected format.");

        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(
                Width * Height * 4));
        for (std::size_t index{};
            index < Width * Height;
            ++index)
        {
            unsigned red{};
            unsigned green{};
            unsigned blue{};
            input >> red >> green >> blue;
            Require(
                input.good()
                    && red <= 255
                    && green <= 255
                    && blue <= 255,
                "Render baseline is truncated.");
            pixels[index * 4] =
                static_cast<std::uint8_t>(red);
            pixels[index * 4 + 1] =
                static_cast<std::uint8_t>(green);
            pixels[index * 4 + 2] =
                static_cast<std::uint8_t>(blue);
            pixels[index * 4 + 3] = 255;
        }
        return pixels;
    }

    void WriteActual(
        const std::filesystem::path& path,
        const std::vector<std::uint8_t>& pixels)
    {
        std::filesystem::create_directories(
            path.parent_path());
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        output << "P3\n"
               << Width << ' ' << Height
               << "\n255\n";
        for (std::uint32_t y{};
            y < Height;
            ++y)
        {
            for (std::uint32_t x{};
                x < Width;
                ++x)
            {
                const auto index =
                    static_cast<std::size_t>(
                        (y * Width + x) * 4);
                output
                    << static_cast<unsigned>(
                        pixels[index]) << ' '
                    << static_cast<unsigned>(
                        pixels[index + 1]) << ' '
                    << static_cast<unsigned>(
                        pixels[index + 2]) << ' ';
            }
            output << '\n';
        }
    }
}

int wmain(
    const int argumentCount,
    wchar_t** arguments)
{
    try
    {
        if (argumentCount != 3)
        {
            throw std::invalid_argument(
                "Usage: LamaPonRenderRegressionTests "
                "<baseline.ppm> <actual.ppm>");
        }

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL featureLevel{};
        ThrowIfFailed(
            D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_WARP,
                nullptr,
                0,
                nullptr,
                0,
                D3D11_SDK_VERSION,
                device.ReleaseAndGetAddressOf(),
                &featureLevel,
                context.ReleaseAndGetAddressOf()),
            "D3D11CreateDevice(WARP)");
        Require(
            featureLevel >= D3D_FEATURE_LEVEL_11_0,
            "The WARP device does not support feature level 11.");

        constexpr char VertexShaderSource[] = R"(
float4 Main(uint vertexId : SV_VertexID) : SV_POSITION
{
    const float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };
    return float4(positions[vertexId], 0.0, 1.0);
}
)";
        constexpr char PixelShaderSource[] = R"(
float4 Main() : SV_TARGET
{
    return float4(
        64.0 / 255.0,
        128.0 / 255.0,
        192.0 / 255.0,
        1.0);
}
)";
        const auto vertexByteCode =
            CompileShader(
                VertexShaderSource,
                "Main",
                "vs_5_0");
        const auto pixelByteCode =
            CompileShader(
                PixelShaderSource,
                "Main",
                "ps_5_0");

        ComPtr<ID3D11VertexShader> vertexShader;
        ComPtr<ID3D11PixelShader> pixelShader;
        ThrowIfFailed(
            device->CreateVertexShader(
                vertexByteCode->GetBufferPointer(),
                vertexByteCode->GetBufferSize(),
                nullptr,
                vertexShader.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateVertexShader");
        ThrowIfFailed(
            device->CreatePixelShader(
                pixelByteCode->GetBufferPointer(),
                pixelByteCode->GetBufferSize(),
                nullptr,
                pixelShader.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader");

        D3D11_TEXTURE2D_DESC textureDescription{};
        textureDescription.Width = Width;
        textureDescription.Height = Height;
        textureDescription.MipLevels = 1;
        textureDescription.ArraySize = 1;
        textureDescription.Format =
            DXGI_FORMAT_R8G8B8A8_UNORM;
        textureDescription.SampleDesc.Count = 1;
        textureDescription.Usage = D3D11_USAGE_DEFAULT;
        textureDescription.BindFlags =
            D3D11_BIND_RENDER_TARGET;

        ComPtr<ID3D11Texture2D> targetTexture;
        ThrowIfFailed(
            device->CreateTexture2D(
                &textureDescription,
                nullptr,
                targetTexture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(target)");
        ComPtr<ID3D11RenderTargetView> targetView;
        ThrowIfFailed(
            device->CreateRenderTargetView(
                targetTexture.Get(),
                nullptr,
                targetView.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateRenderTargetView");

        D3D11_RASTERIZER_DESC rasterizerDescription{};
        rasterizerDescription.FillMode =
            D3D11_FILL_SOLID;
        rasterizerDescription.CullMode =
            D3D11_CULL_NONE;
        rasterizerDescription.DepthClipEnable = true;
        ComPtr<ID3D11RasterizerState> rasterizer;
        ThrowIfFailed(
            device->CreateRasterizerState(
                &rasterizerDescription,
                rasterizer.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateRasterizerState");

        ID3D11RenderTargetView* targets[]{
            targetView.Get()
        };
        context->OMSetRenderTargets(
            1,
            targets,
            nullptr);
        const D3D11_VIEWPORT viewport{
            0.0f,
            0.0f,
            static_cast<float>(Width),
            static_cast<float>(Height),
            0.0f,
            1.0f
        };
        context->RSSetViewports(1, &viewport);
        context->RSSetState(rasterizer.Get());
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(
            vertexShader.Get(),
            nullptr,
            0);
        context->PSSetShader(
            pixelShader.Get(),
            nullptr,
            0);
        constexpr std::array<float, 4> ClearColor{
            1.0f, 0.0f, 1.0f, 1.0f
        };
        context->ClearRenderTargetView(
            targetView.Get(),
            ClearColor.data());
        context->Draw(3, 0);

        textureDescription.Usage =
            D3D11_USAGE_STAGING;
        textureDescription.BindFlags = 0;
        textureDescription.CPUAccessFlags =
            D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> stagingTexture;
        ThrowIfFailed(
            device->CreateTexture2D(
                &textureDescription,
                nullptr,
                stagingTexture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(staging)");
        context->CopyResource(
            stagingTexture.Get(),
            targetTexture.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        ThrowIfFailed(
            context->Map(
                stagingTexture.Get(),
                0,
                D3D11_MAP_READ,
                0,
                &mapped),
            "ID3D11DeviceContext::Map");
        std::vector<std::uint8_t> actual(
            static_cast<std::size_t>(
                Width * Height * 4));
        for (std::uint32_t y{};
            y < Height;
            ++y)
        {
            const auto* source =
                static_cast<const std::uint8_t*>(
                    mapped.pData)
                + static_cast<std::size_t>(
                    y * mapped.RowPitch);
            std::ranges::copy_n(
                source,
                Width * 4,
                actual.begin()
                    + static_cast<std::ptrdiff_t>(
                        y * Width * 4));
        }
        context->Unmap(stagingTexture.Get(), 0);

        const auto expected =
            LoadBaseline(arguments[1]);
        std::size_t differingChannels{};
        unsigned maximumDifference{};
        for (std::size_t index{};
            index < actual.size();
            ++index)
        {
            const unsigned difference =
                static_cast<unsigned>(
                    std::abs(
                        static_cast<int>(actual[index])
                        - static_cast<int>(
                            expected[index])));
            if (difference > 1)
            {
                ++differingChannels;
            }
            maximumDifference =
                std::max(maximumDifference, difference);
        }
        if (differingChannels != 0)
        {
            WriteActual(arguments[2], actual);
            throw std::runtime_error(
                "Rendered image differs from the baseline in "
                + std::to_string(differingChannels)
                + " channels; maximum difference "
                + std::to_string(maximumDifference)
                + ". Actual image: "
                + std::filesystem::path(arguments[2])
                    .string());
        }

        std::cout
            << "WARP render regression matched "
            << Width << 'x' << Height
            << " baseline.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
