#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace LamaPon
{
    class AssetDatabase;

    struct AnimatorFloatParameter final
    {
        std::string name;
        float defaultValue{};
    };

    enum class AnimatorBlendTreeType
    {
        None,
        OneDimensional,
        TwoDimensional
    };

    struct AnimatorBlendChild final
    {
        std::filesystem::path clipPath;
        std::string clipGuid;
        std::string modelClip;
        float threshold{};
        float positionX{};
        float positionY{};
    };

    struct AnimatorEvent final
    {
        std::string name;
        std::string payload;
        float normalizedTime{};
    };

    struct AnimatorState final
    {
        std::string name;
        std::filesystem::path clipPath;
        std::string clipGuid;
        std::string modelClip;
        AnimatorBlendTreeType blendTreeType{
            AnimatorBlendTreeType::None
        };
        std::string blendParameter;
        std::string blendParameterY;
        std::vector<AnimatorBlendChild> blendChildren;
        std::vector<AnimatorEvent> events;
        float speed{ 1.0f };
        bool loop{ true };
    };

    struct AnimatorTransition final
    {
        std::string from;
        std::string to;
        std::string trigger;
        float exitTime{ -1.0f };
        float duration{ 0.2f };
    };

    class AnimatorController final
    {
    public:
        static AnimatorController LoadFromFile(
            const std::filesystem::path& path);
        static AnimatorController FromJson(
            std::string_view json);
        [[nodiscard]] static std::vector<float>
            Calculate2DBlendWeights(
                const std::vector<AnimatorBlendChild>&
                    children,
                float x,
                float y);
        void ResolveAssetReferences(
            const AssetDatabase& database);

        [[nodiscard]] const std::string&
            EntryState() const noexcept
        {
            return m_entryState;
        }
        [[nodiscard]] const std::vector<AnimatorState>&
            States() const noexcept
        {
            return m_states;
        }
        [[nodiscard]] const std::vector<AnimatorTransition>&
            Transitions() const noexcept
        {
            return m_transitions;
        }
        [[nodiscard]] const std::vector<AnimatorFloatParameter>&
            FloatParameters() const noexcept
        {
            return m_floatParameters;
        }
        [[nodiscard]] const AnimatorState* FindState(
            std::string_view name) const noexcept;
        [[nodiscard]] const AnimatorTransition*
            FindTransition(
                std::string_view state,
                float normalizedTime,
                const std::unordered_set<std::string>&
                    activeTriggers) const noexcept;

    private:
        std::string m_entryState;
        std::vector<AnimatorFloatParameter>
            m_floatParameters;
        std::vector<AnimatorState> m_states;
        std::vector<AnimatorTransition>
            m_transitions;
    };
}
