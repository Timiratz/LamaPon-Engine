#pragma once

#include "LamaPon/Scene/Component.h"

#include <cstdint>
#include <cstddef>
#include <vector>

namespace LamaPon
{
    struct LODLevel final
    {
        float maximumDistance{ 25.0f };
        std::uint64_t targetId{};
    };

    class LODGroupComponent final : public Component
    {
    public:
        explicit LODGroupComponent(
            std::vector<LODLevel> levels = {},
            float cullDistance = 200.0f);

        [[nodiscard]] const std::vector<LODLevel>&
            Levels() const noexcept
        {
            return m_levels;
        }
        void SetLevels(std::vector<LODLevel> levels);
        void AddLevel(LODLevel level);
        void RemoveLevel(std::size_t index);
        void SetLevel(std::size_t index, LODLevel level);

        [[nodiscard]] float CullDistance() const noexcept
        {
            return m_cullDistance;
        }
        void SetCullDistance(float value) noexcept;

        [[nodiscard]] std::uint64_t
            SelectTarget(float distance) const noexcept;

        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "LODGroup";
        }

    private:
        void Normalize();

        std::vector<LODLevel> m_levels;
        float m_cullDistance{ 200.0f };
    };
}
