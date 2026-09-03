#include "LamaPon/Components/LODGroupComponent.h"

#include <algorithm>
#include <utility>

namespace LamaPon
{
    LODGroupComponent::LODGroupComponent(
        std::vector<LODLevel> levels,
        const float cullDistance)
        : m_levels(std::move(levels))
        , m_cullDistance(cullDistance)
    {
        Normalize();
    }

    void LODGroupComponent::SetLevels(
        std::vector<LODLevel> levels)
    {
        m_levels = std::move(levels);
        Normalize();
    }

    void LODGroupComponent::AddLevel(LODLevel level)
    {
        if (m_levels.size() >= 8)
        {
            return;
        }
        m_levels.push_back(level);
        Normalize();
    }

    void LODGroupComponent::RemoveLevel(
        const std::size_t index)
    {
        if (index < m_levels.size())
        {
            m_levels.erase(
                m_levels.begin()
                + static_cast<std::ptrdiff_t>(index));
            Normalize();
        }
    }

    void LODGroupComponent::SetLevel(
        const std::size_t index,
        const LODLevel level)
    {
        if (index < m_levels.size())
        {
            m_levels[index] = level;
            Normalize();
        }
    }

    void LODGroupComponent::SetCullDistance(
        const float value) noexcept
    {
        m_cullDistance =
            std::max(value, 0.0f);
        if (!m_levels.empty())
        {
            m_cullDistance = std::max(
                m_cullDistance,
                m_levels.back().
                    maximumDistance);
        }
    }

    std::uint64_t LODGroupComponent::SelectTarget(
        const float distance) const noexcept
    {
        if (m_levels.empty()
            || distance > m_cullDistance)
        {
            return 0;
        }
        for (const auto& level : m_levels)
        {
            if (distance
                <= level.maximumDistance)
            {
                return level.targetId;
            }
        }
        return m_levels.back().targetId;
    }

    void LODGroupComponent::Normalize()
    {
        if (m_levels.size() > 8)
        {
            m_levels.resize(8);
        }
        for (auto& level : m_levels)
        {
            level.maximumDistance =
                std::max(
                    level.maximumDistance,
                    0.0f);
        }
        std::ranges::sort(
            m_levels,
            {},
            &LODLevel::maximumDistance);
        SetCullDistance(m_cullDistance);
    }
}
