#include "LamaPon/Animation/AnimatorController.h"

#include "LamaPon/Assets/AssetDatabase.h"
#include "LamaPon/Core/PathUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <unordered_set>

namespace LamaPon
{
    AnimatorController AnimatorController::LoadFromFile(
        const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error(
                "Could not open Animator Controller: "
                + PathToUtf8(path));
        }
        const std::string json{
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{}
        };
        return FromJson(json);
    }

    AnimatorController AnimatorController::FromJson(
        const std::string_view json)
    {
        const auto document =
            nlohmann::json::parse(
                json.begin(),
                json.end());
        if (document.value(
                "format",
                std::string{})
                != "LamaPonAnimatorController"
            || document.value("version", 0) != 1)
        {
            throw std::runtime_error(
                "Unsupported LamaPon Animator Controller format.");
        }

        const auto states =
            document.find("states");
        if (states == document.end()
            || !states->is_array()
            || states->empty()
            || states->size() > 256)
        {
            throw std::runtime_error(
                "Animator Controller requires between 1 and 256 states.");
        }

        AnimatorController controller;
        controller.m_entryState =
            document.value(
                "entry",
                std::string{});
        std::unordered_set<std::string>
            parameterNames;
        const auto parameters = document.value(
            "parameters",
            nlohmann::json::array());
        if (!parameters.is_array()
            || parameters.size() > 64)
        {
            throw std::runtime_error(
                "Animator Controller supports at most 64 parameters.");
        }
        for (const auto& value : parameters)
        {
            AnimatorFloatParameter parameter{
                value.value("name", std::string{}),
                value.value("default", 0.0f)
            };
            if (parameter.name.empty()
                || parameter.name.size() > 96
                || !std::isfinite(parameter.defaultValue)
                || !parameterNames.insert(
                    parameter.name).second)
            {
                throw std::runtime_error(
                    "Animator float parameter is invalid or duplicated.");
            }
            controller.m_floatParameters.push_back(
                std::move(parameter));
        }
        std::unordered_set<std::string>
            stateNames;
        controller.m_states.reserve(
            states->size());
        for (const auto& value : *states)
        {
            std::string blendParameter;
            std::string blendParameterY;
            AnimatorBlendTreeType blendTreeType{
                AnimatorBlendTreeType::None
            };
            std::vector<AnimatorBlendChild>
                blendChildren;
            std::vector<AnimatorEvent>
                events;
            if (const auto blendTree =
                    value.find("blendTree");
                blendTree != value.end()
                    && blendTree->is_object())
            {
                const auto type = blendTree->value(
                    "type",
                    std::string{ "1D" });
                if (type != "1D" && type != "2D")
                {
                    throw std::runtime_error(
                        "Animator Blend Tree type must be 1D or 2D.");
                }
                blendTreeType =
                    type == "2D"
                    ? AnimatorBlendTreeType::
                        TwoDimensional
                    : AnimatorBlendTreeType::
                        OneDimensional;
                blendParameter = blendTree->value(
                    blendTreeType
                        == AnimatorBlendTreeType::
                            TwoDimensional
                    ? "parameterX"
                    : "parameter",
                    std::string{});
                if (blendTreeType
                    == AnimatorBlendTreeType::
                        TwoDimensional)
                {
                    blendParameterY =
                        blendTree->value(
                            "parameterY",
                            std::string{});
                }
                const auto children = blendTree->value(
                    "children",
                    nlohmann::json::array());
                if (!children.is_array()
                    || children.size() < 2
                    || children.size() > 16)
                {
                    throw std::runtime_error(
                        "Blend Tree requires between 2 and 16 children.");
                }
                for (const auto& childValue : children)
                {
                    AnimatorBlendChild child{
                        PathFromUtf8(
                            childValue.value(
                                "clip",
                                std::string{})),
                        childValue.value(
                            "clipGuid",
                            std::string{}),
                        childValue.value(
                            "modelClip",
                            std::string{}),
                        childValue.value(
                            "threshold",
                            0.0f),
                        0.0f,
                        0.0f
                    };
                    if (const auto position =
                            childValue.find(
                                "position");
                        position
                            != childValue.end()
                            && position->is_array()
                            && position->size() == 2)
                    {
                        child.positionX =
                            position->at(0)
                                .get<float>();
                        child.positionY =
                            position->at(1)
                                .get<float>();
                    }
                    if ((child.clipPath.empty()
                            && child.modelClip.empty())
                        || child.modelClip.size() > 256
                        || !std::isfinite(child.threshold)
                        || !std::isfinite(
                            child.positionX)
                        || !std::isfinite(
                            child.positionY))
                    {
                        throw std::runtime_error(
                            "Animator Blend Tree child is invalid.");
                    }
                    blendChildren.push_back(
                        std::move(child));
                }
                if (blendTreeType
                    == AnimatorBlendTreeType::
                        OneDimensional)
                {
                    std::ranges::sort(
                        blendChildren,
                        {},
                        &AnimatorBlendChild::threshold);
                }
            }
            const auto eventValues = value.value(
                "events",
                nlohmann::json::array());
            if (!eventValues.is_array()
                || eventValues.size() > 128)
            {
                throw std::runtime_error(
                    "Animator State supports at most 128 events.");
            }
            for (const auto& eventValue :
                eventValues)
            {
                AnimatorEvent event{
                    eventValue.value(
                        "name",
                        std::string{}),
                    eventValue.value(
                        "payload",
                        std::string{}),
                    eventValue.value(
                        "time",
                        0.0f)
                };
                if (event.name.empty()
                    || event.name.size() > 96
                    || event.payload.size() > 512
                    || !std::isfinite(
                        event.normalizedTime)
                    || event.normalizedTime < 0.0f
                    || event.normalizedTime > 1.0f)
                {
                    throw std::runtime_error(
                        "Animator Event is invalid.");
                }
                events.push_back(std::move(event));
            }
            std::ranges::sort(
                events,
                {},
                &AnimatorEvent::normalizedTime);
            AnimatorState state{
                value.value("name", std::string{}),
                PathFromUtf8(
                    value.value(
                        "clip",
                        std::string{})),
                value.value(
                    "clipGuid",
                    std::string{}),
                value.value(
                    "modelClip",
                    std::string{}),
                blendTreeType,
                std::move(blendParameter),
                std::move(blendParameterY),
                std::move(blendChildren),
                std::move(events),
                value.value("speed", 1.0f),
                value.value("loop", true)
            };
            if (state.name.empty()
                || (state.clipPath.empty()
                    && state.modelClip.empty()
                    && state.blendChildren.empty())
                || state.modelClip.size() > 256
                || (!state.blendChildren.empty()
                    && (state.blendParameter.empty()
                        || !parameterNames.contains(
                            state.blendParameter)
                        || (state.blendTreeType
                                == AnimatorBlendTreeType::
                                    TwoDimensional
                            && (state.blendParameterY.empty()
                                || !parameterNames.contains(
                                    state.blendParameterY)))))
                || !std::isfinite(state.speed)
                || state.speed <= 0.0f
                || state.speed > 100.0f
                || !stateNames.insert(
                    state.name).second)
            {
                throw std::runtime_error(
                    "Animator state has an invalid name, clip/modelClip, speed, or duplicate name.");
            }
            controller.m_states.push_back(
                std::move(state));
        }
        if (!stateNames.contains(
                controller.m_entryState))
        {
            throw std::runtime_error(
                "Animator entry state does not exist.");
        }

        const auto transitions =
            document.value(
                "transitions",
                nlohmann::json::array());
        if (!transitions.is_array()
            || transitions.size() > 1024)
        {
            throw std::runtime_error(
                "Animator Controller supports at most 1024 transitions.");
        }
        controller.m_transitions.reserve(
            transitions.size());
        for (const auto& value : transitions)
        {
            AnimatorTransition transition{
                value.value("from", std::string{}),
                value.value("to", std::string{}),
                value.value("trigger", std::string{}),
                value.value("exitTime", -1.0f),
                value.value("duration", 0.2f)
            };
            if (!stateNames.contains(transition.from)
                || !stateNames.contains(transition.to)
                || (!transition.trigger.empty()
                    && transition.trigger.size() > 96)
                || !std::isfinite(transition.exitTime)
                || transition.exitTime > 1.0f
                || (transition.exitTime < 0.0f
                    && transition.trigger.empty())
                || !std::isfinite(transition.duration)
                || transition.duration < 0.0f
                || transition.duration > 10.0f)
            {
                throw std::runtime_error(
                    "Animator transition has invalid states, conditions, or duration.");
            }
            controller.m_transitions.push_back(
                std::move(transition));
        }
        return controller;
    }

    std::vector<float>
        AnimatorController::Calculate2DBlendWeights(
            const std::vector<AnimatorBlendChild>&
                children,
            const float x,
            const float y)
    {
        std::vector<float> weights(
            children.size(),
            0.0f);
        float total{};
        for (std::size_t index = 0;
            index < children.size();
            ++index)
        {
            const float dx =
                x - children[index].positionX;
            const float dy =
                y - children[index].positionY;
            const float distanceSquared =
                dx * dx + dy * dy;
            if (distanceSquared <= 0.000001f)
            {
                std::ranges::fill(
                    weights,
                    0.0f);
                weights[index] = 1.0f;
                return weights;
            }
            weights[index] =
                1.0f / distanceSquared;
            total += weights[index];
        }
        if (total > 0.0f)
        {
            for (auto& weight : weights)
            {
                weight /= total;
            }
        }
        return weights;
    }

    void AnimatorController::ResolveAssetReferences(
        const AssetDatabase& database)
    {
        for (auto& state : m_states)
        {
            if (!state.clipGuid.empty())
            {
                state.clipPath =
                    database.ResolveGuid(
                        state.clipGuid,
                        state.clipPath);
            }
            for (auto& child : state.blendChildren)
            {
                if (!child.clipGuid.empty())
                {
                    child.clipPath =
                        database.ResolveGuid(
                            child.clipGuid,
                            child.clipPath);
                }
            }
        }
    }

    const AnimatorState* AnimatorController::FindState(
        const std::string_view name) const noexcept
    {
        for (const auto& state : m_states)
        {
            if (state.name == name)
            {
                return &state;
            }
        }
        return nullptr;
    }

    const AnimatorTransition*
        AnimatorController::FindTransition(
            const std::string_view state,
            const float normalizedTime,
            const std::unordered_set<std::string>&
                activeTriggers) const noexcept
    {
        for (const auto& transition :
            m_transitions)
        {
            if (transition.from != state)
            {
                continue;
            }
            if (!transition.trigger.empty()
                && !activeTriggers.contains(
                    transition.trigger))
            {
                continue;
            }
            if (transition.exitTime >= 0.0f
                && normalizedTime
                    < transition.exitTime)
            {
                continue;
            }
            return &transition;
        }
        return nullptr;
    }
}
