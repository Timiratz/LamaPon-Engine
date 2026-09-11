#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsDeviceState.h"

#include "LamaPon/Graphics/GraphicsDeviceApiResources.h"
#include "LamaPon/Graphics/GraphicsRenderServices.h"

namespace LamaPon
{
    bool GraphicsDevice::DrawPrimitive(
        const PrimitiveDrawRequest& request)
    {
        auto* const renderServices = m_state->m_apiResources != nullptr
            ? m_state->m_apiResources->TryRenderServices()
            : nullptr;
        return renderServices != nullptr
            && renderServices->DrawPrimitive(request);
    }

    bool GraphicsDevice::DrawParticles(
        const ParticleDrawRequest& request,
        const std::filesystem::path& shaderPath,
        const std::array<DirectX::XMFLOAT4, 8>& customParameters,
        std::uint64_t* const shaderGeneration,
        std::string* const shaderError)
    {
        auto* const renderServices = m_state->m_apiResources != nullptr
            ? m_state->m_apiResources->TryRenderServices()
            : nullptr;
        if (renderServices == nullptr)
        {
            return false;
        }

        auto effectiveRequest = request;
        if (!shaderPath.empty())
        {
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
