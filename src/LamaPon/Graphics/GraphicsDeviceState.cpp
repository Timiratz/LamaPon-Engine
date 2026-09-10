#include "LamaPon/Graphics/GraphicsDeviceState.h"

#include "LamaPon/Core/RuntimeServices.h"
#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/GraphicsDeviceApiResources.h"
#include "LamaPon/Graphics/RenderTarget.h"

namespace LamaPon
{
    GraphicsDevice::State::State(GraphicsDevice* const owner)
        : m_resourceLeaseState(CreateResourceLeaseState(owner))
        , m_services(std::make_unique<RuntimeServices>())
    {
    }

    GraphicsDevice::State::~State() = default;
}
