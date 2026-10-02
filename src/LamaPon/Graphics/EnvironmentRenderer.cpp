#include "LamaPon/Graphics/EnvironmentRenderer.h"
#include "LamaPon/Graphics/D3D11Backend.h"
#include "LamaPon/Graphics/D3D11RenderTargetState.h"
#include "LamaPon/Graphics/EnvironmentCache.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/ShaderCompiler.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/PathUtils.h"

#include <DirectXPackedVector.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // 環境畳み込み中に変更する描画状態を、例外時も終了時に復元する。
    class PipelineStateScope final
    {
    public:
        // 畳み込みが変更する状態を保持する(context: 借用する保存・復元先)。
        explicit PipelineStateScope(
            ID3D11DeviceContext* const context) noexcept
            : m_context(context)
        {
            if (m_context == nullptr)
            {
                return;
            }

            // 保存・復元するRTV参照
            std::array<
                ID3D11RenderTargetView*,
                D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rawTargets{};
            m_context->OMGetRenderTargets(
                static_cast<UINT>(rawTargets.size()),
                rawTargets.data(),
                m_depth.ReleaseAndGetAddressOf());
            // 保存・復元する参照の番号
            for (std::size_t index{}; index < rawTargets.size(); ++index)
            {
                m_targets[index].Attach(rawTargets[index]);
            }

            m_viewportCount = static_cast<UINT>(m_viewports.size());
            m_context->RSGetViewports(
                &m_viewportCount,
                m_viewports.data());
            m_context->OMGetDepthStencilState(
                m_depthState.ReleaseAndGetAddressOf(),
                &m_stencilReference);
            m_context->RSGetState(
                m_rasterizer.ReleaseAndGetAddressOf());
            m_context->IAGetInputLayout(
                m_inputLayout.ReleaseAndGetAddressOf());
            m_context->IAGetPrimitiveTopology(&m_topology);

            // 保存・復元するVSクラス参照
            std::array<
                ID3D11ClassInstance*,
                D3D11_SHADER_MAX_INTERFACES> rawVertexInstances{};
            m_vertexInstanceCount =
                static_cast<UINT>(rawVertexInstances.size());
            m_context->VSGetShader(
                m_vertexShader.ReleaseAndGetAddressOf(),
                rawVertexInstances.data(),
                &m_vertexInstanceCount);
            // 保存・復元する参照の番号
            for (UINT index{}; index < m_vertexInstanceCount; ++index)
            {
                m_vertexInstances[index].Attach(
                    rawVertexInstances[index]);
            }

            // 保存・復元するPSクラス参照
            std::array<
                ID3D11ClassInstance*,
                D3D11_SHADER_MAX_INTERFACES> rawPixelInstances{};
            m_pixelInstanceCount =
                static_cast<UINT>(rawPixelInstances.size());
            m_context->PSGetShader(
                m_pixelShader.ReleaseAndGetAddressOf(),
                rawPixelInstances.data(),
                &m_pixelInstanceCount);
            // 保存・復元する参照の番号
            for (UINT index{}; index < m_pixelInstanceCount; ++index)
            {
                m_pixelInstances[index].Attach(
                    rawPixelInstances[index]);
            }

            m_context->PSGetShaderResources(
                1,
                1,
                m_pixelResource.ReleaseAndGetAddressOf());
            m_context->PSGetSamplers(
                0,
                1,
                m_pixelSampler.ReleaseAndGetAddressOf());
            m_context->PSGetConstantBuffers(
                3,
                1,
                m_pixelBuffer.ReleaseAndGetAddressOf());
        }

        // 保持した描画先・状態・VS・PSとt1・s0・b3を復元する。
        ~PipelineStateScope() noexcept
        {
            if (m_context == nullptr)
            {
                return;
            }

            // 保存・復元するRTV参照
            std::array<
                ID3D11RenderTargetView*,
                D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rawTargets{};
            // 保存・復元する参照の番号
            for (std::size_t index{}; index < rawTargets.size(); ++index)
            {
                rawTargets[index] = m_targets[index].Get();
            }
            m_context->OMSetRenderTargets(
                static_cast<UINT>(rawTargets.size()),
                rawTargets.data(),
                m_depth.Get());
            m_context->RSSetViewports(
                m_viewportCount,
                m_viewportCount != 0 ? m_viewports.data() : nullptr);
            m_context->OMSetDepthStencilState(
                m_depthState.Get(),
                m_stencilReference);
            m_context->RSSetState(m_rasterizer.Get());
            m_context->IASetInputLayout(m_inputLayout.Get());
            m_context->IASetPrimitiveTopology(m_topology);

            // 保存・復元するVSクラス参照
            std::array<
                ID3D11ClassInstance*,
                D3D11_SHADER_MAX_INTERFACES> rawVertexInstances{};
            // 保存・復元する参照の番号
            for (UINT index{}; index < m_vertexInstanceCount; ++index)
            {
                rawVertexInstances[index] =
                    m_vertexInstances[index].Get();
            }
            m_context->VSSetShader(
                m_vertexShader.Get(),
                m_vertexInstanceCount != 0
                    ? rawVertexInstances.data()
                    : nullptr,
                m_vertexInstanceCount);

            // 保存・復元するPSクラス参照
            std::array<
                ID3D11ClassInstance*,
                D3D11_SHADER_MAX_INTERFACES> rawPixelInstances{};
            // 保存・復元する参照の番号
            for (UINT index{}; index < m_pixelInstanceCount; ++index)
            {
                rawPixelInstances[index] =
                    m_pixelInstances[index].Get();
            }
            m_context->PSSetShader(
                m_pixelShader.Get(),
                m_pixelInstanceCount != 0
                    ? rawPixelInstances.data()
                    : nullptr,
                m_pixelInstanceCount);

            // 復元するt1の画像参照
            ID3D11ShaderResourceView* resources[]{
                m_pixelResource.Get()
            };
            m_context->PSSetShaderResources(1, 1, resources);
            // 復元するs0のサンプラー
            ID3D11SamplerState* samplers[]{ m_pixelSampler.Get() };
            m_context->PSSetSamplers(0, 1, samplers);
            // 復元するb3の定数参照
            ID3D11Buffer* buffers[]{ m_pixelBuffer.Get() };
            m_context->PSSetConstantBuffers(3, 1, buffers);
        }

        // 状態の復元を二重に行わないようコピーを禁止する。
        PipelineStateScope(const PipelineStateScope&) = delete;
        // 状態の復元を二重に行わないようコピー代入を禁止する。
        PipelineStateScope& operator=(const PipelineStateScope&) = delete;

    private:
        // 借用する状態の保存・復元先
        ID3D11DeviceContext* m_context{};
        // 保存した全RTVの保持参照
        std::array<
            Microsoft::WRL::ComPtr<ID3D11RenderTargetView>,
            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> m_targets;
        // 保存した深度ビュー
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_depth;
        // 保存したビューポート配列
        std::array<
            D3D11_VIEWPORT,
            D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
            m_viewports{};
        // 保存したビューポート数
        UINT m_viewportCount{};
        // 保存した深度・ステンシル状態
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthState;
        // 保存したステンシル参照値
        UINT m_stencilReference{};
        // 保存したラスタライザー状態
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizer;
        // 保存した入力レイアウト
        Microsoft::WRL::ComPtr<ID3D11InputLayout> m_inputLayout;
        // 保存した頂点の接続方式
        D3D11_PRIMITIVE_TOPOLOGY m_topology{
            D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED
        };
        // 保存した頂点シェーダー
        Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
        // 保存したVSクラス参照
        std::array<
            Microsoft::WRL::ComPtr<ID3D11ClassInstance>,
            D3D11_SHADER_MAX_INTERFACES> m_vertexInstances;
        // 保存したVSクラス数
        UINT m_vertexInstanceCount{};
        // 保存したピクセルシェーダー
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;
        // 保存したPSクラス参照
        std::array<
            Microsoft::WRL::ComPtr<ID3D11ClassInstance>,
            D3D11_SHADER_MAX_INTERFACES> m_pixelInstances;
        // 保存したPSクラス数
        UINT m_pixelInstanceCount{};
        // 保存したt1の画像参照
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_pixelResource;
        // 保存したs0のサンプラー
        Microsoft::WRL::ComPtr<ID3D11SamplerState> m_pixelSampler;
        // 保存したb3の定数バッファ
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_pixelBuffer;
    };

    // 失敗したHRESULTを処理名付きの例外へ変換する(result: 処理結果, operation: 処理名)。
    void ThrowIfFailed(const HRESULT result, const char* operation)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(
                std::string(operation)
                + " failed with HRESULT "
                + std::to_string(
                    static_cast<unsigned long>(result)));
        }
    }

    // キャッシュを使ってコンパイルし、失敗なら例外を送出する(assets: 読み込み元, path: HLSLのパス, entryPoint: エントリー名, target: シェーダーモデル)。
    Microsoft::WRL::ComPtr<ID3DBlob> CompileShader(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& path,
        const char* entryPoint,
        const char* target)
    {

        return LamaPon::CompileShaderCached(
            assets,
            path,
            entryPoint,
            target);
    }

    // 16バイト境界に合う型Tの定数バッファを生成する(device: 非空のD3D11機器)。
    template<typename T>
    Microsoft::WRL::ComPtr<ID3D11Buffer>
        CreateConstantBuffer(ID3D11Device* device)
    {
        static_assert(sizeof(T) % 16 == 0);
        // 定数バッファの生成設定
        D3D11_BUFFER_DESC description{};
        description.ByteWidth =
            static_cast<UINT>(sizeof(T));
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags =
            D3D11_BIND_CONSTANT_BUFFER;
        // 生成した定数バッファ
        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
        ThrowIfFailed(
            device->CreateBuffer(
                &description,
                nullptr,
                buffer.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateBuffer(environment)");
        return buffer;
    }

    class ProbeBakeScope final
    {
    public:
        // 再入を拒否してベイク開始を記録する(active: 借用するベイク実行中フラグ)。
        explicit ProbeBakeScope(bool& active)
            : m_active(active)
        {
            if (m_active)
            {
                throw std::logic_error(
                    "Environment probe bake cannot be re-entered.");
            }
            m_active = true;
        }

        // 例外時もベイク実行中フラグを解除する。
        ~ProbeBakeScope()
        {
            m_active = false;
        }

        // ベイク終了解除を二重に行わないようコピーを禁止する。
        ProbeBakeScope(const ProbeBakeScope&) = delete;
        // ベイク終了解除を二重に行わないようコピー代入を禁止する。
        ProbeBakeScope& operator=(const ProbeBakeScope&) = delete;

    private:
        // 借用するベイク実行中フラグ
        bool& m_active;
    };
}

namespace LamaPon
{
    // プローブの6面描画で再利用するHDR・深度・作業画像。
    struct EnvironmentRenderer::ProbeBakeResources final
    {
        // 保持する6面のHDR画像
        Microsoft::WRL::ComPtr<ID3D11Texture2D> cubeTexture;
        // 各キューブ面のRTV
        std::array<
            Microsoft::WRL::ComPtr<ID3D11RenderTargetView>,
            6> faceTargets;
        // キューブ全体のSRV
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            cubeShaderResourceView;
        // 保持する面描画用の深度画像
        Microsoft::WRL::ComPtr<ID3D11Texture2D> depthTexture;
        // 面描画用の深度ビュー
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depthView;
        // カリングを反転させないよう右手系で面を描き、コピー時に左右反転する。
        // 左右反転前のHDR作業画像
        Microsoft::WRL::ComPtr<ID3D11Texture2D> scratchTexture;
        // 面描画用の作業RTV
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> scratchTarget;
        // 反転コピー元の作業SRV
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            scratchShaderResourceView;

        // 再利用するHDR・深度・作業画像を生成する(device: 非空のD3D11機器)。
        explicit ProbeBakeResources(ID3D11Device* const device)
        {
            // HDRキューブの生成設定
            D3D11_TEXTURE2D_DESC cubeDescription{};
            cubeDescription.Width = ProbeBakeFaceSize;
            cubeDescription.Height = ProbeBakeFaceSize;
            cubeDescription.MipLevels = 1;
            cubeDescription.ArraySize = 6;
            cubeDescription.Format =
                DXGI_FORMAT_R16G16B16A16_FLOAT;
            cubeDescription.SampleDesc.Count = 1;
            cubeDescription.Usage = D3D11_USAGE_DEFAULT;
            cubeDescription.BindFlags =
                D3D11_BIND_SHADER_RESOURCE
                | D3D11_BIND_RENDER_TARGET;
            cubeDescription.MiscFlags =
                D3D11_RESOURCE_MISC_TEXTURECUBE;
            ThrowIfFailed(
                device->CreateTexture2D(
                    &cubeDescription,
                    nullptr,
                    cubeTexture.ReleaseAndGetAddressOf()),
                "ID3D11Device::CreateTexture2D(probe cube)");

            // キューブの面番号
            for (std::uint32_t face = 0; face < 6; ++face)
            {
                // 各面のRTV生成設定
                D3D11_RENDER_TARGET_VIEW_DESC targetDescription{};
                targetDescription.Format = cubeDescription.Format;
                targetDescription.ViewDimension =
                    D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                targetDescription.Texture2DArray.FirstArraySlice = face;
                targetDescription.Texture2DArray.ArraySize = 1;
                ThrowIfFailed(
                    device->CreateRenderTargetView(
                        cubeTexture.Get(),
                        &targetDescription,
                        faceTargets[face].ReleaseAndGetAddressOf()),
                    "ID3D11Device::CreateRenderTargetView(probe face)");
            }

            // キューブSRVの生成設定
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
            viewDescription.Format = cubeDescription.Format;
            viewDescription.ViewDimension =
                D3D11_SRV_DIMENSION_TEXTURECUBE;
            viewDescription.TextureCube.MipLevels = 1;
            ThrowIfFailed(
                device->CreateShaderResourceView(
                    cubeTexture.Get(),
                    &viewDescription,
                    cubeShaderResourceView.ReleaseAndGetAddressOf()),
                "ID3D11Device::CreateShaderResourceView(probe cube)");

            // 面描画の深度画像設定
            D3D11_TEXTURE2D_DESC depthDescription{};
            depthDescription.Width = ProbeBakeFaceSize;
            depthDescription.Height = ProbeBakeFaceSize;
            depthDescription.MipLevels = 1;
            depthDescription.ArraySize = 1;
            depthDescription.Format =
                DXGI_FORMAT_D24_UNORM_S8_UINT;
            depthDescription.SampleDesc.Count = 1;
            depthDescription.Usage = D3D11_USAGE_DEFAULT;
            depthDescription.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            ThrowIfFailed(
                device->CreateTexture2D(
                    &depthDescription,
                    nullptr,
                    depthTexture.ReleaseAndGetAddressOf()),
                "ID3D11Device::CreateTexture2D(probe depth)");
            ThrowIfFailed(
                device->CreateDepthStencilView(
                    depthTexture.Get(),
                    nullptr,
                    depthView.ReleaseAndGetAddressOf()),
                "ID3D11Device::CreateDepthStencilView(probe depth)");

            // 面描画のHDR作業画像設定
            D3D11_TEXTURE2D_DESC scratchDescription{};
            scratchDescription.Width = ProbeBakeFaceSize;
            scratchDescription.Height = ProbeBakeFaceSize;
            scratchDescription.MipLevels = 1;
            scratchDescription.ArraySize = 1;
            scratchDescription.Format =
                DXGI_FORMAT_R16G16B16A16_FLOAT;
            scratchDescription.SampleDesc.Count = 1;
            scratchDescription.Usage = D3D11_USAGE_DEFAULT;
            scratchDescription.BindFlags =
                D3D11_BIND_SHADER_RESOURCE
                | D3D11_BIND_RENDER_TARGET;
            ThrowIfFailed(
                device->CreateTexture2D(
                    &scratchDescription,
                    nullptr,
                    scratchTexture.ReleaseAndGetAddressOf()),
                "ID3D11Device::CreateTexture2D(probe scratch)");
            ThrowIfFailed(
                device->CreateRenderTargetView(
                    scratchTexture.Get(),
                    nullptr,
                    scratchTarget.ReleaseAndGetAddressOf()),
                "ID3D11Device::CreateRenderTargetView(probe scratch)");
            ThrowIfFailed(
                device->CreateShaderResourceView(
                    scratchTexture.Get(),
                    nullptr,
                    scratchShaderResourceView.ReleaseAndGetAddressOf()),
                "ID3D11Device::CreateShaderResourceView(probe scratch)");
        }
    };

    EnvironmentRenderer::~EnvironmentRenderer() = default;

    EnvironmentRenderer::EnvironmentRenderer(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        AssetManager& assets,
        const std::filesystem::path& shaderPath)
        : m_device(device)
        , m_context(context)
    {
        if (device == nullptr || context == nullptr)
        {
            throw std::invalid_argument(
                "EnvironmentRenderer requires a Direct3D device and context.");
        }

        // 全画面VSのバイトコード
        const auto vertexByteCode =
            CompileShader(assets, shaderPath, "VSMain", "vs_5_0");
        // 空描画PSのバイトコード
        const auto skyByteCode =
            CompileShader(assets, shaderPath, "PSSky", "ps_5_0");
        // ブルームPSのバイトコード
        const auto bloomByteCode =
            CompileShader(assets, shaderPath, "PSBloom", "ps_5_0");
        // 画面輪郭PSのバイトコード
        const auto screenOutlineByteCode = CompileShader(
            assets,
            shaderPath,
            "PSScreenOutline",
            "ps_5_0");
        // レンズフレアPSのバイトコード
        const auto lensFlareByteCode = CompileShader(
            assets,
            shaderPath,
            "PSScreenSpaceLensFlare",
            "ps_5_0");
        // 光条PSのバイトコード
        const auto lensFlareStreakByteCode = CompileShader(
            assets,
            shaderPath,
            "PSLensFlareStreak",
            "ps_5_0");
        // トーン補正PSのバイトコード
        const auto toneMapByteCode =
            CompileShader(assets, shaderPath, "PSToneMap", "ps_5_0");
        // FXAAのバイトコード
        const auto fxaaByteCode =
            CompileShader(assets, shaderPath, "PSFXAA", "ps_5_0");
        // 画像コピーPSのバイトコード
        const auto copyByteCode =
            CompileShader(assets, shaderPath, "PSCopy", "ps_5_0");
        // 反転コピーPSのバイトコード
        const auto copyMirrorByteCode = CompileShader(
            assets,
            shaderPath,
            "PSCopyMirrorX",
            "ps_5_0");
        // TAAのバイトコード
        const auto temporalByteCode = CompileShader(
            assets,
            shaderPath,
            "PSTemporalAntiAliasing",
            "ps_5_0");
        // 光の筋PSのバイトコード
        const auto volumetricByteCode = CompileShader(
            assets,
            shaderPath,
            "PSVolumetricLight",
            "ps_5_0");
        // ぼけ準備PSのバイトコード
        const auto depthOfFieldPrepareByteCode = CompileShader(
            assets,
            shaderPath,
            "PSDepthOfFieldPrepare",
            "ps_5_0");
        // 円形ぼかしPSのバイトコード
        const auto depthOfFieldBlurByteCode = CompileShader(
            assets,
            shaderPath,
            "PSDepthOfFieldBlur",
            "ps_5_0");
        // ぼけ合成PSのバイトコード
        const auto depthOfFieldCompositeByteCode = CompileShader(
            assets,
            shaderPath,
            "PSDepthOfFieldComposite",
            "ps_5_0");
        // ブレ合成PSのバイトコード
        const auto motionBlurByteCode = CompileShader(
            assets,
            shaderPath,
            "PSMotionBlur",
            "ps_5_0");
        // 輝度測定PSのバイトコード
        const auto luminanceByteCode = CompileShader(
            assets,
            shaderPath,
            "PSLuminance",
            "ps_5_0");
        // AO計算PSのバイトコード
        const auto ambientOcclusionByteCode = CompileShader(
            assets,
            shaderPath,
            "PSAmbientOcclusion",
            "ps_5_0");
        // AOぼかしPSのバイトコード
        const auto ambientOcclusionBlurByteCode = CompileShader(
            assets,
            shaderPath,
            "PSAmbientOcclusionBlur",
            "ps_5_0");
        // 鏡面畳み込みPSのバイトコード
        const auto prefilterByteCode = CompileShader(
            assets,
            shaderPath,
            "PSPrefilterEnvironment",
            "ps_5_0");
        // 拡散畳み込みPSのバイトコード
        const auto irradianceByteCode = CompileShader(
            assets,
            shaderPath,
            "PSIrradiance",
            "ps_5_0");
        // 深度距離化PSのバイトコード
        const auto reflectionLinearizeByteCode =
            CompileShader(
                assets,
                shaderPath,
                "PSReflectionDepthLinearize",
                "ps_5_0");
        // Hi-Z縮小PSのバイトコード
        const auto reflectionDownsampleByteCode =
            CompileShader(
                assets,
                shaderPath,
                "PSReflectionDepthDownsample",
                "ps_5_0");
        ThrowIfFailed(
            device->CreateVertexShader(
                vertexByteCode->GetBufferPointer(),
                vertexByteCode->GetBufferSize(),
                nullptr,
                m_vertexShader.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateVertexShader(environment)");
        ThrowIfFailed(
            device->CreatePixelShader(
                skyByteCode->GetBufferPointer(),
                skyByteCode->GetBufferSize(),
                nullptr,
                m_skyPixelShader.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(sky)");
        ThrowIfFailed(
            device->CreatePixelShader(
                bloomByteCode->GetBufferPointer(),
                bloomByteCode->GetBufferSize(),
                nullptr,
                m_bloomPixelShader.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(bloom)");
        ThrowIfFailed(
            device->CreatePixelShader(
                screenOutlineByteCode->GetBufferPointer(),
                screenOutlineByteCode->GetBufferSize(),
                nullptr,
                m_screenOutlinePixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(screen outline)");
        ThrowIfFailed(
            device->CreatePixelShader(
                lensFlareByteCode->GetBufferPointer(),
                lensFlareByteCode->GetBufferSize(),
                nullptr,
                m_lensFlarePixelShader.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(screen space lens flare)");
        ThrowIfFailed(
            device->CreatePixelShader(
                lensFlareStreakByteCode->GetBufferPointer(),
                lensFlareStreakByteCode->GetBufferSize(),
                nullptr,
                m_lensFlareStreakPixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(lens flare streak)");
        ThrowIfFailed(
            device->CreatePixelShader(
                toneMapByteCode->GetBufferPointer(),
                toneMapByteCode->GetBufferSize(),
                nullptr,
                m_toneMapPixelShader.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(tone map)");
        ThrowIfFailed(
            device->CreatePixelShader(
                fxaaByteCode->GetBufferPointer(),
                fxaaByteCode->GetBufferSize(),
                nullptr,
                m_fxaaPixelShader.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(FXAA)");
        ThrowIfFailed(
            device->CreatePixelShader(
                copyByteCode->GetBufferPointer(),
                copyByteCode->GetBufferSize(),
                nullptr,
                m_copyPixelShader.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(copy)");
        ThrowIfFailed(
            device->CreatePixelShader(
                copyMirrorByteCode->GetBufferPointer(),
                copyMirrorByteCode->GetBufferSize(),
                nullptr,
                m_copyMirrorPixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(copy mirror)");
        ThrowIfFailed(
            device->CreatePixelShader(
                volumetricByteCode->GetBufferPointer(),
                volumetricByteCode->GetBufferSize(),
                nullptr,
                m_volumetricPixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(volumetric)");
        ThrowIfFailed(
            device->CreatePixelShader(
                temporalByteCode->GetBufferPointer(),
                temporalByteCode->GetBufferSize(),
                nullptr,
                m_temporalPixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(TAA)");
        ThrowIfFailed(
            device->CreatePixelShader(
                depthOfFieldPrepareByteCode
                    ->GetBufferPointer(),
                depthOfFieldPrepareByteCode->GetBufferSize(),
                nullptr,
                m_depthOfFieldPreparePixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader"
            "(depth of field prepare)");
        ThrowIfFailed(
            device->CreatePixelShader(
                depthOfFieldBlurByteCode->GetBufferPointer(),
                depthOfFieldBlurByteCode->GetBufferSize(),
                nullptr,
                m_depthOfFieldBlurPixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader"
            "(depth of field blur)");
        ThrowIfFailed(
            device->CreatePixelShader(
                depthOfFieldCompositeByteCode
                    ->GetBufferPointer(),
                depthOfFieldCompositeByteCode
                    ->GetBufferSize(),
                nullptr,
                m_depthOfFieldCompositePixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader"
            "(depth of field composite)");
        ThrowIfFailed(
            device->CreatePixelShader(
                motionBlurByteCode->GetBufferPointer(),
                motionBlurByteCode->GetBufferSize(),
                nullptr,
                m_motionBlurPixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(motion blur)");
        ThrowIfFailed(
            device->CreatePixelShader(
                luminanceByteCode->GetBufferPointer(),
                luminanceByteCode->GetBufferSize(),
                nullptr,
                m_luminancePixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(luminance)");
        ThrowIfFailed(
            device->CreatePixelShader(
                ambientOcclusionByteCode->GetBufferPointer(),
                ambientOcclusionByteCode->GetBufferSize(),
                nullptr,
                m_ambientOcclusionPixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(SSAO)");
        ThrowIfFailed(
            device->CreatePixelShader(
                ambientOcclusionBlurByteCode->GetBufferPointer(),
                ambientOcclusionBlurByteCode->GetBufferSize(),
                nullptr,
                m_ambientOcclusionBlurPixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(SSAO blur)");

        ThrowIfFailed(
            device->CreatePixelShader(
                prefilterByteCode->GetBufferPointer(),
                prefilterByteCode->GetBufferSize(),
                nullptr,
                m_prefilterPixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(prefilter)");
        ThrowIfFailed(
            device->CreatePixelShader(
                irradianceByteCode->GetBufferPointer(),
                irradianceByteCode->GetBufferSize(),
                nullptr,
                m_irradiancePixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(irradiance)");
        ThrowIfFailed(
            device->CreatePixelShader(
                reflectionLinearizeByteCode
                    ->GetBufferPointer(),
                reflectionLinearizeByteCode
                    ->GetBufferSize(),
                nullptr,
                m_reflectionLinearizePixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(hi-z linearize)");
        ThrowIfFailed(
            device->CreatePixelShader(
                reflectionDownsampleByteCode
                    ->GetBufferPointer(),
                reflectionDownsampleByteCode
                    ->GetBufferSize(),
                nullptr,
                m_reflectionDownsamplePixelShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreatePixelShader(hi-z downsample)");

        m_skyBuffer =
            CreateConstantBuffer<SkyConstants>(device);
        m_prefilterBuffer =
            CreateConstantBuffer<PrefilterConstants>(
                device);
        m_bloomBuffer =
            CreateConstantBuffer<BloomConstants>(device);
        m_screenOutlineBuffer =
            CreateConstantBuffer<ScreenOutlineConstants>(device);
        m_lensFlareBuffer =
            CreateConstantBuffer<LensFlareConstants>(device);
        m_colorGradingBuffer =
            CreateConstantBuffer<ColorGradingConstants>(device);
        m_ambientOcclusionBuffer =
            CreateConstantBuffer<AmbientOcclusionConstants>(
                device);
        m_volumetricBuffer =
            CreateConstantBuffer<VolumetricConstants>(
                device);
        m_temporalBuffer =
            CreateConstantBuffer<TemporalConstants>(
                device);
        m_depthOfFieldBuffer =
            CreateConstantBuffer<DepthOfFieldConstants>(
                device);
        m_motionBlurBuffer =
            CreateConstantBuffer<MotionBlurConstants>(
                device);
        m_luminanceBuffer =
            CreateConstantBuffer<LuminanceConstants>(
                device);

        // 影の範囲外は光が届く値1として比較する。
        // 光の筋用の影サンプラー設定
        D3D11_SAMPLER_DESC volumetricShadow{};
        volumetricShadow.Filter =
            D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        volumetricShadow.AddressU =
            D3D11_TEXTURE_ADDRESS_BORDER;
        volumetricShadow.AddressV =
            D3D11_TEXTURE_ADDRESS_BORDER;
        volumetricShadow.AddressW =
            D3D11_TEXTURE_ADDRESS_BORDER;
        volumetricShadow.BorderColor[0] = 1.0f;
        volumetricShadow.BorderColor[1] = 1.0f;
        volumetricShadow.BorderColor[2] = 1.0f;
        volumetricShadow.BorderColor[3] = 1.0f;
        volumetricShadow.ComparisonFunc =
            D3D11_COMPARISON_LESS_EQUAL;
        volumetricShadow.MinLOD = 0.0f;
        volumetricShadow.MaxLOD = 0.0f;
        ThrowIfFailed(
            device->CreateSamplerState(
                &volumetricShadow,
                m_volumetricShadowSampler
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateSamplerState"
            "(volumetric shadow)");

        // 線形CLAMPサンプラー設定
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = std::numeric_limits<float>::max();
        ThrowIfFailed(
            device->CreateSamplerState(
                &sampler,
                m_sampler.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateSamplerState(environment)");

        // 深度の読み書き無効の設定
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = FALSE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        ThrowIfFailed(
            device->CreateDepthStencilState(
                &depth,
                m_depthDisabled.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateDepthStencilState(environment)");

        // 両面描画のラスタライザー設定
        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = TRUE;
        ThrowIfFailed(
            device->CreateRasterizerState(
                &rasterizer,
                m_rasterizer.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateRasterizerState(environment)");
    }

    void EnvironmentRenderer::PrepareProbeBake()
    {
        if (!m_probeBakeResources)
        {
            m_probeBakeResources =
                std::make_unique<ProbeBakeResources>(m_device);
        }
    }

    void EnvironmentRenderer::RenderProbeCube(
        const ProbeFaceRenderer& renderFace)
    {
        if (!renderFace)
        {
            throw std::invalid_argument(
                "Probe bake requires a face renderer.");
        }
        PrepareProbeBake();

        // キューブ面のビューポート
        const D3D11_VIEWPORT viewport{
            0.0f,
            0.0f,
            static_cast<float>(ProbeBakeFaceSize),
            static_cast<float>(ProbeBakeFaceSize),
            0.0f,
            1.0f
        };
        // HDR面の黒い消去色
        constexpr float clearColor[4]{};
        // 描画するキューブ面の番号
        for (std::uint32_t face = 0; face < 6; ++face)
        {
            // 面描画の作業RTV
            ID3D11RenderTargetView* targets[]{
                m_probeBakeResources->scratchTarget.Get()
            };
            m_context->OMSetRenderTargets(
                1,
                targets,
                m_probeBakeResources->depthView.Get());
            m_context->RSSetViewports(1, &viewport);
            m_context->ClearRenderTargetView(
                targets[0], clearColor);
            m_context->ClearDepthStencilView(
                m_probeBakeResources->depthView.Get(),
                D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                1.0f,
                0);

            renderFace(face);
            CopyMirroredX(
                m_probeBakeResources
                    ->scratchShaderResourceView.Get(),
                m_probeBakeResources->faceTargets[face].Get(),
                ProbeBakeFaceSize,
                ProbeBakeFaceSize);
        }

        // 畳み込みで同じ画像を読むため、最後のキューブ面を描画先から外す。
        m_context->OMSetRenderTargets(0, nullptr, nullptr);
    }

    EnvironmentRenderer::OwnedPrefilteredEnvironment
        EnvironmentRenderer::BakeReflectionProbe(
            const ProbeFaceRenderer& renderFace,
            const std::optional<std::uint64_t> cacheKey)
    {
        // ベイクの再入禁止と終了解除
        const ProbeBakeScope bakeScope{ m_probeBakeActive };
        RenderProbeCube(renderFace);
        // 生成した鏡面・拡散画像
        auto baked = CreatePrefilteredEnvironment(
            m_probeBakeResources->cubeShaderResourceView.Get());
        if (cacheKey.has_value() && baked.IsValid())
        {
            EnvironmentCache::Store(
                *cacheKey,
                m_device,
                m_context,
                baked);
        }
        return baked;
    }

    std::optional<std::array<float, 12>>
        EnvironmentRenderer::BakeIrradianceProbe(
            const ProbeFaceRenderer& renderFace)
    {
        // ベイクの再入禁止と終了解除
        const ProbeBakeScope bakeScope{ m_probeBakeActive };
        RenderProbeCube(renderFace);
        // 生成した拡散照明キューブ
        auto irradianceOnly = CreatePrefilteredEnvironment(
            m_probeBakeResources->cubeShaderResourceView.Get(),
            false);
        // RGB各4要素のSH係数
        std::array<float, 12> coefficients{};
        if (irradianceOnly.irradiance == nullptr
            || !ProjectIrradianceToSh(
                irradianceOnly.irradiance.Get(),
                coefficients))
        {
            return std::nullopt;
        }
        return coefficients;
    }

    EnvironmentRenderer::PrefilteredEnvironment
        EnvironmentRenderer::GetPrefilteredEnvironment(
            ID3D11ShaderResourceView* const source,
            const std::uint64_t cacheKey)
    {
        if (source == nullptr)
        {
            return {};
        }
        if (m_prefilterSource.Get() != source
            || m_prefilterCacheKey != cacheKey)
        {
            BuildPrefilteredEnvironment(source, cacheKey);
        }
        return {
            m_prefilteredSpecular.Get(),
            m_prefilteredIrradiance.Get(),
            m_prefilteredMaximumMip
        };
    }

    void EnvironmentRenderer::BuildPrefilteredEnvironment(
        ID3D11ShaderResourceView* const source,
        const std::uint64_t cacheKey)
    {
        // 参照と結果が混在したキャッシュを残さないよう、全画像を完成後に一括更新する。
        // 更新後に保持する元の画像
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> nextSource{
            source
        };
        // 完成後に公開する畳み込み結果
        OwnedPrefilteredEnvironment next;
        // ディスクから復元できたか
        bool restoredFromCache{};

        // 保存済みの畳み込み結果があればGPUでの再計算を省く。
        if (cacheKey != 0)
        {
            next = EnvironmentCache::TryLoad(
                m_device,
                cacheKey);
            restoredFromCache = next.IsValid();
        }
        if (!restoredFromCache)
        {
            next = CreatePrefilteredEnvironment(source);
            if (cacheKey != 0 && next.IsValid())
            {
                EnvironmentCache::Store(
                    cacheKey,
                    m_device,
                    m_context,
                    next);
            }
        }

        m_prefilterSource = std::move(nextSource);
        m_prefilterCacheKey = cacheKey;
        m_prefilteredSpecular =
            std::move(next.specular);
        m_prefilteredIrradiance =
            std::move(next.irradiance);
        m_prefilteredMaximumMip =
            next.specularMaximumMip;
    }

    EnvironmentRenderer::OwnedPrefilteredEnvironment
        EnvironmentRenderer::CreatePrefilteredEnvironment(
            ID3D11ShaderResourceView* const source,
            const bool includeSpecular)
    {
        using Microsoft::WRL::ComPtr;

        // 呼び出し側が保持する生成結果
        OwnedPrefilteredEnvironment result;
        if (source == nullptr)
        {
            return result;
        }


        // 入力SRVが保持する画像資源
        ComPtr<ID3D11Resource> resource;
        source->GetResource(
            resource.ReleaseAndGetAddressOf());
        // 入力キューブの2D配列画像
        ComPtr<ID3D11Texture2D> sourceTexture;
        if (FAILED(resource.As(&sourceTexture)))
        {
            return result;
        }
        // 入力キューブの寸法設定
        D3D11_TEXTURE2D_DESC sourceDescription{};
        sourceTexture->GetDesc(&sourceDescription);

        // RGBA16FキューブとSRVを生成する(size: 一辺の画素数, mips: ミップ段数, texture: 画像の出力, view: SRVの出力)。
        const auto createCube =
            [this](
                const std::uint32_t size,
                const std::uint32_t mips,
                ComPtr<ID3D11Texture2D>& texture,
                ComPtr<ID3D11ShaderResourceView>& view)
        {
            // HDRキューブの生成設定
            D3D11_TEXTURE2D_DESC description{};
            description.Width = size;
            description.Height = size;
            description.MipLevels = mips;
            description.ArraySize = 6;
            description.Format =
                DXGI_FORMAT_R16G16B16A16_FLOAT;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_DEFAULT;
            description.BindFlags =
                D3D11_BIND_SHADER_RESOURCE
                | D3D11_BIND_RENDER_TARGET;
            description.MiscFlags =
                D3D11_RESOURCE_MISC_TEXTURECUBE;
            ThrowIfFailed(
                m_device->CreateTexture2D(
                    &description,
                    nullptr,
                    texture.ReleaseAndGetAddressOf()),
                "ID3D11Device::CreateTexture2D(prefilter)");
            // キューブSRVの生成設定
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
            viewDescription.Format = description.Format;
            viewDescription.ViewDimension =
                D3D11_SRV_DIMENSION_TEXTURECUBE;
            viewDescription.TextureCube.MipLevels = mips;
            ThrowIfFailed(
                m_device->CreateShaderResourceView(
                    texture.Get(),
                    &viewDescription,
                    view.ReleaseAndGetAddressOf()),
                "ID3D11Device::CreateShaderResourceView(prefilter)");
        };

        // 粗さ別の鏡面出力キューブ
        ComPtr<ID3D11Texture2D> specularTexture;
        // 拡散照明の出力キューブ
        ComPtr<ID3D11Texture2D> irradianceTexture;
        // GI用には鏡面畳み込みを省き、拡散キューブだけ生成する。
        if (includeSpecular)
        {
            createCube(
                PrefilteredSpecularSize,
                PrefilteredSpecularMipLevels,
                specularTexture,
                result.specular);
        }
        createCube(
            PrefilteredIrradianceSize,
            PrefilteredIrradianceMipLevels,
            irradianceTexture,
            result.irradiance);

        // 生成途中の失敗で描画状態を変えないよう、全RTVを事前に作る。
        // 各ミップの6面のRTVを生成する(texture: 出力キューブ, mipLevels: ミップ段数)。
        const auto createFaceTargets = [this](
            ID3D11Texture2D* const texture,
            const std::uint32_t mipLevels)
        {
            // ミップ順に並べる各面のRTV
            std::vector<Microsoft::WRL::ComPtr<
                ID3D11RenderTargetView>> targets;
            targets.reserve(
                static_cast<std::size_t>(mipLevels) * 6u);
            // 生成するミップ段の番号
            for (std::uint32_t mip{}; mip < mipLevels; ++mip)
            {
                // 生成するキューブ面の番号
                for (std::uint32_t face{}; face < 6u; ++face)
                {
                    // 出力ミップと面のRTV設定
                    D3D11_RENDER_TARGET_VIEW_DESC description{};
                    description.Format =
                        DXGI_FORMAT_R16G16B16A16_FLOAT;
                    description.ViewDimension =
                        D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                    description.Texture2DArray.MipSlice = mip;
                    description.Texture2DArray.FirstArraySlice = face;
                    description.Texture2DArray.ArraySize = 1;
                    // 生成したミップと面のRTV
                    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
                    ThrowIfFailed(
                        m_device->CreateRenderTargetView(
                            texture,
                            &description,
                            target.ReleaseAndGetAddressOf()),
                        "ID3D11Device::CreateRenderTargetView(prefilter)");
                    targets.push_back(std::move(target));
                }
            }
            return targets;
        };
        // 鏡面畳み込み用の各面RTV
        std::vector<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>>
            specularTargets;
        if (includeSpecular)
        {
            specularTargets = createFaceTargets(
                specularTexture.Get(),
                PrefilteredSpecularMipLevels);
        }
        // 拡散畳み込み用の各面RTV
        const auto irradianceTargets = createFaceTargets(
            irradianceTexture.Get(),
            PrefilteredIrradianceMipLevels);

        // シーン描画中の生成でも、畳み込みが変更する描画状態を復元する。
        // 描画状態の保存と終了時の復元
        const PipelineStateScope pipelineState{ m_context };

        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(
            m_vertexShader.Get(),
            nullptr,
            0);
        // t1へ渡す入力キューブ
        ID3D11ShaderResourceView* sourceResources[]{
            source };
        m_context->PSSetShaderResources(
            1,
            1,
            sourceResources);
        // 畳み込み用のサンプラー参照
        ID3D11SamplerState* samplers[]{
            m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        // b3の畳み込み定数参照
        ID3D11Buffer* buffers[]{
            m_prefilterBuffer.Get() };
        m_context->PSSetConstantBuffers(3, 1, buffers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(),
            0);
        m_context->RSSetState(m_rasterizer.Get());

        // 全ミップの6面を畳み込む(shader: 畳み込みPS, size: 最上位の幅, mips: ミップ段数, sourceResolution: 元画像の幅, faceTargets: ミップごとの6面RTV)。
        const auto renderFaces =
            [this](
                ID3D11PixelShader* shader,
                const std::uint32_t size,
                const std::uint32_t mips,
                const float sourceResolution,
                const std::vector<Microsoft::WRL::ComPtr<
                    ID3D11RenderTargetView>>& faceTargets)
        {
            m_context->PSSetShader(shader, nullptr, 0);
            // 生成するミップ段の番号
            for (std::uint32_t mip = 0;
                mip < mips;
                ++mip)
            {
                // 今回のミップの一辺の画素数
                const float mipSize = static_cast<float>(
                    std::max(size >> mip, 1u));
                // 今回のミップのビューポート
                D3D11_VIEWPORT viewport{};
                viewport.Width = mipSize;
                viewport.Height = mipSize;
                viewport.MaxDepth = 1.0f;
                m_context->RSSetViewports(1, &viewport);
                // ミップ段に対応する材質粗さ
                const float roughness =
                    mips <= 1
                        ? 0.0f
                        : static_cast<float>(mip)
                            / static_cast<float>(
                                mips - 1);
                // 生成するキューブ面の番号
                for (std::uint32_t face = 0;
                    face < 6;
                    ++face)
                {
                    // 今回の面と粗さの定数
                    PrefilterConstants constants{};
                    constants.parameters = {
                        static_cast<float>(face),
                        roughness,
                        sourceResolution,
                        0.0f
                    };
                    m_context->UpdateSubresource(
                        m_prefilterBuffer.Get(),
                        0,
                        nullptr,
                        &constants,
                        0,
                        0);

                    // 今回描くキューブ面のRTV
                    ID3D11RenderTargetView* targets[]{
                        faceTargets[static_cast<std::size_t>(mip) * 6u
                            + face].Get() };
                    m_context->OMSetRenderTargets(
                        1,
                        targets,
                        nullptr);
                    m_context->Draw(3, 0);
                }
            }
        };

        if (includeSpecular)
        {
            renderFaces(
                m_prefilterPixelShader.Get(),
                PrefilteredSpecularSize,
                PrefilteredSpecularMipLevels,
                static_cast<float>(sourceDescription.Width),
                specularTargets);
        }
        renderFaces(
            m_irradiancePixelShader.Get(),
            PrefilteredIrradianceSize,
            PrefilteredIrradianceMipLevels,
            static_cast<float>(sourceDescription.Width),
            irradianceTargets);

        result.specularMaximumMip =
            static_cast<float>(PrefilteredSpecularMipLevels - 1);
        return result;
    }

    void EnvironmentRenderer::DrawSky(
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection,
        const SkySettings& settings,
        ID3D11ShaderResourceView* cubemap,
        const SkySun* sun)
    {
        if (!settings.enabled)
        {
            return;
        }
        using namespace DirectX;
        // 逆行列計算時の行列式
        XMVECTOR determinant{};
        // 空の方向を復元する逆行列
        const XMMATRIX inverseViewProjection =
            XMMatrixInverse(
                &determinant,
                view * projection);
        // カメラ位置を求める逆ビュー
        const XMMATRIX inverseView =
            XMMatrixInverse(&determinant, view);
        // 空と太陽の描画定数
        SkyConstants constants{};
        XMStoreFloat4x4(
            &constants.inverseViewProjection,
            inverseViewProjection);
        XMStoreFloat4(
            &constants.cameraPosition,
            inverseView.r[3]);
        constants.topColor = {
            settings.topColor.x,
            settings.topColor.y,
            settings.topColor.z,
            settings.intensity
        };
        constants.horizonColor = {
            settings.horizonColor.x,
            settings.horizonColor.y,
            settings.horizonColor.z,
            1.0f
        };
        constants.groundColor = {
            settings.groundColor.x,
            settings.groundColor.y,
            settings.groundColor.z,
            1.0f
        };
        if (sun != nullptr)
        {
            // 太陽方向ベクトルの長さ
            const auto length = std::sqrt(
                sun->directionToSun.x * sun->directionToSun.x
                + sun->directionToSun.y * sun->directionToSun.y
                + sun->directionToSun.z
                    * sun->directionToSun.z);
            // 太陽方向を正規化する倍率
            const float scale =
                length > 0.0001f ? 1.0f / length : 0.0f;
            // 角半径ゼロでも太陽円盤を残すため、描画半径の下限を適用する。
            constants.sunDirection = {
                sun->directionToSun.x * scale,
                sun->directionToSun.y * scale,
                sun->directionToSun.z * scale,
                std::max(sun->angularRadius, 0.004625f)
            };
            constants.sunDiskColor = {
                sun->color.x,
                sun->color.y,
                sun->color.z,
                1.0f
            };
        }
        constants.options = {
            cubemap != nullptr ? 1.0f : 0.0f,
            0.0f,
            0.0f,
            0.0f
        };
        m_context->UpdateSubresource(
            m_skyBuffer.Get(), 0, nullptr, &constants, 0, 0);

        // 保存する深度・ステンシル状態
        Microsoft::WRL::ComPtr<
            ID3D11DepthStencilState> previousDepth;
        // 保存するステンシル参照値
        UINT previousStencilReference{};
        m_context->OMGetDepthStencilState(
            previousDepth.ReleaseAndGetAddressOf(),
            &previousStencilReference);
        // 保存するラスタライザー状態
        Microsoft::WRL::ComPtr<
            ID3D11RasterizerState> previousRasterizer;
        m_context->RSGetState(
            previousRasterizer.ReleaseAndGetAddressOf());
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(
            m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(
            m_skyPixelShader.Get(), nullptr, 0);
        // 空の定数バッファ参照
        ID3D11Buffer* buffers[]{ m_skyBuffer.Get() };
        m_context->PSSetConstantBuffers(0, 1, buffers);
        // t1の空キューブSRV
        ID3D11ShaderResourceView* skyResources[]{
            cubemap };
        m_context->PSSetShaderResources(
            1,
            1,
            skyResources);
        // 空の画像サンプラー参照
        ID3D11SamplerState* skySamplers[]{
            m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, skySamplers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(), 0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);
        // 空キューブの割り当て解除
        ID3D11ShaderResourceView* clearResources[]{
            nullptr };
        m_context->PSSetShaderResources(
            1,
            1,
            clearResources);
        m_context->OMSetDepthStencilState(
            previousDepth.Get(),
            previousStencilReference);
        m_context->RSSetState(
            previousRasterizer.Get());
    }

    void EnvironmentRenderer::ApplyBloom(
        ID3D11ShaderResourceView* source,
        ID3D11RenderTargetView* destination,
        const std::uint32_t width,
        const std::uint32_t height,
        const BloomSettings& settings)
    {
        if (source == nullptr || destination == nullptr)
        {
            return;
        }
        // 今回の画像処理に渡す定数
        const BloomConstants constants{
            {
                1.0f / static_cast<float>(std::max(width, 1u)),
                1.0f / static_cast<float>(std::max(height, 1u))
            },
            std::clamp(settings.threshold, 0.0f, 4.0f),
            settings.enabled
                ? std::clamp(settings.intensity, 0.0f, 8.0f)
                : 0.0f,
            std::clamp(settings.radius, 0.25f, 12.0f),
            {}
        };
        m_context->UpdateSubresource(
            m_bloomBuffer.Get(), 0, nullptr, &constants, 0, 0);

        // 今回の画像処理の出力RTV
        ID3D11RenderTargetView* targets[]{ destination };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        // 出力画像のビューポート
        D3D11_VIEWPORT viewport{
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
            m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(
            m_bloomPixelShader.Get(), nullptr, 0);
        // 今回の画像処理の定数参照
        ID3D11Buffer* buffers[]{ m_bloomBuffer.Get() };
        m_context->PSSetConstantBuffers(1, 1, buffers);
        // 今回の画像処理のSRV参照
        ID3D11ShaderResourceView* resources[]{ source };
        m_context->PSSetShaderResources(0, 1, resources);
        // 画像処理用のサンプラー参照
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(), 0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);
        // 読み取り参照の解除用配列
        ID3D11ShaderResourceView* nullResource[]{ nullptr };
        m_context->PSSetShaderResources(
            0, 1, nullResource);
    }

    void EnvironmentRenderer::ApplyScreenOutline(
        ID3D11ShaderResourceView* const source,
        ID3D11ShaderResourceView* const depth,
        ID3D11RenderTargetView* const destination,
        const std::uint32_t width,
        const std::uint32_t height,
        const ScreenOutlineSettings& settings,
        const DirectX::XMFLOAT4X4& projection)
    {
        if (!settings.enabled
            || source == nullptr
            || depth == nullptr
            || destination == nullptr
            || std::abs(projection._11) < 1e-6f
            || std::abs(projection._22) < 1e-6f)
        {
            return;
        }

        // 1画素以上に補正した出力幅
        const float safeWidth =
            static_cast<float>(std::max(width, 1u));
        // 1画素以上に補正した出力高
        const float safeHeight =
            static_cast<float>(std::max(height, 1u));
        // 今回の画像処理に渡す定数
        ScreenOutlineConstants constants{};
        constants.color = {
            std::clamp(settings.color.x, 0.0f, 1.0f),
            std::clamp(settings.color.y, 0.0f, 1.0f),
            std::clamp(settings.color.z, 0.0f, 1.0f),
            std::clamp(settings.intensity, 0.0f, 1.0f)
        };
        constants.parameters = {
            std::clamp(settings.thickness, 1.0f, 4.0f),
            std::clamp(settings.depthThreshold, 0.0001f, 1.0f),
            std::clamp(settings.normalThreshold, 0.0f, 1.0f),
            0.0f
        };
        constants.projection = {
            projection._33,
            projection._43,
            1.0f / projection._11,
            1.0f / projection._22
        };
        constants.texel = {
            1.0f / safeWidth,
            1.0f / safeHeight,
            safeWidth,
            safeHeight
        };
        m_context->UpdateSubresource(
            m_screenOutlineBuffer.Get(),
            0,
            nullptr,
            &constants,
            0,
            0);

        // 今回の画像処理の出力RTV
        ID3D11RenderTargetView* targets[]{ destination };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        // 出力画像のビューポート
        const D3D11_VIEWPORT viewport{
            0.0f,
            0.0f,
            safeWidth,
            safeHeight,
            0.0f,
            1.0f
        };
        m_context->RSSetViewports(1, &viewport);
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(
            m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(
            m_screenOutlinePixelShader.Get(), nullptr, 0);
        // 今回の画像処理の定数参照
        ID3D11Buffer* buffers[]{ m_screenOutlineBuffer.Get() };
        m_context->PSSetConstantBuffers(11, 1, buffers);
        // t0は現在色、t2は深度とし、t1は空のキューブ画像用に空ける。
        // 今回の画像処理のSRV参照
        ID3D11ShaderResourceView* resources[]{
            source,
            nullptr,
            depth
        };
        m_context->PSSetShaderResources(
            0,
            static_cast<UINT>(std::size(resources)),
            resources);
        // 画像処理用のサンプラー参照
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(),
            0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);

        // 読み取り参照の解除用配列
        ID3D11ShaderResourceView* nullResources[3]{};
        m_context->PSSetShaderResources(
            0,
            static_cast<UINT>(std::size(nullResources)),
            nullResources);
    }

    void EnvironmentRenderer::ApplyScreenSpaceLensFlare(
        ID3D11ShaderResourceView* const source,
        ID3D11RenderTargetView* const destination,
        const std::uint32_t width,
        const std::uint32_t height,
        const ScreenSpaceLensFlareSettings& settings,
        ID3D11ShaderResourceView* const streak)
    {
        if (!settings.enabled
            || source == nullptr
            || destination == nullptr)
        {
            return;
        }

        // 今回の画像処理に渡す定数
        const LensFlareConstants constants{
            {
                1.0f / static_cast<float>(std::max(width, 1u)),
                1.0f / static_cast<float>(std::max(height, 1u)),
                std::clamp(settings.threshold, 0.0f, 16.0f),
                std::clamp(settings.intensity, 0.0f, 8.0f)
            },
            {
                std::clamp(settings.ghostDispersal, 0.01f, 2.0f),
                std::clamp(settings.haloWidth, 0.05f, 1.5f),
                std::clamp(settings.chromaticAberration, 0.0f, 1.0f),
                std::clamp(settings.streakIntensity, 0.0f, 4.0f)
            },
            {
                std::clamp(settings.streakLength, 0.0f, 1.0f),
                0.0f,
                0.0f,
                0.0f
            }
        };
        m_context->UpdateSubresource(
            m_lensFlareBuffer.Get(),
            0,
            nullptr,
            &constants,
            0,
            0);

        // 今回の画像処理の出力RTV
        ID3D11RenderTargetView* targets[]{ destination };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        // 出力画像のビューポート
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
            m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(
            m_lensFlarePixelShader.Get(), nullptr, 0);
        // 今回の画像処理の定数参照
        ID3D11Buffer* buffers[]{ m_lensFlareBuffer.Get() };
        m_context->PSSetConstantBuffers(7, 1, buffers);
        // 今回の画像処理のSRV参照
        ID3D11ShaderResourceView* resources[]{ source };
        m_context->PSSetShaderResources(0, 1, resources);
        // 光条合成用のt5参照
        ID3D11ShaderResourceView* streakResources[]{ streak };
        m_context->PSSetShaderResources(5, 1, streakResources);
        // 画像処理用のサンプラー参照
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(),
            0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);
        // 読み取り参照の解除用配列
        ID3D11ShaderResourceView* nullResource[]{ nullptr };
        m_context->PSSetShaderResources(
            0,
            1,
            nullResource);
        m_context->PSSetShaderResources(
            5,
            1,
            nullResource);
    }

    void EnvironmentRenderer::BuildLensFlareStreaks(
        ID3D11ShaderResourceView* const source,
        ID3D11RenderTargetView* const firstTarget,
        ID3D11ShaderResourceView* const firstResource,
        ID3D11RenderTargetView* const secondTarget,
        ID3D11ShaderResourceView* const secondResource,
        const std::uint32_t width,
        const std::uint32_t height,
        const ScreenSpaceLensFlareSettings& settings)
    {
        m_lastStreakResource = nullptr;
        if (!settings.enabled
            || source == nullptr
            || firstTarget == nullptr
            || secondTarget == nullptr)
        {
            return;
        }
        if (settings.streakIntensity <= 0.0f
            || settings.streakLength <= 0.0f)
        {

            return;
        }

        // 出力画像のビューポート
        const D3D11_VIEWPORT viewport{
            0.0f,
            0.0f,
            static_cast<float>(std::max(width, 1u)),
            static_cast<float>(std::max(height, 1u)),
            0.0f,
            1.0f
        };
        // 画像処理用のサンプラー参照
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        // 今回の画像処理の定数参照
        ID3D11Buffer* buffers[]{ m_lensFlareBuffer.Get() };
        // 読み取り参照の解除用配列
        ID3D11ShaderResourceView* nullResource[]{ nullptr };

        // 初回は元画像の高輝度を抽出し、後続2回は直前の光条を読み、タップ間隔を毎回4倍にする。
        // 光条を広げるパス数
        constexpr int PassCount = 3;
        // 光条を広げる最大距離
        const float longest = std::clamp(
            settings.streakLength,
            0.0f,
            1.0f);
        // 片側2タップを3回広げる総距離が2(s+4s+16s)=42sとなるため、初回刻みを逆算する。
        // 初回の光条のタップ間隔
        const float baseStride = longest / 42.0f;

        // 光条を広げるパス番号
        for (int pass = 0; pass < PassCount; ++pass)
        {
            // 元の高輝度を抽出する初回か
            const bool first = pass == 0;
            // 今回の光条を書き出すRTV
            auto* const target = first
                ? firstTarget
                : ((pass % 2) == 1 ? secondTarget : firstTarget);
            // 今回の光条処理が読むSRV
            auto* const readResource = first
                ? source
                : ((pass % 2) == 1
                    ? firstResource
                    : secondResource);

            // 今回の画像処理に渡す定数
            LensFlareConstants constants{};
            constants.primary = {
                1.0f / static_cast<float>(std::max(width, 1u)),
                1.0f / static_cast<float>(std::max(height, 1u)),
                std::clamp(settings.threshold, 0.0f, 16.0f),
                std::clamp(settings.intensity, 0.0f, 8.0f)
            };
            constants.secondary = {
                std::clamp(settings.ghostDispersal, 0.01f, 2.0f),
                std::clamp(settings.haloWidth, 0.05f, 1.5f),
                std::clamp(
                    settings.chromaticAberration,
                    0.0f,
                    1.0f),
                std::clamp(settings.streakIntensity, 0.0f, 4.0f)
            };
            constants.tertiary = { longest, 0.0f, 0.0f, 0.0f };
            constants.streakPass = {
                baseStride
                    * std::pow(4.0f, static_cast<float>(pass)),
                static_cast<float>(
                    std::clamp<std::uint32_t>(
                        settings.streakDirections,
                        1u,
                        4u)),
                DirectX::XMConvertToRadians(
                    settings.streakAngleDegrees),
                first ? 1.0f : 0.0f
            };
            m_context->UpdateSubresource(
                m_lensFlareBuffer.Get(),
                0,
                nullptr,
                &constants,
                0,
                0);

            // 今回の画像処理の出力RTV
            ID3D11RenderTargetView* targets[]{ target };
            m_context->OMSetRenderTargets(1, targets, nullptr);
            m_context->RSSetViewports(1, &viewport);
            m_context->IASetInputLayout(nullptr);
            m_context->IASetPrimitiveTopology(
                D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_context->VSSetShader(
                m_vertexShader.Get(),
                nullptr,
                0);
            m_context->PSSetShader(
                m_lensFlareStreakPixelShader.Get(),
                nullptr,
                0);
            m_context->PSSetConstantBuffers(7, 1, buffers);
            m_context->PSSetSamplers(0, 1, samplers);
            // 出力と同じ画像を読まないよう、初回の元画像をt0、後続の光条をt5へ設定する。
            // 初回だけt0へ渡す元の画像
            ID3D11ShaderResourceView* sourceSlot[]{
                first ? readResource : nullptr
            };
            // 後続でt5へ渡す直前の光条
            ID3D11ShaderResourceView* streakSlot[]{
                first ? nullptr : readResource
            };
            m_context->PSSetShaderResources(0, 1, sourceSlot);
            m_context->PSSetShaderResources(5, 1, streakSlot);
            m_context->OMSetDepthStencilState(
                m_depthDisabled.Get(),
                0);
            m_context->RSSetState(m_rasterizer.Get());
            m_context->Draw(3, 0);
            m_context->PSSetShaderResources(0, 1, nullResource);
            m_context->PSSetShaderResources(5, 1, nullResource);

            m_lastStreakResource = (target == firstTarget)
                ? firstResource
                : secondResource;
        }
        // 描画先の解除用配列
        ID3D11RenderTargetView* noTargets[]{ nullptr };
        m_context->OMSetRenderTargets(1, noTargets, nullptr);
    }

    void EnvironmentRenderer::ApplyFXAA(
        ID3D11ShaderResourceView* source,
        ID3D11RenderTargetView* destination,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        if (source == nullptr || destination == nullptr)
        {
            return;
        }
        // 今回の画像処理に渡す定数
        const BloomConstants constants{
            {
                1.0f / static_cast<float>(std::max(width, 1u)),
                1.0f / static_cast<float>(std::max(height, 1u))
            },
            0.0f,
            0.0f,
            1.0f,
            {}
        };
        m_context->UpdateSubresource(
            m_bloomBuffer.Get(), 0, nullptr, &constants, 0, 0);

        // 今回の画像処理の出力RTV
        ID3D11RenderTargetView* targets[]{ destination };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        // 出力画像のビューポート
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
            m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(
            m_fxaaPixelShader.Get(), nullptr, 0);
        // 今回の画像処理の定数参照
        ID3D11Buffer* buffers[]{ m_bloomBuffer.Get() };
        m_context->PSSetConstantBuffers(1, 1, buffers);
        // 今回の画像処理のSRV参照
        ID3D11ShaderResourceView* resources[]{ source };
        m_context->PSSetShaderResources(0, 1, resources);
        // 画像処理用のサンプラー参照
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(), 0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);
        // 読み取り参照の解除用配列
        ID3D11ShaderResourceView* nullResource[]{ nullptr };
        m_context->PSSetShaderResources(0, 1, nullResource);
    }

    void EnvironmentRenderer::ApplyToneMapping(
        ID3D11ShaderResourceView* source,
        ID3D11RenderTargetView* destination,
        const std::uint32_t width,
        const std::uint32_t height,
        const ColorGradingSettings& settings)
    {
        if (source == nullptr || destination == nullptr)
        {
            return;
        }
        if (!settings.toneMappingEnabled)
        {
            Copy(
                source,
                destination,
                width,
                height);
            return;
        }

        // 定数バッファの配置を保ち、予約していた成分へ自動露出の補正段数を格納する。
        // 今回の画像処理に渡す定数
        const ColorGradingConstants constants{
            {
                std::clamp(settings.exposure, -8.0f, 8.0f),
                std::clamp(settings.contrast, 0.0f, 4.0f),
                std::clamp(settings.saturation, 0.0f, 4.0f),
                std::clamp(settings.temperature, -2.0f, 2.0f)
            },
            {
                std::clamp(settings.tint, -2.0f, 2.0f),
                std::clamp(settings.vignette, 0.0f, 1.0f),
                settings.enabled ? 1.0f : 0.0f,
                std::clamp(
                    settings.autoExposureStops,
                    -16.0f,
                    16.0f)
            }
        };
        m_context->UpdateSubresource(
            m_colorGradingBuffer.Get(),
            0,
            nullptr,
            &constants,
            0,
            0);

        // 今回の画像処理の出力RTV
        ID3D11RenderTargetView* targets[]{ destination };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        // 出力画像のビューポート
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
        m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(
            m_toneMapPixelShader.Get(), nullptr, 0);
        // 今回の画像処理の定数参照
        ID3D11Buffer* buffers[]{ m_colorGradingBuffer.Get() };
        m_context->PSSetConstantBuffers(2, 1, buffers);
        // 今回の画像処理のSRV参照
        ID3D11ShaderResourceView* resources[]{ source };
        m_context->PSSetShaderResources(0, 1, resources);
        // 画像処理用のサンプラー参照
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        m_context->OMSetDepthStencilState(m_depthDisabled.Get(), 0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);
        // 読み取り参照の解除用配列
        ID3D11ShaderResourceView* nullResource[]{ nullptr };
        m_context->PSSetShaderResources(0, 1, nullResource);
    }

    bool EnvironmentRenderer::RenderAmbientOcclusion(
        ID3D11ShaderResourceView* depth,
        ID3D11RenderTargetView* destination,
        const std::uint32_t width,
        const std::uint32_t height,
        const AmbientOcclusionSettings& settings,
        const DirectX::XMFLOAT4X4& projection,
        const std::uint32_t sampleCount)
    {
        if (depth == nullptr
            || destination == nullptr
            || !settings.enabled)
        {
            return false;
        }

        // 1画素以上に補正した出力幅
        const float safeWidth =
            static_cast<float>(std::max(width, 1u));
        // 1画素以上に補正した出力高
        const float safeHeight =
            static_cast<float>(std::max(height, 1u));
        // 射影の11・22成分の逆数を使うため、ゼロに近い場合は描画を省く。
        if (std::abs(projection._11) < 1e-6f
            || std::abs(projection._22) < 1e-6f)
        {
            return false;
        }
        // 今回の画像処理に渡す定数
        const AmbientOcclusionConstants constants{
            DirectX::XMFLOAT4{
                1.0f / safeWidth,
                1.0f / safeHeight,
                std::clamp(settings.radius, 0.01f, 10.0f),
                std::clamp(settings.strength, 0.0f, 1.0f)
            },
            DirectX::XMFLOAT4{
                projection._33,
                projection._43,
                1.0f / projection._11,
                1.0f / projection._22
            },
            DirectX::XMFLOAT4{
                static_cast<float>(
                    std::clamp(sampleCount, 4u, 32u)),
                0.0f,
                0.0f,
                0.0f
            }
        };
        m_context->UpdateSubresource(
            m_ambientOcclusionBuffer.Get(),
            0,
            nullptr,
            &constants,
            0,
            0);

        DrawAmbientOcclusionPass(
            m_ambientOcclusionPixelShader.Get(),
            nullptr,
            depth,
            destination,
            safeWidth,
            safeHeight);
        return true;
    }

    void EnvironmentRenderer::BlurAmbientOcclusion(
        ID3D11ShaderResourceView* occlusion,
        ID3D11ShaderResourceView* depth,
        ID3D11RenderTargetView* destination,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        if (occlusion == nullptr
            || depth == nullptr
            || destination == nullptr)
        {
            return;
        }
        // RenderAmbientOcclusionで設定した画像サイズと射影の定数を再利用する。
        DrawAmbientOcclusionPass(
            m_ambientOcclusionBlurPixelShader.Get(),
            occlusion,
            depth,
            destination,
            static_cast<float>(std::max(width, 1u)),
            static_cast<float>(std::max(height, 1u)));
    }


    void EnvironmentRenderer::DrawAmbientOcclusionPass(
        ID3D11PixelShader* pixelShader,
        ID3D11ShaderResourceView* source,
        ID3D11ShaderResourceView* depth,
        ID3D11RenderTargetView* destination,
        const float width,
        const float height)
    {
        if (pixelShader == nullptr)
        {
            return;
        }
        // 今回の画像処理の出力RTV
        ID3D11RenderTargetView* targets[]{ destination };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        // 出力画像のビューポート
        const D3D11_VIEWPORT viewport{
            0.0f,
            0.0f,
            width,
            height,
            0.0f,
            1.0f
        };
        m_context->RSSetViewports(1, &viewport);
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(
            m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(pixelShader, nullptr, 0);
        // 今回の画像処理の定数参照
        ID3D11Buffer* buffers[]{
            m_ambientOcclusionBuffer.Get()
        };
        m_context->PSSetConstantBuffers(4, 1, buffers);
        // t1は空のキューブ画像用に空ける。
        // 今回の画像処理のSRV参照
        ID3D11ShaderResourceView* resources[]{
            source,
            nullptr,
            depth
        };
        m_context->PSSetShaderResources(
            0,
            static_cast<UINT>(std::size(resources)),
            resources);
        // 画像処理用のサンプラー参照
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(), 0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);

        // 同じ画像を次のパスで描画先にできるよう、読み取り参照を解除する。
        // 読み取り参照の解除用配列
        ID3D11ShaderResourceView* nullResources[]{
            nullptr,
            nullptr,
            nullptr,
            nullptr
        };
        m_context->PSSetShaderResources(
            0,
            static_cast<UINT>(std::size(nullResources)),
            nullResources);
    }

    bool EnvironmentRenderer::ApplyTemporalAntiAliasing(
        ID3D11ShaderResourceView* const source,
        ID3D11RenderTargetView* const destination,
        const std::uint32_t width,
        const std::uint32_t height,
        const TemporalAntiAliasingSettings& settings,
        const TemporalInputs& inputs)
    {
        // 履歴・深度・前フレーム行列が揃う場合だけ再投影する。
        if (!settings.enabled
            || !inputs.previousValid
            || source == nullptr
            || destination == nullptr
            || !inputs.history
            || !inputs.depth
            || m_backend == nullptr)
        {
            return false;
        }

        // 解決したTAAの色履歴SRV
        ID3D11ShaderResourceView* history{};
        // 解決したシーン深度SRV
        ID3D11ShaderResourceView* depth{};
        try
        {
            history = m_backend->ResolveShaderResourceView(
                inputs.history);
            depth = m_backend->ResolveShaderResourceView(
                inputs.depth);
        }
        catch (const std::exception&)
        {
            // 別機器や旧世代の画像は描画に渡さず、TAAを省く。
            return false;
        }
        if (history == nullptr || depth == nullptr)
        {
            return false;
        }

        // 履歴の寸法・ミップ・用途を検証する(view: 非空のSRV, viewFormat: SRV形式, textureFormat: 画像形式, requiredBindFlags: 必須の用途フラグ)。
        const auto validateTexture2D = [width, height](
            ID3D11ShaderResourceView* const view,
            const DXGI_FORMAT viewFormat,
            const DXGI_FORMAT textureFormat,
            const UINT requiredBindFlags)
        {
            // 履歴SRVの形式と範囲
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
            view->GetDesc(&viewDescription);
            if (viewDescription.Format != viewFormat
                || viewDescription.ViewDimension
                    != D3D11_SRV_DIMENSION_TEXTURE2D
                || viewDescription.Texture2D.MostDetailedMip != 0
                || viewDescription.Texture2D.MipLevels != 1)
            {
                return false;
            }

            // 履歴SRVが保持する資源
            Microsoft::WRL::ComPtr<ID3D11Resource> resource;
            view->GetResource(resource.ReleaseAndGetAddressOf());
            // 検証する履歴の2D画像
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            if (resource == nullptr
                || FAILED(resource.As(&texture)))
            {
                return false;
            }
            // 履歴2D画像の寸法と用途
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            return description.Width == std::max(width, 1u)
                && description.Height == std::max(height, 1u)
                && description.MipLevels == 1
                && description.ArraySize == 1
                && description.Format == textureFormat
                && description.SampleDesc.Count == 1
                && (description.BindFlags & requiredBindFlags)
                    == requiredBindFlags;
        };
        if (!validateTexture2D(
                history,
                DXGI_FORMAT_R16G16B16A16_FLOAT,
                DXGI_FORMAT_R16G16B16A16_FLOAT,
                D3D11_BIND_SHADER_RESOURCE)
            || !validateTexture2D(
                depth,
                DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
                DXGI_FORMAT_R24G8_TYPELESS,
                D3D11_BIND_DEPTH_STENCIL
                    | D3D11_BIND_SHADER_RESOURCE))
        {
            return false;
        }

        // 今回の画像処理に渡す定数
        TemporalConstants constants{};
        constants.inverseViewProjection =
            inputs.inverseViewProjection;
        constants.previousViewProjection =
            inputs.previousViewProjection;
        constants.parameters = {
            std::clamp(settings.historyWeight, 0.0f, 0.98f),
            std::max(settings.clampTolerance, 0.0f),
            1.0f / static_cast<float>(std::max(width, 1u)),
            1.0f / static_cast<float>(std::max(height, 1u))
        };
        m_context->UpdateSubresource(
            m_temporalBuffer.Get(),
            0,
            nullptr,
            &constants,
            0,
            0);

        // 今回の画像処理の出力RTV
        ID3D11RenderTargetView* targets[]{ destination };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        // 出力画像のビューポート
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
            m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(
            m_temporalPixelShader.Get(), nullptr, 0);
        // 今回の画像処理の定数参照
        ID3D11Buffer* buffers[]{ m_temporalBuffer.Get() };
        m_context->PSSetConstantBuffers(6, 1, buffers);
        // t0は現在色、t2は深度、t4は色履歴とし、t1・t3は共通の予約枠として空ける。
        // 今回の画像処理のSRV参照
        ID3D11ShaderResourceView* resources[]{
            source,
            nullptr,
            depth,
            nullptr,
            history
        };
        m_context->PSSetShaderResources(
            0,
            static_cast<UINT>(std::size(resources)),
            resources);
        // 画像処理用のサンプラー参照
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(), 0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);

        // 同じ画像を次のパスで描画先にできるよう、読み取り参照を解除する。
        // 読み取り参照の解除用配列
        ID3D11ShaderResourceView* nullResources[5]{};
        m_context->PSSetShaderResources(
            0,
            static_cast<UINT>(std::size(nullResources)),
            nullResources);
        return true;
    }

    bool EnvironmentRenderer::ApplyVolumetricLight(
        ID3D11ShaderResourceView* const source,
        ID3D11RenderTargetView* const destination,
        const std::uint32_t width,
        const std::uint32_t height,
        const VolumetricLightSettings& settings,
        const VolumetricInputs& inputs)
    {
        // 光の筋にはシーン深度と平行光のカスケード影が必要で、未準備なら描画を省く。
        if (!settings.enabled
            || settings.intensity <= 0.0f
            || source == nullptr
            || destination == nullptr
            || inputs.cascadeCount == 0
            || inputs.cascadeCount > 4u
            || m_backend == nullptr
            || !std::isfinite(inputs.shadowResolution)
            || inputs.shadowResolution < 1.0f
            || inputs.shadowResolution
                > static_cast<float>(
                    D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION))
        {
            return false;
        }

        // 解決したシーン深度SRV
        ID3D11ShaderResourceView* depth{};
        // 解決したカスケード影SRV
        ID3D11ShaderResourceView* cascadeShadow{};
        try
        {
            depth = m_backend->ResolveShaderResourceView(inputs.depth);
            cascadeShadow = m_backend->ResolveShaderResourceView(
                inputs.cascadeShadow);
        }
        catch (const std::exception&)
        {
            return false;
        }
        if (depth == nullptr || cascadeShadow == nullptr)
        {
            return false;
        }

        // 深度SRVの形式と範囲
        D3D11_SHADER_RESOURCE_VIEW_DESC depthView{};
        depth->GetDesc(&depthView);
        // シーン深度が保持する資源
        Microsoft::WRL::ComPtr<ID3D11Resource> depthResource;
        depth->GetResource(depthResource.ReleaseAndGetAddressOf());
        // 検証するシーン深度画像
        Microsoft::WRL::ComPtr<ID3D11Texture2D> depthTexture;
        if (depthView.Format != DXGI_FORMAT_R24_UNORM_X8_TYPELESS
            || depthView.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D
            || depthView.Texture2D.MostDetailedMip != 0
            || depthView.Texture2D.MipLevels != 1
            || depthResource == nullptr
            || FAILED(depthResource.As(&depthTexture)))
        {
            return false;
        }
        // シーン深度の寸法と用途
        D3D11_TEXTURE2D_DESC depthDescription{};
        depthTexture->GetDesc(&depthDescription);
        if (depthDescription.Width != std::max(width, 1u)
            || depthDescription.Height != std::max(height, 1u)
            || depthDescription.MipLevels != 1
            || depthDescription.ArraySize != 1
            || depthDescription.Format != DXGI_FORMAT_R24G8_TYPELESS
            || depthDescription.SampleDesc.Count != 1
            || (depthDescription.BindFlags
                & D3D11_BIND_DEPTH_STENCIL) == 0
            || (depthDescription.BindFlags
                & D3D11_BIND_SHADER_RESOURCE) == 0)
        {
            return false;
        }

        // 整数に丸めた影の解像度
        const auto roundedShadowResolution =
            std::round(inputs.shadowResolution);
        if (std::abs(
                static_cast<double>(inputs.shadowResolution)
                    - roundedShadowResolution) > 0.0001)
        {
            return false;
        }
        // カスケード影のSRV設定
        D3D11_SHADER_RESOURCE_VIEW_DESC shadowView{};
        cascadeShadow->GetDesc(&shadowView);
        // カスケード影が保持する資源
        Microsoft::WRL::ComPtr<ID3D11Resource> shadowResource;
        cascadeShadow->GetResource(
            shadowResource.ReleaseAndGetAddressOf());
        // 検証するカスケード影画像
        Microsoft::WRL::ComPtr<ID3D11Texture2D> shadowTexture;
        if (shadowView.Format != DXGI_FORMAT_R32_FLOAT
            || shadowView.ViewDimension
                != D3D11_SRV_DIMENSION_TEXTURE2DARRAY
            || shadowView.Texture2DArray.MostDetailedMip != 0
            || shadowView.Texture2DArray.MipLevels != 1
            || shadowView.Texture2DArray.FirstArraySlice != 0
            || shadowView.Texture2DArray.ArraySize
                < inputs.cascadeCount
            || shadowView.Texture2DArray.ArraySize
                > 4u
            || shadowResource == nullptr
            || FAILED(shadowResource.As(&shadowTexture)))
        {
            return false;
        }
        // 影画像の寸法と用途
        D3D11_TEXTURE2D_DESC shadowDescription{};
        shadowTexture->GetDesc(&shadowDescription);
        // 検証済みの影の整数解像度
        const auto shadowResolution =
            static_cast<std::uint32_t>(roundedShadowResolution);
        if (shadowDescription.Width != shadowResolution
            || shadowDescription.Height != shadowResolution
            || shadowDescription.MipLevels != 1
            || shadowDescription.ArraySize
                != shadowView.Texture2DArray.ArraySize
            || shadowDescription.Format != DXGI_FORMAT_R32_TYPELESS
            || shadowDescription.SampleDesc.Count != 1
            || (shadowDescription.MiscFlags
                & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0
            || (shadowDescription.BindFlags
                & D3D11_BIND_DEPTH_STENCIL) == 0
            || (shadowDescription.BindFlags
                & D3D11_BIND_SHADER_RESOURCE) == 0)
        {
            return false;
        }

        // 今回の画像処理に渡す定数
        VolumetricConstants constants{};
        constants.inverseViewProjection =
            inputs.inverseViewProjection;
        constants.cameraPosition = {
            inputs.cameraPosition.x,
            inputs.cameraPosition.y,
            inputs.cameraPosition.z,
            std::max(settings.maximumDistance, 0.1f)
        };
        constants.lightDirection = {
            inputs.lightDirection.x,
            inputs.lightDirection.y,
            inputs.lightDirection.z,
            static_cast<float>(
                std::clamp<std::uint32_t>(
                    settings.sampleCount,
                    1u,
                    128u))
        };
        constants.lightColor = {
            inputs.lightColor.x * settings.intensity,
            inputs.lightColor.y * settings.intensity,
            inputs.lightColor.z * settings.intensity,
            std::clamp(settings.scattering, 0.0f, 0.95f)
        };
        constants.cascades = inputs.cascadeViewProjections;
        constants.shadowParameters = {
            static_cast<float>(
                std::min<std::uint32_t>(
                    inputs.cascadeCount,
                    4u)),
            inputs.shadowBias,
            1.0f / std::max(
                inputs.shadowResolution,
                1.0f),
            0.0f
        };
        m_context->UpdateSubresource(
            m_volumetricBuffer.Get(),
            0,
            nullptr,
            &constants,
            0,
            0);

        // 今回の画像処理の出力RTV
        ID3D11RenderTargetView* targets[]{ destination };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        // 出力画像のビューポート
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
            m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(
            m_volumetricPixelShader.Get(), nullptr, 0);
        // 今回の画像処理の定数参照
        ID3D11Buffer* buffers[]{
            m_volumetricBuffer.Get()
        };
        m_context->PSSetConstantBuffers(5, 1, buffers);
        // t0は現在色、t2は深度、t3はカスケード影とし、t1は空のキューブ画像用に空ける。
        // 今回の画像処理のSRV参照
        ID3D11ShaderResourceView* resources[]{
            source,
            nullptr,
            depth,
            cascadeShadow
        };
        m_context->PSSetShaderResources(
            0,
            static_cast<UINT>(std::size(resources)),
            resources);
        // 画像処理用のサンプラー参照
        ID3D11SamplerState* samplers[]{
            m_sampler.Get(),
            m_volumetricShadowSampler.Get()
        };
        m_context->PSSetSamplers(0, 2, samplers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(), 0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);

        // 同じ画像を次のパスで描画先にできるよう、読み取り参照を解除する。
        // 読み取り参照の解除用配列
        ID3D11ShaderResourceView* nullResources[4]{};
        m_context->PSSetShaderResources(
            0,
            static_cast<UINT>(std::size(nullResources)),
            nullResources);
        return true;
    }

    bool EnvironmentRenderer::ApplyDepthOfField(
        ID3D11ShaderResourceView* const source,
        ID3D11RenderTargetView* const destination,
        const std::uint32_t width,
        const std::uint32_t height,
        const DepthOfFieldSettings& settings,
        const DepthOfFieldInputs& inputs)
    {
        if (!settings.enabled
            || settings.maximumRadius <= 0.0f
            || source == nullptr
            || destination == nullptr
            || inputs.depth == nullptr
            || inputs.prepareTarget == nullptr
            || inputs.prepareResource == nullptr
            || inputs.blurTarget == nullptr
            || inputs.blurResource == nullptr
            || inputs.halfWidth == 0
            || inputs.halfHeight == 0)
        {
            return false;
        }
        // 射影の11・22成分がゼロに近い場合は、被写界深度の描画を省く。
        if (std::abs(inputs.projection._11) < 1e-6f
            || std::abs(inputs.projection._22) < 1e-6f)
        {
            return false;
        }

        // 全解像度の出力画像幅
        const float fullWidth =
            static_cast<float>(std::max(width, 1u));
        // 全解像度の出力画像高
        const float fullHeight =
            static_cast<float>(std::max(height, 1u));
        // 半解像度の作業画像幅
        const float halfWidth =
            static_cast<float>(inputs.halfWidth);
        // 半解像度の作業画像高
        const float halfHeight =
            static_cast<float>(inputs.halfHeight);

        // 今回の画像処理に渡す定数
        DepthOfFieldConstants constants{};
        constants.parameters = {
            std::max(settings.focusDistance, 0.01f),
            std::clamp(settings.focusRange, 0.0f, 1000.0f),
            std::clamp(settings.blurStrength, 0.0f, 8.0f),
            std::clamp(settings.maximumRadius, 0.0f, 32.0f)
        };
        constants.projection = {
            inputs.projection._33,
            inputs.projection._43,
            0.0f,
            0.0f
        };
        // 4～64に補正したぼかし採取数
        const float sampleCount = static_cast<float>(
            std::clamp<std::uint32_t>(
                inputs.sampleCount,
                4u,
                64u));

        // 半解像度へ色と符号付きのぼけ量を準備する。
        constants.texel = {
            1.0f / halfWidth,
            1.0f / halfHeight,
            sampleCount,
            0.0f
        };
        m_context->UpdateSubresource(
            m_depthOfFieldBuffer.Get(),
            0,
            nullptr,
            &constants,
            0,
            0);
        DrawDepthOfFieldPass(
            m_depthOfFieldPreparePixelShader.Get(),
            source,
            inputs.depth,
            nullptr,
            inputs.prepareTarget,
            halfWidth,
            halfHeight);

        // 準備時の半解像度定数を再利用して円形にぼかす。
        DrawDepthOfFieldPass(
            m_depthOfFieldBlurPixelShader.Get(),
            nullptr,
            nullptr,
            inputs.prepareResource,
            inputs.blurTarget,
            halfWidth,
            halfHeight);

        // 画像の逆寸法だけ全解像度へ戻して合成する。
        constants.texel = {
            1.0f / fullWidth,
            1.0f / fullHeight,
            sampleCount,
            0.0f
        };
        m_context->UpdateSubresource(
            m_depthOfFieldBuffer.Get(),
            0,
            nullptr,
            &constants,
            0,
            0);
        DrawDepthOfFieldPass(
            m_depthOfFieldCompositePixelShader.Get(),
            source,
            inputs.depth,
            inputs.blurResource,
            destination,
            fullWidth,
            fullHeight);
        return true;
    }

    void EnvironmentRenderer::DrawDepthOfFieldPass(
        ID3D11PixelShader* const pixelShader,
        ID3D11ShaderResourceView* const source,
        ID3D11ShaderResourceView* const depth,
        ID3D11ShaderResourceView* const work,
        ID3D11RenderTargetView* const destination,
        const float width,
        const float height)
    {
        if (pixelShader == nullptr || destination == nullptr)
        {
            return;
        }
        // 今回の画像処理の出力RTV
        ID3D11RenderTargetView* targets[]{ destination };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        // 出力画像のビューポート
        const D3D11_VIEWPORT viewport{
            0.0f,
            0.0f,
            width,
            height,
            0.0f,
            1.0f
        };
        m_context->RSSetViewports(1, &viewport);
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(
            m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(pixelShader, nullptr, 0);
        // 今回の画像処理の定数参照
        ID3D11Buffer* buffers[]{
            m_depthOfFieldBuffer.Get()
        };
        m_context->PSSetConstantBuffers(8, 1, buffers);
        // t0は現在色、t2は深度とし、t1は空のキューブ画像用に空ける。
        // 今回の画像処理のSRV参照
        ID3D11ShaderResourceView* resources[]{
            source,
            nullptr,
            depth
        };
        m_context->PSSetShaderResources(
            0,
            static_cast<UINT>(std::size(resources)),
            resources);
        // t6へ渡すぼかし作業画像
        ID3D11ShaderResourceView* workResources[]{ work };
        m_context->PSSetShaderResources(6, 1, workResources);
        // 画像処理用のサンプラー参照
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(), 0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);

        // 次のパスで同じ画像を描画先にできるよう、全ての読み取り参照を解除する。
        // 読み取り参照の解除用配列
        ID3D11ShaderResourceView* nullResources[3]{};
        m_context->PSSetShaderResources(
            0,
            static_cast<UINT>(std::size(nullResources)),
            nullResources);
        m_context->PSSetShaderResources(6, 1, nullResources);
    }

    bool EnvironmentRenderer::ApplyMotionBlur(
        ID3D11ShaderResourceView* const source,
        ID3D11RenderTargetView* const destination,
        const std::uint32_t width,
        const std::uint32_t height,
        const MotionBlurSettings& settings,
        const MotionBlurInputs& inputs)
    {
        // 前フレームの行列がない場合は、ブレの方向を求めず描画を省く。
        if (!settings.enabled
            || settings.intensity <= 0.0f
            || settings.maximumRadius <= 0.0f
            || source == nullptr
            || destination == nullptr
            || inputs.depth == nullptr
            || !inputs.previousValid)
        {
            return false;
        }

        // 1画素以上に補正した出力幅
        const float safeWidth =
            static_cast<float>(std::max(width, 1u));
        // 1画素以上に補正した出力高
        const float safeHeight =
            static_cast<float>(std::max(height, 1u));

        // 今回の画像処理に渡す定数
        MotionBlurConstants constants{};
        constants.inverseViewProjection =
            inputs.inverseViewProjection;
        constants.previousViewProjection =
            inputs.previousViewProjection;
        constants.parameters = {
            std::clamp(settings.intensity, 0.0f, 4.0f),
            std::clamp(settings.maximumRadius, 0.0f, 64.0f),
            static_cast<float>(
                std::clamp<std::uint32_t>(
                    inputs.sampleCount,
                    2u,
                    32u)),
            0.0f
        };
        constants.texel = {
            1.0f / safeWidth,
            1.0f / safeHeight,
            0.0f,
            0.0f
        };
        m_context->UpdateSubresource(
            m_motionBlurBuffer.Get(),
            0,
            nullptr,
            &constants,
            0,
            0);

        // 今回の画像処理の出力RTV
        ID3D11RenderTargetView* targets[]{ destination };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        // 出力画像のビューポート
        const D3D11_VIEWPORT viewport{
            0.0f,
            0.0f,
            safeWidth,
            safeHeight,
            0.0f,
            1.0f
        };
        m_context->RSSetViewports(1, &viewport);
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(
            m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(
            m_motionBlurPixelShader.Get(), nullptr, 0);
        // 今回の画像処理の定数参照
        ID3D11Buffer* buffers[]{ m_motionBlurBuffer.Get() };
        m_context->PSSetConstantBuffers(9, 1, buffers);
        // t0は現在色、t2は深度とし、t1は空のキューブ画像用に空ける。
        // 今回の画像処理のSRV参照
        ID3D11ShaderResourceView* resources[]{
            source,
            nullptr,
            inputs.depth
        };
        m_context->PSSetShaderResources(
            0,
            static_cast<UINT>(std::size(resources)),
            resources);
        // 画像処理用のサンプラー参照
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(), 0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);

        // 読み取り参照の解除用配列
        ID3D11ShaderResourceView* nullResources[3]{};
        m_context->PSSetShaderResources(
            0,
            static_cast<UINT>(std::size(nullResources)),
            nullResources);
        return true;
    }

    void EnvironmentRenderer::RenderLuminance(
        ID3D11ShaderResourceView* const source,
        ID3D11RenderTargetView* const destination,
        ID3D11ShaderResourceView* const resource,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        if (source == nullptr
            || destination == nullptr
            || resource == nullptr)
        {
            return;
        }

        // 1画素以上に補正した出力幅
        const float safeWidth =
            static_cast<float>(std::max(width, 1u));
        // 1画素以上に補正した出力高
        const float safeHeight =
            static_cast<float>(std::max(height, 1u));
        // 今回の画像処理に渡す定数
        const LuminanceConstants constants{
            DirectX::XMFLOAT4{
                1.0f / safeWidth,
                1.0f / safeHeight,
                0.0f,
                0.0f
            }
        };
        m_context->UpdateSubresource(
            m_luminanceBuffer.Get(),
            0,
            nullptr,
            &constants,
            0,
            0);

        // 今回の画像処理の出力RTV
        ID3D11RenderTargetView* targets[]{ destination };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        // 出力画像のビューポート
        const D3D11_VIEWPORT viewport{
            0.0f,
            0.0f,
            safeWidth,
            safeHeight,
            0.0f,
            1.0f
        };
        m_context->RSSetViewports(1, &viewport);
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(
            m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(
            m_luminancePixelShader.Get(), nullptr, 0);
        // 今回の画像処理の定数参照
        ID3D11Buffer* buffers[]{ m_luminanceBuffer.Get() };
        m_context->PSSetConstantBuffers(10, 1, buffers);
        // 今回の画像処理のSRV参照
        ID3D11ShaderResourceView* resources[]{ source };
        m_context->PSSetShaderResources(0, 1, resources);
        // 画像処理用のサンプラー参照
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(), 0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);

        // 読み取り参照の解除用配列
        ID3D11ShaderResourceView* nullResource[]{ nullptr };
        m_context->PSSetShaderResources(0, 1, nullResource);
        // ミップ生成で同じ画像を読むため、測定画像を描画先から外す。
        // 描画先の解除用配列
        ID3D11RenderTargetView* noTargets[]{ nullptr };
        m_context->OMSetRenderTargets(1, noTargets, nullptr);
        // 各段の2×2平均から対数輝度の最終ミップを求める。
        m_context->GenerateMips(resource);
    }

    void EnvironmentRenderer::BuildReflectionDepthPyramid(
        RenderTarget& target,
        const float projectionZ,
        const float projectionW)
    {
        // フレーム途中のHi-Z生成後も描画を続けられるよう、主RTV・深度・ビューポートを保存する。
        // 同じ機器の画像描画先状態
        const auto* const targetState = dynamic_cast<const
            Detail::D3D11RenderTargetState*>(
                Detail::RenderTargetBackendAccess::Get(target));
        if (targetState == nullptr
            || !targetState->IsValid()
            || targetState->m_ownerDevice.Get() != m_device)
        {
            return;
        }
        // Hi-Zピラミッドのミップ数
        const auto mipCount =
            targetState->m_reflectionDepthPyramidMipCount;
        // 距離へ変換する深度コピー
        auto* const rawDepth =
            targetState->m_depthCopyShaderResourceView.Get();
        if (mipCount == 0
            || rawDepth == nullptr
            || targetState->m_reflectionDepthPyramidTargets.size()
                < mipCount
            || targetState->m_reflectionDepthPyramidMipViews.size()
                < mipCount)
        {
            return;
        }

        // SSR用のt21・t22を外して読み書きの競合を避け、次のLit描画で再設定する。
        {
            // SSR読み取り参照の解除
            ID3D11ShaderResourceView* nullReflection[]{
                nullptr, nullptr };
            m_context->PSSetShaderResources(
                21,
                static_cast<UINT>(
                    std::size(nullReflection)),
                nullReflection);
        }

        // 保存する主描画先のRTV
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            previousTarget;
        // 保存する深度ビュー
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView>
            previousDepth;
        m_context->OMGetRenderTargets(
            1,
            previousTarget.ReleaseAndGetAddressOf(),
            previousDepth.ReleaseAndGetAddressOf());
        // 保存する主ビューポート
        D3D11_VIEWPORT previousViewport{};
        // 保存したビューポートの数
        UINT previousViewportCount = 1;
        m_context->RSGetViewports(
            &previousViewportCount,
            &previousViewport);
        // 保存する深度・ステンシル状態
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState>
            previousDepthState;
        // 保存するステンシル参照値
        UINT previousStencilReference{};
        m_context->OMGetDepthStencilState(
            previousDepthState.ReleaseAndGetAddressOf(),
            &previousStencilReference);
        // 保存するラスタライザー状態
        Microsoft::WRL::ComPtr<ID3D11RasterizerState>
            previousRasterizer;
        m_context->RSGetState(
            previousRasterizer.ReleaseAndGetAddressOf());

        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(
            m_vertexShader.Get(), nullptr, 0);
        // Hi-Z処理用のサンプラー
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        // b3へ渡すHi-Z処理定数
        ID3D11Buffer* buffers[]{ m_prefilterBuffer.Get() };
        m_context->PSSetConstantBuffers(3, 1, buffers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(), 0);
        m_context->RSSetState(m_rasterizer.Get());

        // Hi-Zの1段を描く(shader: 距離化または縮小PS, source: 入力SRV, destination: 出力RTV, width: 出力幅, height: 出力高, parameterX: 射影33または入力幅, parameterY: 射影43または入力高)。
        const auto runPass =
            [this](
                ID3D11PixelShader* shader,
                ID3D11ShaderResourceView* source,
                ID3D11RenderTargetView* destination,
                const std::uint32_t width,
                const std::uint32_t height,
                const float parameterX,
                const float parameterY)
        {
            // 今回のHi-Z段の処理定数
            PrefilterConstants constants{};
            constants.parameters = {
                parameterX,
                parameterY,
                0.0f,
                0.0f
            };
            m_context->UpdateSubresource(
                m_prefilterBuffer.Get(),
                0,
                nullptr,
                &constants,
                0,
                0);
            // 今回のHi-Z段の出力RTV
            ID3D11RenderTargetView* targets[]{ destination };
            m_context->OMSetRenderTargets(
                1, targets, nullptr);
            // 今回のHi-Z段のビューポート
            const D3D11_VIEWPORT viewport{
                0.0f,
                0.0f,
                static_cast<float>(std::max(width, 1u)),
                static_cast<float>(std::max(height, 1u)),
                0.0f,
                1.0f
            };
            m_context->RSSetViewports(1, &viewport);
            m_context->PSSetShader(shader, nullptr, 0);
            // 今回のHi-Z段の入力SRV
            ID3D11ShaderResourceView* resources[]{ source };
            m_context->PSSetShaderResources(0, 1, resources);
            m_context->Draw(3, 0);
            // 次のパスで同じ画像を読めるよう、入力SRVを解除する。
            // 入力SRVの解除用配列
            ID3D11ShaderResourceView* nullResource[]{
                nullptr };
            m_context->PSSetShaderResources(
                0, 1, nullResource);
        };

        // 元のシーン深度の画像幅
        const std::uint32_t width = targetState->m_width;
        // 元のシーン深度の画像高
        const std::uint32_t height = targetState->m_height;
        runPass(
            m_reflectionLinearizePixelShader.Get(),
            rawDepth,
            targetState->m_reflectionDepthPyramidTargets[0].Get(),
            width,
            height,
            projectionZ,
            projectionW);
        // 縮小するHi-Z段の番号
        for (std::uint32_t mip = 1; mip < mipCount; ++mip)
        {
            // 縮小元のミップ画像幅
            const std::uint32_t parentWidth =
                std::max(width >> (mip - 1), 1u);
            // 縮小元のミップ画像高
            const std::uint32_t parentHeight =
                std::max(height >> (mip - 1), 1u);
            runPass(
                m_reflectionDownsamplePixelShader.Get(),
                targetState->m_reflectionDepthPyramidMipViews[
                    mip - 1].Get(),
                targetState->m_reflectionDepthPyramidTargets[mip].Get(),
                std::max(width >> mip, 1u),
                std::max(height >> mip, 1u),
                static_cast<float>(parentWidth),
                static_cast<float>(parentHeight));
        }


        // 復元する主描画先のRTV
        ID3D11RenderTargetView* restoreTargets[]{
            previousTarget.Get() };
        m_context->OMSetRenderTargets(
            1, restoreTargets, previousDepth.Get());
        if (previousViewportCount > 0)
        {
            m_context->RSSetViewports(1, &previousViewport);
        }
        m_context->OMSetDepthStencilState(
            previousDepthState.Get(),
            previousStencilReference);
        m_context->RSSetState(previousRasterizer.Get());
    }

    bool EnvironmentRenderer::ProjectIrradianceToSh(
        ID3D11ShaderResourceView* const irradiance,
        std::array<float, 12>& coefficients)
    {
        using Microsoft::WRL::ComPtr;
        if (irradiance == nullptr)
        {
            return false;
        }
        // 拡散SRVが保持する画像資源
        ComPtr<ID3D11Resource> resource;
        irradiance->GetResource(resource.ReleaseAndGetAddressOf());
        // 読み戻す6面の拡散画像
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(resource.As(&texture)))
        {
            return false;
        }
        // 拡散画像の形式と寸法
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        if (description.Format != DXGI_FORMAT_R16G16B16A16_FLOAT
            || description.ArraySize != 6)
        {
            return false;
        }
        // CPU読み取り用の画像設定
        D3D11_TEXTURE2D_DESC staging = description;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging.MiscFlags = 0;
        // CPUへ読み戻す画像のコピー
        ComPtr<ID3D11Texture2D> copy;
        if (FAILED(m_device->CreateTexture2D(
            &staging,
            nullptr,
            copy.ReleaseAndGetAddressOf())))
        {
            return false;
        }
        m_context->CopyResource(copy.Get(), texture.Get());

        // キューブ面の一辺の画素数
        const std::uint32_t edge = description.Width;
        // RGB各4要素のSH積分値
        double sums[12]{};
        // 読み戻すキューブ面の番号
        for (std::uint32_t face = 0; face < 6; ++face)
        {
            // 各面のミップ0の副資源番号
            const UINT subresource = D3D11CalcSubresource(
                0, face, description.MipLevels);
            // CPU読み取り用のマップ情報
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(m_context->Map(
                copy.Get(),
                subresource,
                D3D11_MAP_READ,
                0,
                &mapped)))
            {
                return false;
            }
            // 読み戻す画素の行番号
            for (std::uint32_t y = 0; y < edge; ++y)
            {
                // 行ピッチを適用した半精度画素
                const auto* row = reinterpret_cast<
                    const DirectX::PackedVector::HALF*>(
                    static_cast<const std::uint8_t*>(mapped.pData)
                    + y * mapped.RowPitch);
                // 読み戻す画素の列番号
                for (std::uint32_t x = 0; x < edge; ++x)
                {
                    // HLSLのCubeDirectionと同じ面規約で、画素中心の方向を積分する。
                    // 面上の横方向の-1～1座標
                    const float uc =
                        (static_cast<float>(x) + 0.5f)
                            / edge * 2.0f
                        - 1.0f;
                    // 面上の縦方向の-1～1座標
                    const float vc =
                        (static_cast<float>(y) + 0.5f)
                            / edge * 2.0f
                        - 1.0f;
                    // 面規約から求めた未正規化方向
                    DirectX::XMFLOAT3 direction{};
                    switch (face)
                    {
                    case 0:
                        direction = { 1.0f, -vc, -uc };
                        break;
                    case 1:
                        direction = { -1.0f, -vc, uc };
                        break;
                    case 2:
                        direction = { uc, 1.0f, vc };
                        break;
                    case 3:
                        direction = { uc, -1.0f, -vc };
                        break;
                    case 4:
                        direction = { uc, -vc, 1.0f };
                        break;
                    default:
                        direction = { -uc, -vc, -1.0f };
                        break;
                    }
                    // 方向ベクトルの長さの2乗
                    const float lengthSquared =
                        direction.x * direction.x
                        + direction.y * direction.y
                        + direction.z * direction.z;
                    // 方向ベクトルの長さ
                    const float length = std::sqrt(lengthSquared);
                    // 画素が占める近似立体角
                    const float solidAngle =
                        4.0f / (edge * edge)
                        / (lengthSquared * length);
                    // 正規化した方向のX成分
                    const float nx = direction.x / length;
                    // 正規化した方向のY成分
                    const float ny = direction.y / length;
                    // 正規化した方向のZ成分
                    const float nz = direction.z / length;
                    // 積分するRGBチャンネル番号
                    for (int channel = 0; channel < 3; ++channel)
                    {
                        // 半精度から戻した画素成分
                        const float value =
                            DirectX::PackedVector::
                                XMConvertHalfToFloat(
                                    row[x * 4 + channel]);
                        // 画素値に立体角を掛けた重み
                        const double weighted =
                            static_cast<double>(value) * solidAngle;
                        // 今回の色のSH4要素の先頭
                        double* base = sums + channel * 4;
                        base[0] += weighted * nx;
                        base[1] += weighted * ny;
                        base[2] += weighted * nz;
                        base[3] += weighted;
                    }
                }
            }
            m_context->Unmap(copy.Get(), subresource);
        }
        // 一次のSH係数の正規化倍率
        constexpr double AxisScale =
            3.0 / (4.0 * 3.14159265358979323846);
        // 定数のSH係数の正規化倍率
        constexpr double ConstantScale =
            1.0 / (4.0 * 3.14159265358979323846);
        // 積分するRGBチャンネル番号
        for (int channel = 0; channel < 3; ++channel)
        {
            coefficients[channel * 4 + 0] = static_cast<float>(
                sums[channel * 4 + 0] * AxisScale);
            coefficients[channel * 4 + 1] = static_cast<float>(
                sums[channel * 4 + 1] * AxisScale);
            coefficients[channel * 4 + 2] = static_cast<float>(
                sums[channel * 4 + 2] * AxisScale);
            coefficients[channel * 4 + 3] = static_cast<float>(
                sums[channel * 4 + 3] * ConstantScale);
        }
        return true;
    }

    void EnvironmentRenderer::CopyMirroredX(
        ID3D11ShaderResourceView* source,
        ID3D11RenderTargetView* destination,
        const std::uint32_t destinationWidth,
        const std::uint32_t destinationHeight)
    {
        if (source == nullptr || destination == nullptr)
        {
            return;
        }
        // コピー先のRTV参照
        ID3D11RenderTargetView* targets[]{ destination };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        // コピー先のビューポート
        const D3D11_VIEWPORT viewport{
            0.0f,
            0.0f,
            static_cast<float>(
                std::max(destinationWidth, 1u)),
            static_cast<float>(
                std::max(destinationHeight, 1u)),
            0.0f,
            1.0f
        };
        m_context->RSSetViewports(1, &viewport);
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(
            m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(
            m_copyMirrorPixelShader.Get(), nullptr, 0);
        // コピー元のSRV参照
        ID3D11ShaderResourceView* resources[]{ source };
        m_context->PSSetShaderResources(0, 1, resources);
        // コピー用のサンプラー参照
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(), 0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);
        // コピー元のSRVの解除
        ID3D11ShaderResourceView* nullResource[]{ nullptr };
        m_context->PSSetShaderResources(0, 1, nullResource);
    }

    void EnvironmentRenderer::Copy(
        ID3D11ShaderResourceView* source,
        ID3D11RenderTargetView* destination,
        const std::uint32_t destinationWidth,
        const std::uint32_t destinationHeight)
    {
        if (source == nullptr || destination == nullptr)
        {
            return;
        }
        // コピー先のRTV参照
        ID3D11RenderTargetView* targets[]{ destination };
        m_context->OMSetRenderTargets(1, targets, nullptr);
        // コピー先のビューポート
        const D3D11_VIEWPORT viewport{
            0.0f,
            0.0f,
            static_cast<float>(
                std::max(destinationWidth, 1u)),
            static_cast<float>(
                std::max(destinationHeight, 1u)),
            0.0f,
            1.0f
        };
        m_context->RSSetViewports(1, &viewport);
        CopyToBoundRenderTarget(source);
    }

    void EnvironmentRenderer::CopyToBoundRenderTarget(
        ID3D11ShaderResourceView* source)
    {
        if (source == nullptr)
        {
            return;
        }
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(
            m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(
            m_copyPixelShader.Get(), nullptr, 0);
        // コピー元のSRV参照
        ID3D11ShaderResourceView* resources[]{ source };
        m_context->PSSetShaderResources(0, 1, resources);
        // コピー用のサンプラー参照
        ID3D11SamplerState* samplers[]{ m_sampler.Get() };
        m_context->PSSetSamplers(0, 1, samplers);
        m_context->OMSetDepthStencilState(
            m_depthDisabled.Get(), 0);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->Draw(3, 0);
        // コピー元のSRVの解除
        ID3D11ShaderResourceView* nullResource[]{ nullptr };
        m_context->PSSetShaderResources(0, 1, nullResource);
    }
}
