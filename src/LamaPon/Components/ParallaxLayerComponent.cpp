#include "LamaPon/Components/ParallaxLayerComponent.h"

#include "LamaPon/Components/CameraComponent.h"
#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/Scene.h"

namespace LamaPon
{
    ParallaxLayerComponent::ParallaxLayerComponent(
        const DirectX::XMFLOAT2 factor,
        const std::uint64_t referenceId) noexcept
        : m_factor(factor)
        , m_referenceId(referenceId)
    {
    }

    void ParallaxLayerComponent::OnUpdate(float)
    {
        GameObject* reference{};
        if (m_referenceId != 0)
        {
            reference = Owner().GetScene().FindGameObject(
                m_referenceId);
        }
        else if (auto* camera =
                Owner().GetScene().MainCamera())
        {
            reference = &camera->Owner();
        }
        if (reference == nullptr || reference == &Owner())
        {
            return;
        }

        const auto& referencePosition =
            reference->GetTransform().position;
        auto& ownTransform = Owner().GetTransform();
        if (!m_initialized)
        {
            m_referenceOrigin = {
                referencePosition.x,
                referencePosition.y };
            m_ownOrigin = {
                ownTransform.position.x,
                ownTransform.position.y };
            m_initialized = true;
            return;
        }

        const float deltaX =
            referencePosition.x - m_referenceOrigin.x;
        const float deltaY =
            referencePosition.y - m_referenceOrigin.y;
        ownTransform.position.x =
            m_ownOrigin.x + deltaX * m_factor.x;
        ownTransform.position.y =
            m_ownOrigin.y + deltaY * m_factor.y;
    }
}
