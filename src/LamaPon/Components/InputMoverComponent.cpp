#include "LamaPon/Components/InputMoverComponent.h"

#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Input/InputSystem.h"
#include "LamaPon/Scene/GameObject.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace LamaPon
{
    InputMoverComponent::InputMoverComponent(
        std::string horizontalAction,
        std::string verticalAction,
        const float speed)
        : m_horizontalAction(
            std::move(horizontalAction))
        , m_verticalAction(
            std::move(verticalAction))
        , m_speed(std::max(speed, 0.0f))
    {
        ValidateActionName(m_horizontalAction);
        ValidateActionName(m_verticalAction);
    }

    void InputMoverComponent::SetHorizontalAction(
        std::string action)
    {
        ValidateActionName(action);
        m_horizontalAction = std::move(action);
    }

    void InputMoverComponent::SetVerticalAction(
        std::string action)
    {
        ValidateActionName(action);
        m_verticalAction = std::move(action);
    }

    void InputMoverComponent::SetSpeed(
        const float speed) noexcept
    {
        m_speed = std::clamp(speed, 0.0f, 1000.0f);
    }

    void InputMoverComponent::OnInitialize(
        GraphicsDevice& graphics)
    {
        m_input = &graphics.Input();
    }

    void InputMoverComponent::OnUpdate(
        const float deltaTime)
    {
        if (m_input == nullptr)
        {
            return;
        }

        float horizontal =
            m_input->Value(m_horizontalAction);
        float vertical =
            m_input->Value(m_verticalAction);
        const float magnitude = std::sqrt(
            horizontal * horizontal
            + vertical * vertical);
        if (magnitude > 1.0f)
        {
            horizontal /= magnitude;
            vertical /= magnitude;
        }

        Owner().TranslateWorld(
            DirectX::XMFLOAT3{
                horizontal * m_speed * deltaTime,
                0.0f,
                -vertical * m_speed * deltaTime
            });
    }

    void InputMoverComponent::ValidateActionName(
        const std::string_view action)
    {
        if (action.empty()
            || action.size() > 64
            || action.find_first_not_of(" \t\r\n")
                == std::string_view::npos)
        {
            throw std::invalid_argument(
                "Input action name must contain 1 to 64 bytes.");
        }
    }
}
