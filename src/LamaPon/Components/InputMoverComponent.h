#pragma once

#include "LamaPon/Scene/Component.h"

#include <string>

namespace LamaPon
{
    class InputSystem;

    class InputMoverComponent final : public Component
    {
    public:
        explicit InputMoverComponent(
            std::string horizontalAction = "MoveHorizontal",
            std::string verticalAction = "MoveVertical",
            float speed = 3.0f);

        void SetHorizontalAction(std::string action);
        [[nodiscard]] const std::string&
            HorizontalAction() const noexcept
        {
            return m_horizontalAction;
        }
        void SetVerticalAction(std::string action);
        [[nodiscard]] const std::string&
            VerticalAction() const noexcept
        {
            return m_verticalAction;
        }
        void SetSpeed(float speed) noexcept;
        [[nodiscard]] float Speed() const noexcept
        {
            return m_speed;
        }

        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "InputMover";
        }

    protected:
        void OnInitialize(GraphicsDevice& graphics) override;
        void OnUpdate(float deltaTime) override;

    private:
        static void ValidateActionName(
            std::string_view action);

        std::string m_horizontalAction;
        std::string m_verticalAction;
        float m_speed{ 3.0f };
        InputSystem* m_input{};
    };
}
