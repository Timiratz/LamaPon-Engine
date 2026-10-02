#include "LamaPon/Graphics/GraphicsRenderServices.h"

#include "LamaPon/Graphics/GraphicsBackend.h"
#include "LamaPon/Graphics/GraphicsDeviceD3D11Resources.h"

#include <CommonStates.h>
#include <Effects.h>
#include <PrimitiveBatch.h>
#include <VertexTypes.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>

namespace
{
    // 描画できる最大粒子数
    constexpr std::size_t MaximumParticleCount = 4096;

    class D3D11RenderServices final
        : public LamaPon::GraphicsRenderServices
    {
    public:
        // 描画用デバイスを保持する(device: 描画デバイス, context: 描画コンテキスト, backend: 寿命が長い基盤)。
        D3D11RenderServices(
            ID3D11Device* const device,
            ID3D11DeviceContext* const context,
            LamaPon::GraphicsBackend& backend)
            : m_device(device)
            , m_context(context)
            , m_backend(backend)
        {
        }

        // 四頂点単位の粒子を描画する(request: 粒子と描画条件)。
        // 終了時は既定の混合・深度・カリング状態へ戻し、直前の状態は復元しない。
        [[nodiscard]] bool DrawParticles(
            const LamaPon::ParticleDrawRequest& request) override
        {
            if (request.vertices.empty())
            {
                return true;
            }
            if (request.vertices.size() % 4 != 0
                || request.vertices.size()
                    > MaximumParticleCount * 4)
            {
                throw std::invalid_argument(
                    "Particle draw requests require complete quads within "
                    "the service capacity.");
            }
            EnsureParticleResources();

            // 混合係数
            constexpr float blendFactor[]{
                0.0f,
                0.0f,
                0.0f,
                0.0f
            };
            // 独自シェーダー適用済み
            bool customShaderApplied{};
            // 描画バッチ開始済み
            bool batchBegun{};
            // 独自入力を解除して既定の描画状態へ戻す。
            const auto restorePipeline = [&]() noexcept
            {
                if (customShaderApplied)
                {
                    // テクスチャ解除用参照
                    const std::array<
                        LamaPon::GraphicsViewHandle,
                        2> emptyViews{};
                    static_cast<void>(
                        m_backend.TryBindPixelShaderResources(
                            0,
                            emptyViews,
                            {}));
                }
                m_context->OMSetBlendState(
                    m_opaqueBlendState,
                    blendFactor,
                    0xffffffffu);
                m_context->OMSetDepthStencilState(
                    m_depthDefaultState,
                    0);
                m_context->RSSetState(
                    m_cullCounterClockwiseState);
            };

            try
            {
                m_context->OMSetBlendState(
                    request.additive
                        ? m_additiveBlendState
                        : m_nonPremultipliedBlendState,
                    blendFactor,
                    0xffffffffu);
                m_context->OMSetDepthStencilState(
                    m_depthReadState,
                    0);
                m_context->RSSetState(m_cullNoneState);
                // 粒子用サンプラー
                ID3D11SamplerState* samplers[]{
                    m_linearWrapSampler
                };
                m_context->PSSetSamplers(0, 1, samplers);

                m_effect->SetWorld(DirectX::XMMatrixIdentity());
                m_effect->SetView(
                    DirectX::XMLoadFloat4x4(&request.view));
                m_effect->SetProjection(
                    DirectX::XMLoadFloat4x4(&request.projection));
                // 前回のテクスチャをBasicEffectが再設定しないよう、適用前に参照を外す。
                m_effect->SetTexture(nullptr);
                m_effect->Apply(m_context.Get());
                m_context->IASetInputLayout(m_inputLayout.Get());

                customShaderApplied = request.applyCustomPixelShader
                    && request.applyCustomPixelShader();
                // 粒子のテクスチャ参照
                const std::array textureViews{
                    request.texture,
                    request.auxiliaryTexture
                };
                // 実際に設定する参照範囲
                const auto boundViews = customShaderApplied
                    ? std::span<const LamaPon::GraphicsViewHandle>{
                        textureViews }
                    : std::span<const LamaPon::GraphicsViewHandle>{
                        textureViews.data(), 1 };
                if (!m_backend.TryBindPixelShaderResources(
                        0,
                        boundViews,
                        request.fallbackTexture))
                {
                    restorePipeline();
                    return false;
                }

                m_batch->Begin();
                batchBegun = true;
                // 四角形の先頭頂点番号
                for (std::size_t first{};
                    first < request.vertices.size();
                    first += 4)
                {
                    // 粒子頂点を標準頂点へ変換する(source: 元の粒子頂点)。
                    const auto makeVertex = [](const auto& source)
                    {
                        return DirectX::VertexPositionColorTexture{
                            source.position,
                            source.color,
                            source.textureCoordinate
                        };
                    };
                    m_batch->DrawQuad(
                        makeVertex(request.vertices[first]),
                        makeVertex(request.vertices[first + 1]),
                        makeVertex(request.vertices[first + 2]),
                        makeVertex(request.vertices[first + 3]));
                }
                m_batch->End();
                batchBegun = false;
            }
            catch (...)
            {
                if (batchBegun)
                {
                    try
                    {
                        m_batch->End();
                    }
                    catch (...)
                    {
                    }
                }
                restorePipeline();
                throw;
            }

            restorePipeline();
            return true;
        }

    private:
        // 初回描画時に粒子用資源を生成し、すべて成功してから保持する。
        void EnsureParticleResources()
        {
            if (m_effect != nullptr)
            {
                return;
            }

            // 粒子を使わないプロジェクトの起動に影響しないよう、資源生成を初回描画まで遅らせる。
            // 生成中の粒子シェーダー
            auto effect = std::make_unique<DirectX::BasicEffect>(
                m_device.Get());
            // 生成中の描画状態所有元
            auto states = std::make_unique<DirectX::CommonStates>(
                m_device.Get());
            // 生成中の粒子描画バッチ
            auto batch = std::make_unique<DirectX::PrimitiveBatch<
                DirectX::VertexPositionColorTexture>>(
                    m_context.Get(),
                    MaximumParticleCount * 6,
                    MaximumParticleCount * 4);
            effect->SetVertexColorEnabled(true);
            effect->SetTextureEnabled(true);

            // 復元処理で遅延生成の例外が出ないよう、CommonStatesが所有する状態を先に生成する。
            // 生成した不透明混合状態
            auto* const opaqueBlendState = states->Opaque();
            // 生成した加算混合状態
            auto* const additiveBlendState = states->Additive();
            // 生成した非乗算混合状態
            auto* const nonPremultipliedBlendState =
                states->NonPremultiplied();
            // 生成した通常深度状態
            auto* const depthDefaultState = states->DepthDefault();
            // 生成した深度読取状態
            auto* const depthReadState = states->DepthRead();
            // 生成した両面描画状態
            auto* const cullNoneState = states->CullNone();
            // 生成した通常面除去状態
            auto* const cullCounterClockwiseState =
                states->CullCounterClockwise();
            // 生成した線形循環サンプラー
            auto* const linearWrapSampler = states->LinearWrap();

            // 頂点シェーダーのバイト列
            const void* shaderByteCode{};
            // バイトコードの長さ
            std::size_t byteCodeLength{};
            effect->GetVertexShaderBytecode(
                &shaderByteCode,
                &byteCodeLength);
            // 生成中の頂点入力形式
            Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
            // 入力形式の生成結果
            const HRESULT result = m_device->CreateInputLayout(
                DirectX::VertexPositionColorTexture::InputElements,
                DirectX::VertexPositionColorTexture::InputElementCount,
                shaderByteCode,
                byteCodeLength,
                inputLayout.ReleaseAndGetAddressOf());
            if (FAILED(result))
            {
                throw std::runtime_error(
                    "Failed to create the DirectX 11 particle input layout.");
            }

            m_effect = std::move(effect);
            m_states = std::move(states);
            m_batch = std::move(batch);
            m_inputLayout = std::move(inputLayout);
            m_opaqueBlendState = opaqueBlendState;
            m_additiveBlendState = additiveBlendState;
            m_nonPremultipliedBlendState =
                nonPremultipliedBlendState;
            m_depthDefaultState = depthDefaultState;
            m_depthReadState = depthReadState;
            m_cullNoneState = cullNoneState;
            m_cullCounterClockwiseState =
                cullCounterClockwiseState;
            m_linearWrapSampler = linearWrapSampler;
        }

        // 保持する描画デバイス
        Microsoft::WRL::ComPtr<ID3D11Device> m_device;
        // 保持する描画コンテキスト
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
        // 借用する描画基盤
        LamaPon::GraphicsBackend& m_backend;
        // 粒子用標準シェーダー
        std::unique_ptr<DirectX::BasicEffect> m_effect;
        // 描画状態の所有元
        std::unique_ptr<DirectX::CommonStates> m_states;
        // 粒子の四角形描画バッチ
        std::unique_ptr<DirectX::PrimitiveBatch<
            DirectX::VertexPositionColorTexture>> m_batch;
        // 粒子頂点の入力形式
        Microsoft::WRL::ComPtr<ID3D11InputLayout> m_inputLayout;
        // 借用する不透明混合状態
        ID3D11BlendState* m_opaqueBlendState{};
        // 借用する加算混合状態
        ID3D11BlendState* m_additiveBlendState{};
        // 借用する非乗算混合状態
        ID3D11BlendState* m_nonPremultipliedBlendState{};
        // 借用する通常深度状態
        ID3D11DepthStencilState* m_depthDefaultState{};
        // 借用する深度読取状態
        ID3D11DepthStencilState* m_depthReadState{};
        // 借用する両面描画状態
        ID3D11RasterizerState* m_cullNoneState{};
        // 借用する通常面除去状態
        ID3D11RasterizerState* m_cullCounterClockwiseState{};
        // 借用する線形循環サンプラー
        ID3D11SamplerState* m_linearWrapSampler{};
    };
}

namespace LamaPon::Detail
{
    // 非空のデバイスから描画サービスを作る(device: 描画デバイス, context: 描画コンテキスト, backend: 寿命が長い基盤)。
    std::unique_ptr<GraphicsRenderServices>
        CreateD3D11GraphicsRenderServices(
            ID3D11Device* const device,
            ID3D11DeviceContext* const context,
            GraphicsBackend& backend)
    {
        if (device == nullptr || context == nullptr)
        {
            throw std::invalid_argument(
                "DirectX 11 render services require a device and context.");
        }
        return std::make_unique<D3D11RenderServices>(
            device,
            context,
            backend);
    }
}
