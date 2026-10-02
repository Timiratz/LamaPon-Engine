#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsDeviceState.h"

#include "LamaPon/Graphics/GraphicsDeviceApiResources.h"
#include "LamaPon/Graphics/GraphicsRenderServices.h"

namespace LamaPon
{
    // 描画サービスへ基本図形を渡し、サービス未提供または描画失敗ならfalseを返す(request: 基本図形の描画指定)。
    bool GraphicsDevice::DrawPrimitive(
        const PrimitiveDrawRequest& request)
    {
        // 借用したAPI別の描画サービス
        auto* const renderServices = m_state->m_apiResources != nullptr
            ? m_state->m_apiResources->TryRenderServices()
            : nullptr;
        return renderServices != nullptr
            && renderServices->DrawPrimitive(request);
    }

    // 粒子をAPI別に描き、指定があればカスタムシェーダーを適用する(request: 完全な四頂点単位の粒子指定, shaderPath: HLSLパスで空は既定描画, customParameters: 8個の四成分定数, shaderGeneration: 世代の出力先かnullptr, shaderError: 診断の出力先かnullptr)。
    bool GraphicsDevice::DrawParticles(
        const ParticleDrawRequest& request,
        const std::filesystem::path& shaderPath,
        const std::array<DirectX::XMFLOAT4, 8>& customParameters,
        std::uint64_t* const shaderGeneration,
        std::string* const shaderError)
    {
        // 借用したAPI別の描画サービス
        auto* const renderServices = m_state->m_apiResources != nullptr
            ? m_state->m_apiResources->TryRenderServices()
            : nullptr;
        if (renderServices == nullptr)
        {
            return false;
        }

        // シェーダー適用を補う描画指定
        auto effectiveRequest = request;
        if (!shaderPath.empty()
            && ActiveRenderingApi() == RenderingApi::DirectX12Experimental)
        {
            // D3D12のカスタム描画と代替描画が失敗した場合は既定パイプラインで描く。
            return DrawD3D12CustomParticles(
                    request,
                    shaderPath,
                    customParameters,
                    shaderGeneration,
                    shaderError)
                || renderServices->DrawParticles(request);
        }
        if (!shaderPath.empty())
        {
            // 描画時に指定のカスタムシェーダーを適用する。
            // 参照を借用したコールバックはDrawParticlesの復帰前に実行し、保存しない。
            effectiveRequest.applyCustomPixelShader =
                [this,
                    &shaderPath,
                    &customParameters,
                    shaderGeneration,
                    shaderError]()
            {
                return ApplyCustomPixelShader(
                    shaderPath,
                    customParameters,
                    shaderGeneration,
                    shaderError);
            };
        }
        return renderServices->DrawParticles(effectiveRequest);
    }
}
