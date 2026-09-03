#include "LamaPon/Scene/Component.h"

#include "LamaPon/Scene/GameObject.h"

#include <cassert>

namespace LamaPon
{
    GameObject& Component::Owner() const noexcept
    {
        assert(m_owner != nullptr);
        return *m_owner;
    }

    Transform& Component::GetTransform() const noexcept
    {
        return Owner().GetTransform();
    }

    void Component::SetEnabled(const bool enabled)
    {
        if (m_enabled == enabled)
        {
            return;
        }
        m_enabled = enabled;
        RefreshActiveState();
    }

    void Component::InitializeIfNeeded(GraphicsDevice& graphics)
    {
        if (m_initialized)
        {
            return;
        }

        OnInitialize(graphics);
        m_initialized = true;
        RefreshActiveState();
    }

    void Component::RefreshActiveState()
    {
        const bool active =
            m_initialized
            && m_enabled
            && m_owner != nullptr
            && m_owner->IsActiveInHierarchy();
        if (active == m_lastActiveState)
        {
            return;
        }
        m_lastActiveState = active;
        OnActiveStateChanged(active);
    }
}
