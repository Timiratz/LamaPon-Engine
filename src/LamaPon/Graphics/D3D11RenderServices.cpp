#include "LamaPon/Graphics/GraphicsRenderServices.h"

#include "LamaPon/Graphics/GraphicsBackend.h"
#include "LamaPon/Graphics/GraphicsDeviceApiResources.h"

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
    constexpr std::size_t MaximumParticleCount = 4096;

    class D3D11RenderServices final
        : public LamaPon::GraphicsRenderServices
    {
    public:
        D3D11RenderServices(
            ID3D11Device* const device,
            ID3D11DeviceContext* const context,
            LamaPon::GraphicsBackend& backend)
            : m_device(device)
            , m_context(context)
            , m_backend(backend)
        {
        }

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

            constexpr float blendFactor[]{
                0.0f,
                0.0f,
                0.0f,
                0.0f
            };
            bool customShaderApplied{};
            bool batchBegun{};
            const auto restorePipeline = [&]() noexcept
            {
                if (customShaderApplied)
                {
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
                ID3D11SamplerState* samplers[]{
                    m_linearWrapSampler
                };
                m_context->PSSetSamplers(0, 1, samplers);

                m_effect->SetWorld(DirectX::XMMatrixIdentity());
                m_effect->SetView(
                    DirectX::XMLoadFloat4x4(&request.view));
                m_effect->SetProjection(
                    DirectX::XMLoadFloat4x4(&request.projection));
                // 前回のraw viewをBasicEffectが再bindしないよう、textureは
                // neutral handleのbind直前に必ず外します。
                m_effect->SetTexture(nullptr);
                m_effect->Apply(m_context.Get());
                m_context->IASetInputLayout(m_inputLayout.Get());

                customShaderApplied = request.applyCustomPixelShader
                    && request.applyCustomPixelShader();
                const std::array textureViews{
                    request.texture,
                    request.auxiliaryTexture
                };
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
                for (std::size_t first{};
                    first < request.vertices.size();
                    first += 4)
                {
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
        void EnsureParticleResources()
        {
            if (m_effect != nullptr)
            {
                return;
            }

            // ParticleSystemを使わないプロジェクトの起動を追加のD3D11
            // allocation失敗へ巻き込まないよう、最初の描画まで遅延します。
            // 全てlocalで完成させてから公開し、失敗時は未初期化のままです。
            auto effect = std::make_unique<DirectX::BasicEffect>(
                m_device.Get());
            auto states = std::make_unique<DirectX::CommonStates>(
                m_device.Get());
            auto batch = std::make_unique<DirectX::PrimitiveBatch<
                DirectX::VertexPositionColorTexture>>(
                    m_context.Get(),
                    MaximumParticleCount * 6,
                    MaximumParticleCount * 4);
            effect->SetVertexColorEnabled(true);
            effect->SetTextureEnabled(true);

            // DirectXTKのstate getterは初回に遅延生成して例外を投げ得ます。
            // restorePipelineのnoexcept区間ではgetterを呼ばないよう、全state
            // をここで生成しCommonStatesの寿命中だけ使うpointerを控えます。
            auto* const opaqueBlendState = states->Opaque();
            auto* const additiveBlendState = states->Additive();
            auto* const nonPremultipliedBlendState =
                states->NonPremultiplied();
            auto* const depthDefaultState = states->DepthDefault();
            auto* const depthReadState = states->DepthRead();
            auto* const cullNoneState = states->CullNone();
            auto* const cullCounterClockwiseState =
                states->CullCounterClockwise();
            auto* const linearWrapSampler = states->LinearWrap();

            const void* shaderByteCode{};
            std::size_t byteCodeLength{};
            effect->GetVertexShaderBytecode(
                &shaderByteCode,
                &byteCodeLength);
            Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
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

        Microsoft::WRL::ComPtr<ID3D11Device> m_device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
        LamaPon::GraphicsBackend& m_backend;
        std::unique_ptr<DirectX::BasicEffect> m_effect;
        std::unique_ptr<DirectX::CommonStates> m_states;
        std::unique_ptr<DirectX::PrimitiveBatch<
            DirectX::VertexPositionColorTexture>> m_batch;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> m_inputLayout;
        ID3D11BlendState* m_opaqueBlendState{};
        ID3D11BlendState* m_additiveBlendState{};
        ID3D11BlendState* m_nonPremultipliedBlendState{};
        ID3D11DepthStencilState* m_depthDefaultState{};
        ID3D11DepthStencilState* m_depthReadState{};
        ID3D11RasterizerState* m_cullNoneState{};
        ID3D11RasterizerState* m_cullCounterClockwiseState{};
        ID3D11SamplerState* m_linearWrapSampler{};
    };
}

namespace LamaPon::Detail
{
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
