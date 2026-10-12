#include "LamaPon/Components/Rig2DComponent.h"

#include "LamaPon/Components/Keyform2DComponent.h"
#include "LamaPon/Scene/GameObject.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace LamaPon
{
    namespace
    {
        // パラメータ名の最大バイト数
        constexpr std::size_t MaximumParameterNameLength = 64;
        // 自動の揺れの時刻を巻き戻す周期秒数
        constexpr float AutoTimeWrapSeconds = 3600.0f;
        // 値の絶対値の上限
        constexpr float MaximumMagnitude = 1.0e6f;

        // 有限値ならそのまま、非有限なら代替値を返します(value: 確認する値, fallback: 代替値)。
        [[nodiscard]] float FiniteOr(
            const float value,
            const float fallback) noexcept
        {
            return std::isfinite(value)
                ? std::clamp(value, -MaximumMagnitude, MaximumMagnitude)
                : fallback;
        }

        // 物体と子孫のKeyform2Dへ処理を行います(object: 起点, action: Keyform2Dへの処理)。
        template<typename Action>
        void ForEachKeyform(GameObject& object, const Action& action)
        {
            // 物体のKeyform2D
            if (auto* keyform = object.GetComponent<Keyform2DComponent>())
            {
                action(*keyform);
            }
            // 処理を伝える子
            for (auto* child : object.Children())
            {
                ForEachKeyform(*child, action);
            }
        }
    }

    Rig2DComponent::Rig2DComponent(
        std::vector<Rig2DParameter> parameters)
    {
        SetParameters(std::move(parameters));
    }

    void Rig2DComponent::SetParameters(
        std::vector<Rig2DParameter> parameters)
    {
        m_parameters.clear();
        // 登録するパラメータ
        for (auto& parameter : parameters)
        {
            if (FindParameter(parameter.name) == nullptr)
            {
                static_cast<void>(AddParameter(std::move(parameter)));
            }
        }
    }

    Rig2DParameter Rig2DComponent::Sanitize(
        Rig2DParameter parameter) noexcept
    {
        parameter.minimum = FiniteOr(parameter.minimum, 0.0f);
        parameter.maximum = FiniteOr(parameter.maximum, 1.0f);
        if (parameter.maximum < parameter.minimum)
        {
            std::swap(parameter.minimum, parameter.maximum);
        }
        parameter.defaultValue = std::clamp(
            FiniteOr(parameter.defaultValue, parameter.minimum),
            parameter.minimum,
            parameter.maximum);
        parameter.value = std::clamp(
            FiniteOr(parameter.value, parameter.defaultValue),
            parameter.minimum,
            parameter.maximum);
        parameter.autoAmplitude = std::max(
            FiniteOr(parameter.autoAmplitude, 0.0f),
            0.0f);
        parameter.autoFrequency = std::clamp(
            FiniteOr(parameter.autoFrequency, 0.25f),
            0.0f,
            60.0f);
        return parameter;
    }

    bool Rig2DComponent::AddParameter(Rig2DParameter parameter)
    {
        if (parameter.name.empty()
            || parameter.name.size() > MaximumParameterNameLength)
        {
            return false;
        }
        parameter = Sanitize(std::move(parameter));
        // 同名を探す登録済みのパラメータ
        for (auto& existing : m_parameters)
        {
            if (existing.name == parameter.name)
            {
                existing = std::move(parameter);
                return true;
            }
        }
        m_parameters.push_back(std::move(parameter));
        return true;
    }

    bool Rig2DComponent::RemoveParameter(const std::string_view name)
    {
        // 除去前の数
        const auto count = m_parameters.size();
        std::erase_if(
            m_parameters,
            // 指定名のパラメータか判定します(parameter: 判定するパラメータ)。
            [name](const Rig2DParameter& parameter)
            {
                return parameter.name == name;
            });
        return m_parameters.size() != count;
    }

    const Rig2DParameter* Rig2DComponent::FindParameter(
        const std::string_view name) const noexcept
    {
        // 名前を比べるパラメータ
        for (const auto& parameter : m_parameters)
        {
            if (parameter.name == name)
            {
                return &parameter;
            }
        }
        return nullptr;
    }

    bool Rig2DComponent::SetParameter(
        const std::string_view name,
        const float value) noexcept
    {
        // 名前を比べるパラメータ
        for (auto& parameter : m_parameters)
        {
            if (parameter.name == name)
            {
                parameter.value = std::clamp(
                    FiniteOr(value, parameter.value),
                    parameter.minimum,
                    parameter.maximum);
                return true;
            }
        }
        return false;
    }

    float Rig2DComponent::ParameterValue(
        const std::string_view name) const noexcept
    {
        // 指定名のパラメータ
        const auto* parameter = FindParameter(name);
        if (parameter == nullptr)
        {
            return 0.0f;
        }
        // 自動の揺れの量
        const float wave = parameter->autoAmplitude > 0.0f
            ? parameter->autoAmplitude
                * std::sin(
                    2.0f
                    * std::numbers::pi_v<float>
                    * parameter->autoFrequency
                    * m_time)
            : 0.0f;
        return std::clamp(
            parameter->value + wave,
            parameter->minimum,
            parameter->maximum);
    }

    void Rig2DComponent::ResetParameters() noexcept
    {
        // 既定値へ戻すパラメータ
        for (auto& parameter : m_parameters)
        {
            parameter.value = parameter.defaultValue;
        }
    }

    void Rig2DComponent::ApplyToHierarchy()
    {
        ForEachKeyform(
            Owner(),
            // 現在の値の姿勢を適用します(keyform: 適用する部品)。
            [](Keyform2DComponent& keyform)
            {
                keyform.ApplyPose();
            });
    }

    void Rig2DComponent::RestoreHierarchyRestPose()
    {
        ForEachKeyform(
            Owner(),
            // 基準姿勢へ戻します(keyform: 戻す部品)。
            [](Keyform2DComponent& keyform)
            {
                keyform.RestoreRestPose();
            });
    }

    void Rig2DComponent::OnUpdate(const float deltaTime)
    {
        if (std::isfinite(deltaTime) && deltaTime > 0.0f)
        {
            m_time = std::fmod(m_time + deltaTime, AutoTimeWrapSeconds);
        }
    }
}
