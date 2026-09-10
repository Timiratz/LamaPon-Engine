#include "LamaPon/Graphics/GraphicsDeviceShaderState.h"
#include "LamaPon/Graphics/GraphicsDeviceState.h"

#include "LamaPon/Core/RuntimeServices.h"
#include "LamaPon/Graphics/ClusteredLights.h"
#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/EnvironmentRenderer.h"
#include "LamaPon/Graphics/GraphicsDeviceApiResources.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/ShadowMap.h"

namespace LamaPon
{
    GraphicsDevice::State::State(GraphicsDevice* const owner)
        : m_resourceLeaseState(CreateResourceLeaseState(owner))
        , m_services(std::make_unique<RuntimeServices>())
    {
    }

    GraphicsDevice::State::~State() = default;
}
