#include "LamaPon/Components/CharacterControllerComponent.h"

#include "LamaPon/Physics/PhysicsSettings.h"

#include "LamaPon/Components/BoxCollider3DComponent.h"
#include "LamaPon/Components/CapsuleCollider3DComponent.h"
#include "LamaPon/Components/ConvexHullCollider3DComponent.h"
#include "LamaPon/Components/MeshCollider3DComponent.h"
#include "LamaPon/Components/SphereCollider3DComponent.h"
#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Input/InputSystem.h"
#include "LamaPon/Physics/PhysicsQuery.h"
#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace LamaPon
{
    CharacterControllerComponent::CharacterControllerComponent(
        const float radius,
        const float height,
        const float moveSpeed,
        const float gravity,
        const float jumpSpeed,
        const float stepOffset,
        const float skinWidth,
        const std::uint32_t layer,
        const std::uint32_t collisionMask)
        : m_radius(std::clamp(radius, 0.05f, 100.0f))
        , m_height(std::clamp(height, m_radius * 2.0f, 1000.0f))
        , m_moveSpeed(std::clamp(moveSpeed, 0.0f, 1000.0f))
        , m_gravity(std::clamp(gravity, 0.0f, 1000.0f))
        , m_jumpSpeed(std::clamp(jumpSpeed, 0.0f, 1000.0f))
        , m_stepOffset(std::clamp(stepOffset, 0.0f, m_height))
        , m_skinWidth(std::clamp(skinWidth, 0.001f, m_radius * 0.5f))
        , m_layer(layer % 32u)
        , m_collisionMask(collisionMask)
    {
    }

    void CharacterControllerComponent::SetRadius(const float value) noexcept
    {
        m_radius = std::clamp(value, 0.05f, 100.0f);
        m_height = std::max(m_height, m_radius * 2.0f);
        m_skinWidth = std::min(m_skinWidth, m_radius * 0.5f);
    }

    void CharacterControllerComponent::SetHeight(const float value) noexcept
    {
        m_height = std::clamp(value, m_radius * 2.0f, 1000.0f);
        m_stepOffset = std::min(m_stepOffset, m_height);
    }

    void CharacterControllerComponent::SetMoveSpeed(const float value) noexcept
    {
        m_moveSpeed = std::clamp(value, 0.0f, 1000.0f);
    }

    void CharacterControllerComponent::SetGravity(const float value) noexcept
    {
        m_gravity = std::clamp(value, 0.0f, 1000.0f);
    }

    void CharacterControllerComponent::SetJumpSpeed(const float value) noexcept
    {
        m_jumpSpeed = std::clamp(value, 0.0f, 1000.0f);
    }

    void CharacterControllerComponent::SetStepOffset(const float value) noexcept
    {
        m_stepOffset = std::clamp(value, 0.0f, m_height);
    }

    void CharacterControllerComponent::SetSkinWidth(const float value) noexcept
    {
        m_skinWidth = std::clamp(value, 0.001f, m_radius * 0.5f);
    }

    void CharacterControllerComponent::SetHorizontalAction(std::string value)
    {
        ValidateActionName(value);
        m_horizontalAction = std::move(value);
    }

    void CharacterControllerComponent::SetVerticalAction(std::string value)
    {
        ValidateActionName(value);
        m_verticalAction = std::move(value);
    }

    void CharacterControllerComponent::SetJumpAction(std::string value)
    {
        ValidateActionName(value);
        m_jumpAction = std::move(value);
    }

    Bounds3D CharacterControllerComponent::WorldBounds() const noexcept
    {
        DirectX::XMFLOAT4X4 world{};
        DirectX::XMStoreFloat4x4(&world, Owner().WorldMatrix());
        return {
            {
                world._41 - m_radius + m_skinWidth,
                world._42 + m_skinWidth,
                world._43 - m_radius + m_skinWidth
            },
            {
                world._41 + m_radius - m_skinWidth,
                world._42 + m_height - m_skinWidth,
                world._43 + m_radius - m_skinWidth
            }
        };
    }

    void CharacterControllerComponent::Move(
        const DirectX::XMFLOAT3& displacement) noexcept
    {
        m_queuedDisplacement.x += displacement.x;
        m_queuedDisplacement.y += displacement.y;
        m_queuedDisplacement.z += displacement.z;
    }

    void CharacterControllerComponent::OnInitialize(GraphicsDevice& graphics)
    {
        m_graphics = &graphics;
        if (m_useInput && graphics.IsInitialized())
        {
            m_input = &graphics.Input();
        }
    }

    bool CharacterControllerComponent::HasCollision(
        const Bounds3D& bounds) const
    {
        const PhysicsQueryFilter filter{
            m_collisionMask,
            false,
            Owner().Id()
        };
        for (const auto& hit :
            Owner().GetScene().OverlapBox(bounds, filter))
        {
            // ヒット種別ごとに有効なコライダーからマスクとレイヤーを
            // 取ります（box以外のヒットではcolliderはnullptrです）。
            std::uint32_t mask{ 0xffffffffu };
            std::uint32_t layer{ 0 };
            if (hit.collider != nullptr)
            {
                mask = hit.collider->CollisionMask();
                layer = hit.collider->Layer();
            }
            else if (hit.capsuleCollider != nullptr)
            {
                mask = hit.capsuleCollider
                    ->CollisionMask();
                layer = hit.capsuleCollider->Layer();
            }
            else if (hit.sphereCollider != nullptr)
            {
                mask = hit.sphereCollider
                    ->CollisionMask();
                layer = hit.sphereCollider->Layer();
            }
            else if (hit.hullCollider != nullptr)
            {
                mask = hit.hullCollider->CollisionMask();
                layer = hit.hullCollider->Layer();
            }
            else if (hit.meshCollider != nullptr)
            {
                mask = hit.meshCollider->CollisionMask();
                layer = hit.meshCollider->Layer();
            }
            if ((mask & (1u << m_layer)) != 0
                // プロジェクト設定の衝突マトリクスにも従います
                // （既定は全部当たるので挙動は変わりません）。
                && LayersCanCollide(m_layer, layer))
            {
                return true;
            }
        }
        return false;
    }

    bool CharacterControllerComponent::HasGround(
        const float probeDistance) const
    {
        auto bounds = WorldBounds();
        bounds.maximum.y = bounds.minimum.y + m_skinWidth;
        bounds.minimum.y -= std::max(probeDistance, m_skinWidth);
        return HasCollision(bounds);
    }

    bool CharacterControllerComponent::MoveAxis(
        const int axis,
        const float amount,
        const bool allowStep)
    {
        if (std::abs(amount) <= 0.000001f)
        {
            return true;
        }
        DirectX::XMFLOAT3 movement{};
        (&movement.x)[axis] = amount;
        Owner().TranslateWorld(movement);
        if (!HasCollision(WorldBounds()))
        {
            return true;
        }
        Owner().TranslateWorld({
            -movement.x,
            -movement.y,
            -movement.z
        });

        if (allowStep && m_grounded && m_stepOffset > 0.0f)
        {
            Owner().TranslateWorld({ 0.0f, m_stepOffset, 0.0f });
            if (!HasCollision(WorldBounds()))
            {
                Owner().TranslateWorld(movement);
                if (!HasCollision(WorldBounds()))
                {
                    return true;
                }
                Owner().TranslateWorld({
                    -movement.x,
                    -movement.y,
                    -movement.z
                });
            }
            Owner().TranslateWorld({ 0.0f, -m_stepOffset, 0.0f });
        }
        return false;
    }

    void CharacterControllerComponent::OnUpdate(const float deltaTime)
    {
        const float safeDeltaTime = std::clamp(deltaTime, 0.0f, 0.1f);
        DirectX::XMFLOAT3 displacement = m_queuedDisplacement;
        m_queuedDisplacement = {};

        if (m_useInput
            && m_input == nullptr
            && m_graphics != nullptr
            && m_graphics->IsInitialized())
        {
            m_input = &m_graphics->Input();
        }
        if (m_useInput && m_input != nullptr)
        {
            float horizontal = m_input->Value(m_horizontalAction);
            float vertical = m_input->Value(m_verticalAction);
            const float magnitude =
                std::sqrt(horizontal * horizontal + vertical * vertical);
            if (magnitude > 1.0f)
            {
                horizontal /= magnitude;
                vertical /= magnitude;
            }
            displacement.x += horizontal * m_moveSpeed * safeDeltaTime;
            displacement.z -= vertical * m_moveSpeed * safeDeltaTime;
            m_jumpRequested =
                m_jumpRequested || m_input->WasPressed(m_jumpAction);
        }

        m_grounded = HasGround(m_skinWidth + 0.04f);
        if (m_jumpRequested && m_grounded)
        {
            m_verticalVelocity = m_jumpSpeed;
            m_grounded = false;
        }
        m_jumpRequested = false;
        m_verticalVelocity -= m_gravity * safeDeltaTime;
        displacement.y += m_verticalVelocity * safeDeltaTime;

        const float maximumStep =
            std::max(0.05f, m_radius * 0.35f);
        const float largest = std::max({
            std::abs(displacement.x),
            std::abs(displacement.y),
            std::abs(displacement.z)
        });
        const int steps = std::max(
            1,
            static_cast<int>(std::ceil(largest / maximumStep)));
        const DirectX::XMFLOAT3 step{
            displacement.x / static_cast<float>(steps),
            displacement.y / static_cast<float>(steps),
            displacement.z / static_cast<float>(steps)
        };

        bool verticalBlocked{};
        for (int index{}; index < steps; ++index)
        {
            MoveAxis(0, step.x, true);
            MoveAxis(2, step.z, true);
            if (!MoveAxis(1, step.y, false))
            {
                verticalBlocked = true;
            }
        }
        if (verticalBlocked)
        {
            if (displacement.y <= 0.0f)
            {
                m_grounded = true;
            }
            m_verticalVelocity = 0.0f;
        }
        else
        {
            m_grounded = HasGround(m_skinWidth + 0.04f);
            if (m_grounded && m_verticalVelocity < 0.0f)
            {
                m_verticalVelocity = 0.0f;
            }
        }
    }

    void CharacterControllerComponent::OnRenderDebug3D(
        GraphicsDevice& graphics,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        const auto color = m_grounded
            ? DirectX::XMVectorSet(0.15f, 1.0f, 0.45f, 1.0f)
            : DirectX::XMVectorSet(0.15f, 0.75f, 1.0f, 1.0f);
        graphics.Debug().DrawBounds(WorldBounds(), color, view, projection);
    }

    void CharacterControllerComponent::ValidateActionName(
        const std::string_view value)
    {
        if (value.empty()
            || value.size() > 64
            || value.find_first_not_of(" \t\r\n") == std::string_view::npos)
        {
            throw std::invalid_argument(
                "Input action name must contain 1 to 64 bytes.");
        }
    }
}
