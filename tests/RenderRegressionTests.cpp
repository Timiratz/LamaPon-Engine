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

    // 描画比較画像の幅
    constexpr std::uint32_t Width = 8;
    // 描画比較画像の高さ
    constexpr std::uint32_t Height = 8;

    // Require(condition: 成立条件, message: 失敗理由): 条件不成立を検査失敗にする。
    void Require(
        const bool condition,
        const char* message)
    {
        // 検査条件の不成立を検出する。
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // ThrowIfFailed(result: HRESULT, operation: 実行処理): 失敗コードを例外へ変換する。
    void ThrowIfFailed(
        const HRESULT result,
        const char* operation)
    {
        // HRESULTの失敗を検出する。
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

    // CompileShader(source: HLSLソース, entryPoint: 入口関数, target: シェーダー種別): HLSLを最適化してコンパイルする。
    ComPtr<ID3DBlob> CompileShader(
        const char* source,
        const char* entryPoint,
        const char* target)
    {
        // コンパイル済みシェーダー本体
        ComPtr<ID3DBlob> shader;
        // コンパイラー診断
        ComPtr<ID3DBlob> errors;
        // シェーダーコンパイル結果
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
        // コンパイル失敗時に診断を返す。
        if (FAILED(result))
        {
            // コンパイラーが返した診断文
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

    // LoadBaseline(path: 基準画像ファイル): P3形式のRGBA画素列を読み込む。
    std::vector<std::uint8_t> LoadBaseline(
        const std::filesystem::path& path)
    {
        // 読み込む基準画像
        std::ifstream input(path, std::ios::binary);
        // 基準画像が開けない場合を検出する。
        if (!input)
        {
            throw std::runtime_error(
                "Render baseline was not found.");
        }

        // PPM形式識別子
        std::string magic;
        // 基準画像の幅
        std::uint32_t width{};
        // 基準画像の高さ
        std::uint32_t height{};
        // 基準画素の最大値
        std::uint32_t maximum{};
        input >> magic >> width >> height >> maximum;
        Require(
            magic == "P3"
                && width == Width
                && height == Height
                && maximum == 255,
            "Render baseline has an unexpected format.");

        // 基準画像のRGBA画素列
        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(
                Width * Height * 4));
        // 全画素を基準ファイルから読む。
        for (std::size_t index{};
            index < Width * Height;
            ++index)
        {
            // 基準画素の赤成分
            unsigned red{};
            // 基準画素の緑成分
            unsigned green{};
            // 基準画素の青成分
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

    // WriteActual(path: 出力先, pixels: 描画画素列): 実画像をP3形式で保存する。
    void WriteActual(
        const std::filesystem::path& path,
        const std::vector<std::uint8_t>& pixels)
    {
        std::filesystem::create_directories(
            path.parent_path());
        // 書き込む実画像ファイル
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        output << "P3\n"
               << Width << ' ' << Height
               << "\n255\n";
        // 画像の全行を保存する。
        for (std::uint32_t y{};
            y < Height;
            ++y)
        {
            // 行内の全画素を保存する。
            for (std::uint32_t x{};
                x < Width;
                ++x)
            {
                // 現在画素の先頭オフセット
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

// wmain(argumentCount: 引数個数, arguments: コマンド行引数): WARP描画を基準画像と比較する。
int wmain(
    const int argumentCount,
    wchar_t** arguments)
{
    // 検査失敗を終了コードへ変換する。
    try
    {
        // 必須引数が揃わない場合を検出する。
        if (argumentCount != 3)
        {
            throw std::invalid_argument(
                "Usage: LamaPonRenderRegressionTests "
                "<baseline.ppm> <actual.ppm>");
        }

        // WARP描画用デバイス
        ComPtr<ID3D11Device> device;
        // 描画コマンド用コンテキスト
        ComPtr<ID3D11DeviceContext> context;
        // 作成された機能レベル
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

        // 三角形頂点を生成するHLSLソース
        constexpr char VertexShaderSource[] = R"(
// Main(vertexId: 頂点番号): 三角形の頂点位置を返す。
float4 Main(uint vertexId : SV_VertexID) : SV_POSITION
{
    // 画面全体を覆う三角形の頂点
    const float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };
    return float4(positions[vertexId], 0.0, 1.0);
}
)";
        // 定数色を出力するピクセルシェーダー
        constexpr char PixelShaderSource[] = R"(
// Main(): 各ピクセルへ比較用の固定色を出力する。
float4 Main() : SV_TARGET
{
    return float4(
        64.0 / 255.0,
        128.0 / 255.0,
        192.0 / 255.0,
        1.0);
}
)";
        // コンパイル済み頂点シェーダー
        const auto vertexByteCode =
            CompileShader(
                VertexShaderSource,
                "Main",
                "vs_5_0");
        // コンパイル済みピクセルシェーダー
        const auto pixelByteCode =
            CompileShader(
                PixelShaderSource,
                "Main",
                "ps_5_0");

        // 描画用頂点シェーダー
        ComPtr<ID3D11VertexShader> vertexShader;
        // 描画用ピクセルシェーダー
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

        // 描画先テクスチャの設定
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

        // GPU描画先テクスチャ
        ComPtr<ID3D11Texture2D> targetTexture;
        ThrowIfFailed(
            device->CreateTexture2D(
                &textureDescription,
                nullptr,
                targetTexture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(target)");
        // 描画先テクスチャのビュー
        ComPtr<ID3D11RenderTargetView> targetView;
        ThrowIfFailed(
            device->CreateRenderTargetView(
                targetTexture.Get(),
                nullptr,
                targetView.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateRenderTargetView");

        // ラスタライザー設定
        D3D11_RASTERIZER_DESC rasterizerDescription{};
        rasterizerDescription.FillMode =
            D3D11_FILL_SOLID;
        rasterizerDescription.CullMode =
            D3D11_CULL_NONE;
        rasterizerDescription.DepthClipEnable = true;
        // カリングなしのラスタライザー状態
        ComPtr<ID3D11RasterizerState> rasterizer;
        ThrowIfFailed(
            device->CreateRasterizerState(
                &rasterizerDescription,
                rasterizer.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateRasterizerState");

        // 出力マージャーへ渡す描画先
        ID3D11RenderTargetView* targets[]{
            targetView.Get()
        };
        context->OMSetRenderTargets(
            1,
            targets,
            nullptr);
        // 8x8描画用のビューポート
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
        // 画素比較で使う初期クリア色
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
        // CPU読み取り用のステージング画像
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

        // ステージング画像のCPUマップ結果
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ThrowIfFailed(
            context->Map(
                stagingTexture.Get(),
                0,
                D3D11_MAP_READ,
                0,
                &mapped),
            "ID3D11DeviceContext::Map");
        // GPUから読み戻したRGBA画素列
        std::vector<std::uint8_t> actual(
            static_cast<std::size_t>(
                Width * Height * 4));
        // GPU画像の全行を連続バッファーへコピーする。
        for (std::uint32_t y{};
            y < Height;
            ++y)
        {
            // GPU画像内の現在行
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

        // 基準画像のRGBA画素列
        const auto expected =
            LoadBaseline(arguments[1]);
        // 許容差を超えたチャンネル数
        std::size_t differingChannels{};
        // 観測した最大のチャンネル差
        unsigned maximumDifference{};
        // 基準と描画の全チャンネルを比較する。
        for (std::size_t index{};
            index < actual.size();
            ++index)
        {
            // 現在チャンネルの絶対差
            const unsigned difference =
                static_cast<unsigned>(
                    std::abs(
                        static_cast<int>(actual[index])
                        - static_cast<int>(
                            expected[index])));
            // 許容差を超えるチャンネルを数える。
            if (difference > 1)
            {
                ++differingChannels;
            }
            maximumDifference =
                std::max(maximumDifference, difference);
        }
        // 差分があれば実画像を保存して診断する。
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
    // 例外(exception: 描画検査失敗)を標準エラーへ出力する。
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
