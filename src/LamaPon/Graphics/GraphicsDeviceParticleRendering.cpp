#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsDeviceState.h"

#include "LamaPon/Graphics/GraphicsDeviceApiResources.h"
#include "LamaPon/Graphics/GraphicsRenderServices.h"

namespace LamaPon
{
    bool GraphicsDevice::DrawParticles(
        const ParticleDrawRequest& request,
        const std::filesystem::path& shaderPath,
        const std::array<DirectX::XMFLOAT4, 8>& customParameters,
        std::uint64_t* const shaderGeneration,
        std::string* const shaderError)
    {
        if (m_state->m_apiResources == nullptr
            || m_state->m_apiResources->renderServices == nullptr)
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
        return m_state->m_apiResources->renderServices->DrawParticles(
            effectiveRequest);
    }
}
