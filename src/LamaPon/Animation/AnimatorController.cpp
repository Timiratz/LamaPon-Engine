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
        // 文書ファイルの入力ストリーム
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error(
                "Could not open Animator Controller: "
                + PathToUtf8(path));
        }
        // ファイルから読んだJSON全文
        const std::string json{
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{}
        };
        return FromJson(json);
    }

    AnimatorController AnimatorController::FromJson(
        const std::string_view json)
    {
        // 解析したコントローラー文書
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

        // 状態定義のJSON項目
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

        // 復元するコントローラー
        AnimatorController controller;
        controller.m_entryState =
            document.value(
                "entry",
                std::string{});
        // 重複と参照を検査する変数名
        std::unordered_set<std::string>
            parameterNames;
        // 浮動小数点パラメーター定義
        const auto parameters = document.value(
            "parameters",
            nlohmann::json::array());
        if (!parameters.is_array()
            || parameters.size() > 64)
        {
            throw std::runtime_error(
                "Animator Controller supports at most 64 parameters.");
        }
        // 復元するパラメーターのJSON値
        for (const auto& value : parameters)
        {
            // 検証するパラメーター定義
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
        // 重複と参照を検査する状態名
        std::unordered_set<std::string>
            stateNames;
        controller.m_states.reserve(
            states->size());
        // 復元する状態のJSON値
        for (const auto& value : *states)
        {
            // 1DまたはXのブレンド変数名
            std::string blendParameter;
            // Y方向のブレンド変数名
            std::string blendParameterY;
            // 復元するブレンド方式
            AnimatorBlendTreeType blendTreeType{
                AnimatorBlendTreeType::None
            };
            // 復元するブレンド子の一覧
            std::vector<AnimatorBlendChild>
                blendChildren;
            // 復元する状態のイベント列
            std::vector<AnimatorEvent>
                events;
            // ブレンドツリーのJSON項目
            if (const auto blendTree =
                    value.find("blendTree");
                blendTree != value.end()
                    && blendTree->is_object())
            {
                // 文書に保存されたブレンド方式名
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
                // ブレンド子のJSON配列
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
                // 復元するブレンド子のJSON値
                for (const auto& childValue : children)
                {
                    // 検証するブレンド子
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
                    // ブレンド子の2D位置のJSON項目
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
            // 状態のイベントのJSON配列
            const auto eventValues = value.value(
                "events",
                nlohmann::json::array());
            if (!eventValues.is_array()
                || eventValues.size() > 128)
            {
                throw std::runtime_error(
                    "Animator State supports at most 128 events.");
            }
            // 復元するイベントのJSON値
            for (const auto& eventValue :
                eventValues)
            {
                // 検証するイベント定義
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
            // 検証するアニメーション状態
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

        // 復元する遷移のJSON配列
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
        // 復元する遷移のJSON値
        for (const auto& value : transitions)
        {
            // 検証する状態間の遷移
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
        // 子ごとの混合重みの出力列
        std::vector<float> weights(
            children.size(),
            0.0f);
        // 正規化前の混合重みの合計
        float total{};
        // 重みを計算する子の番号
        for (std::size_t index = 0;
            index < children.size();
            ++index)
        {
            // 入力から子のX位置への差
            const float dx =
                x - children[index].positionX;
            // 入力から子のY位置への差
            const float dy =
                y - children[index].positionY;
            // 入力と子の位置の距離の二乗
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
            // 合計で正規化する子の重み
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
        // GUID参照を解決する状態
        for (auto& state : m_states)
        {
            if (!state.clipGuid.empty())
            {
                state.clipPath =
                    database.ResolveGuid(
                        state.clipGuid,
                        state.clipPath);
            }
            // GUID参照を解決する子
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
        // 名前を照合する状態
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
        // 条件を照合する登録順の遷移
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
