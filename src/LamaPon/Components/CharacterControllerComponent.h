#pragma once

#include "LamaPon/Physics/CollisionTypes.h"
#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <cstdint>
#include <string>

namespace LamaPon
{
    class InputSystem;
    class GraphicsDevice;

    class CharacterControllerComponent final : public Component
    {
    public:
        CharacterControllerComponent(
            float radius = 0.4f,
            float height = 1.8f,
            float moveSpeed = 4.0f,
            float gravity = 20.0f,
            float jumpSpeed = 7.0f,
            float stepOffset = 0.3f,
            float skinWidth = 0.03f,
            std::uint32_t layer = 2,
            std::uint32_t collisionMask = 0xffffffffu);

        [[nodiscard]] float Radius() const noexcept { return m_radius; }
        void SetRadius(float value) noexcept;
        [[nodiscard]] float Height() const noexcept { return m_height; }
        void SetHeight(float value) noexcept;
        [[nodiscard]] float MoveSpeed() const noexcept { return m_moveSpeed; }
        void SetMoveSpeed(float value) noexcept;
        [[nodiscard]] float Gravity() const noexcept { return m_gravity; }
        void SetGravity(float value) noexcept;
        [[nodiscard]] float JumpSpeed() const noexcept { return m_jumpSpeed; }
        void SetJumpSpeed(float value) noexcept;
        [[nodiscard]] float StepOffset() const noexcept { return m_stepOffset; }
        void SetStepOffset(float value) noexcept;
        [[nodiscard]] float SkinWidth() const noexcept { return m_skinWidth; }
        void SetSkinWidth(float value) noexcept;
        [[nodiscard]] std::uint32_t Layer() const noexcept { return m_layer; }
        void SetLayer(std::uint32_t value) noexcept { m_layer = value % 32u; }
        [[nodiscard]] std::uint32_t CollisionMask() const noexcept
        {
            return m_collisionMask;
        }
        void SetCollisionMask(std::uint32_t value) noexcept
        {
            m_collisionMask = value;
        }

        [[nodiscard]] bool UseInput() const noexcept { return m_useInput; }
        void SetUseInput(bool value) noexcept { m_useInput = value; }
        [[nodiscard]] const std::string& HorizontalAction() const noexcept
        {
            return m_horizontalAction;
        }
        void SetHorizontalAction(std::string value);
        [[nodiscard]] const std::string& VerticalAction() const noexcept
        {
            return m_verticalAction;
        }
        void SetVerticalAction(std::string value);
        [[nodiscard]] const std::string& JumpAction() const noexcept
        {
            return m_jumpAction;
        }
        void SetJumpAction(std::string value);

        [[nodiscard]] bool IsGrounded() const noexcept { return m_grounded; }
        [[nodiscard]] float VerticalVelocity() const noexcept
        {
            return m_verticalVelocity;
        }
        [[nodiscard]] Bounds3D WorldBounds() const noexcept;

        void Move(const DirectX::XMFLOAT3& displacement) noexcept;
        void Jump() noexcept { m_jumpRequested = true; }

        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "CharacterController";
        }

    protected:
        void OnInitialize(GraphicsDevice& graphics) override;
        void OnUpdate(float deltaTime) override;
        void OnRenderDebug3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection) override;

    private:
        static void ValidateActionName(std::string_view value);
        [[nodiscard]] bool HasCollision(const Bounds3D& bounds) const;
        [[nodiscard]] bool HasGround(float probeDistance) const;
        bool MoveAxis(int axis, float amount, bool allowStep);

        float m_radius{ 0.4f };
        float m_height{ 1.8f };
        float m_moveSpeed{ 4.0f };
        float m_gravity{ 20.0f };
        float m_jumpSpeed{ 7.0f };
        float m_stepOffset{ 0.3f };
        float m_skinWidth{ 0.03f };
        std::uint32_t m_layer{ 2 };
        std::uint32_t m_collisionMask{ 0xffffffffu };
        bool m_useInput{ true };
        std::string m_horizontalAction{ "MoveHorizontal" };
        std::string m_verticalAction{ "MoveVertical" };
        std::string m_jumpAction{ "Jump" };
        InputSystem* m_input{};
        GraphicsDevice* m_graphics{};
        DirectX::XMFLOAT3 m_queuedDisplacement{};
        float m_verticalVelocity{};
        bool m_grounded{};
        bool m_jumpRequested{};
    };
}
