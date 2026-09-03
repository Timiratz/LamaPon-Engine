#include "LamaPon/Scene/GameObject.h"

#include "LamaPon/Components/RenderCullingComponent.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace
{
    bool SameTransform(
        const LamaPon::Transform& left,
        const LamaPon::Transform& right) noexcept
    {
        return left.position.x == right.position.x
            && left.position.y == right.position.y
            && left.position.z == right.position.z
            && left.rotationQuaternion.x
                == right.rotationQuaternion.x
            && left.rotationQuaternion.y
                == right.rotationQuaternion.y
            && left.rotationQuaternion.z
                == right.rotationQuaternion.z
            && left.rotationQuaternion.w
                == right.rotationQuaternion.w
            && left.scale.x == right.scale.x
            && left.scale.y == right.scale.y
            && left.scale.z == right.scale.z;
    }
}

namespace LamaPon
{
    GameObject::GameObject(const GameObjectId id, std::string name)
        : m_id(id)
        , m_name(std::move(name))
    {
    }

    DirectX::XMMATRIX GameObject::WorldMatrix() const noexcept
    {
        if (m_scene != nullptr
            && m_scene
                ->RenderingInterpolatedTransforms())
        {
            return InterpolatedWorldMatrix(
                m_scene
                    ->PhysicsInterpolationAlpha());
        }
        const auto local = m_transform.LocalMatrix();
        return m_parent != nullptr
            ? local * m_parent->WorldMatrix()
            : local;
    }

    DirectX::XMMATRIX
        GameObject::InterpolatedLocalMatrix(
            const float alpha) const noexcept
    {
        if (!m_physicsInterpolationActive
            || !m_physicsInterpolationInitialized)
        {
            return m_transform.LocalMatrix();
        }

        using namespace DirectX;
        const float amount =
            std::clamp(alpha, 0.0f, 1.0f);
        const auto lerp = [amount](
            const float from,
            const float to) noexcept
        {
            return from
                + (to - from) * amount;
        };
        const XMFLOAT3 position{
            lerp(
                m_previousPhysicsTransform.position.x,
                m_currentPhysicsTransform.position.x),
            lerp(
                m_previousPhysicsTransform.position.y,
                m_currentPhysicsTransform.position.y),
            lerp(
                m_previousPhysicsTransform.position.z,
                m_currentPhysicsTransform.position.z)
        };
        const XMFLOAT3 scale{
            lerp(
                m_previousPhysicsTransform.scale.x,
                m_currentPhysicsTransform.scale.x),
            lerp(
                m_previousPhysicsTransform.scale.y,
                m_currentPhysicsTransform.scale.y),
            lerp(
                m_previousPhysicsTransform.scale.z,
                m_currentPhysicsTransform.scale.z)
        };
        // 回転はクォータニオンが正本になったので、変換せずに
        // そのままSlerpできます。
        const auto rotation =
            XMQuaternionSlerp(
                m_previousPhysicsTransform
                    .RotationVector(),
                m_currentPhysicsTransform
                    .RotationVector(),
                amount);
        return XMMatrixScaling(
                scale.x,
                scale.y,
                scale.z)
            * XMMatrixRotationQuaternion(rotation)
            * XMMatrixTranslation(
                position.x,
                position.y,
                position.z);
    }

    DirectX::XMMATRIX
        GameObject::InterpolatedWorldMatrix(
            const float alpha) const noexcept
    {
        const auto local =
            InterpolatedLocalMatrix(alpha);
        return m_parent != nullptr
            ? local
                * m_parent
                    ->InterpolatedWorldMatrix(alpha)
            : local;
    }

    void GameObject::SetPhysicsInterpolationActive(
        const bool active) noexcept
    {
        if (m_physicsInterpolationActive == active)
        {
            return;
        }
        m_physicsInterpolationActive = active;
        if (active)
        {
            ResetPhysicsInterpolation();
        }
        else
        {
            m_physicsInterpolationInitialized =
                false;
        }
    }

    void GameObject::BeginPhysicsInterpolationStep()
        noexcept
    {
        if (!m_physicsInterpolationActive)
        {
            return;
        }
        SynchronizePhysicsInterpolation();
        m_previousPhysicsTransform =
            m_currentPhysicsTransform;
    }

    void GameObject::EndPhysicsInterpolationStep()
        noexcept
    {
        if (!m_physicsInterpolationActive)
        {
            return;
        }
        m_currentPhysicsTransform =
            m_transform;
        m_physicsInterpolationInitialized =
            true;
    }

    void GameObject::SynchronizePhysicsInterpolation()
        noexcept
    {
        if (!m_physicsInterpolationActive)
        {
            return;
        }
        if (!m_physicsInterpolationInitialized
            || !SameTransform(
                m_transform,
                m_currentPhysicsTransform))
        {
            ResetPhysicsInterpolation();
        }
    }

    void GameObject::ResetPhysicsInterpolation()
        noexcept
    {
        m_previousPhysicsTransform = m_transform;
        m_currentPhysicsTransform = m_transform;
        m_physicsInterpolationInitialized = true;
    }

    void GameObject::TranslateWorld(
        const DirectX::XMFLOAT3& displacement) noexcept
    {
        using namespace DirectX;

        XMVECTOR localDisplacement = XMLoadFloat3(&displacement);
        if (m_parent != nullptr)
        {
            XMVECTOR determinant{};
            const XMMATRIX inverseParent = XMMatrixInverse(
                &determinant,
                m_parent->WorldMatrix());
            localDisplacement = XMVector3TransformNormal(
                localDisplacement,
                inverseParent);
        }

        XMFLOAT3 local{};
        XMStoreFloat3(&local, localDisplacement);
        m_transform.position.x += local.x;
        m_transform.position.y += local.y;
        m_transform.position.z += local.z;
    }

    void GameObject::RotateWorld(
        const DirectX::XMFLOAT3& radians) noexcept
    {
        using namespace DirectX;
        // この引数はEuler角の差分ではなく、ワールド軸まわりの
        // 回転ベクトル（長さが角度、向きが軸）として扱います。
        const XMVECTOR worldRotationVector =
            XMLoadFloat3(&radians);
        const float angle =
            XMVectorGetX(
                XMVector3Length(worldRotationVector));
        if (!std::isfinite(angle)
            || angle <= 1.0e-6f)
        {
            return;
        }

        const XMVECTOR worldDelta =
            XMQuaternionRotationAxis(
                XMVector3Normalize(worldRotationVector),
                angle);

        XMVECTOR worldScale{};
        XMVECTOR worldRotation{};
        XMVECTOR worldTranslation{};
        if (!XMMatrixDecompose(
                &worldScale,
                &worldRotation,
                &worldTranslation,
                WorldMatrix()))
        {
            return;
        }

        XMMATRIX newWorldRotation =
            XMMatrixRotationQuaternion(worldRotation)
            * XMMatrixRotationQuaternion(worldDelta);

        if (m_parent != nullptr)
        {
            XMVECTOR parentScale{};
            XMVECTOR parentRotation{};
            XMVECTOR parentTranslation{};
            if (!XMMatrixDecompose(
                    &parentScale,
                    &parentRotation,
                    &parentTranslation,
                    m_parent->WorldMatrix()))
            {
                return;
            }
            newWorldRotation =
                newWorldRotation
                * XMMatrixRotationQuaternion(
                    XMQuaternionInverse(parentRotation));
        }

        m_transform.SetRotationVector(
            XMQuaternionRotationMatrix(newWorldRotation));
    }

    bool GameObject::IsAlwaysVisible() const noexcept
    {
        const auto* culling =
            GetComponent<RenderCullingComponent>();
        return culling != nullptr
            && culling->IsEnabled()
            && culling->AlwaysVisible();
    }

    float GameObject::CullingMargin() const noexcept
    {
        const auto* culling =
            GetComponent<RenderCullingComponent>();
        return culling != nullptr && culling->IsEnabled()
            ? culling->CullingMargin()
            : 0.0f;
    }

    bool GameObject::ReorderComponent(
        const Component& moved,
        const Component& reference,
        const bool insertAfter)
    {
        if (&moved == &reference)
        {
            return false;
        }
        const auto movedPosition = std::ranges::find_if(
            m_components,
            [&moved](
                const std::unique_ptr<Component>& candidate)
            {
                return candidate.get() == &moved;
            });
        if (movedPosition == m_components.end())
        {
            return false;
        }
        // いったん抜いてから基準を探します（抜くと添字がずれる
        // ため、先に見つけた位置は使えません）。
        const auto movedIndex = static_cast<std::size_t>(
            movedPosition - m_components.begin());
        auto detached = std::move(*movedPosition);
        m_components.erase(movedPosition);
        const auto referencePosition = std::ranges::find_if(
            m_components,
            [&reference](
                const std::unique_ptr<Component>& candidate)
            {
                return candidate.get() == &reference;
            });
        if (referencePosition == m_components.end())
        {
            // 基準が別のGameObjectのものだった場合。抜いた分は
            // 元の位置へ戻します（並びを変えません）。
            m_components.insert(
                m_components.begin()
                    + static_cast<std::ptrdiff_t>(
                        movedIndex),
                std::move(detached));
            return false;
        }
        m_components.insert(
            insertAfter
                ? std::next(referencePosition)
                : referencePosition,
            std::move(detached));
        return true;
    }

    void GameObject::SetParent(GameObject* parent)
    {
        if (parent == this)
        {
            throw std::invalid_argument("A GameObject cannot be parented to itself.");
        }

        for (auto* ancestor = parent; ancestor != nullptr; ancestor = ancestor->m_parent)
        {
            if (ancestor == this)
            {
                throw std::invalid_argument("GameObject parenting would create a cycle.");
            }
        }

        if (m_parent == parent)
        {
            return;
        }

        if (m_parent != nullptr)
        {
            std::erase(m_parent->m_children, this);
        }

        m_parent = parent;

        if (m_parent != nullptr)
        {
            m_parent->m_children.push_back(this);
        }

        // 祖先チェーンが変わると、このサブツリーの実効アクティブ
        // 状態も変わり得ます。
        PropagateActiveState();
    }

    GameObject* GameObject::FindChild(
        const std::string_view path) const noexcept
    {
        if (path.empty())
        {
            return nullptr;
        }

        // "腕/手/武器"のようなパスをセグメントごとにたどります。
        const std::vector<GameObject*>* children =
            &m_children;
        std::size_t start = 0;
        for (;;)
        {
            const auto separator = path.find('/', start);
            const auto name =
                separator == std::string_view::npos
                    ? path.substr(start)
                    : path.substr(start, separator - start);
            if (name.empty())
            {
                return nullptr;
            }

            GameObject* found = nullptr;
            for (auto* child : *children)
            {
                if (child->Name() == name)
                {
                    found = child;
                    break;
                }
            }
            if (found == nullptr)
            {
                return nullptr;
            }
            if (separator == std::string_view::npos)
            {
                return found;
            }
            children = &found->m_children;
            start = separator + 1;
        }
    }

    void GameObject::SetEnabled(const bool enabled)
    {
        if (m_enabled == enabled)
        {
            return;
        }
        m_enabled = enabled;
        PropagateActiveState();
    }

    bool GameObject::IsActiveInHierarchy() const noexcept
    {
        for (const auto* current = this;
            current != nullptr;
            current = current->m_parent)
        {
            if (!current->m_enabled)
            {
                return false;
            }
        }
        return true;
    }

    void GameObject::PropagateActiveState()
    {
        for (const auto& component : m_components)
        {
            component->RefreshActiveState();
        }
        for (auto* child : m_children)
        {
            child->PropagateActiveState();
        }
    }

    bool GameObject::RemoveComponent(Component& component) noexcept
    {
        const auto originalSize = m_components.size();
        std::erase_if(
            m_components,
            [&component](const std::unique_ptr<Component>& candidate)
            {
                return candidate.get() == &component;
            });
        return m_components.size() != originalSize;
    }

    void GameObject::NotifyCollisionEnter(
        GameObject& other,
        const DirectX::XMFLOAT3& normal,
        const DirectX::XMFLOAT3& point,
        const float penetration,
        const bool isTrigger)
    {
        const CollisionEvent event{
            other,
            normal,
            point,
            penetration,
            isTrigger
        };
        for (const auto& component : m_components)
        {
            if (component->m_enabled)
            {
                if (isTrigger)
                {
                    component->OnTriggerEnter(event);
                }
                else
                {
                    component->OnCollisionEnter(event);
                }
            }
        }
    }

    void GameObject::NotifyCollisionStay(
        GameObject& other,
        const DirectX::XMFLOAT3& normal,
        const DirectX::XMFLOAT3& point,
        const float penetration,
        const bool isTrigger)
    {
        const CollisionEvent event{
            other,
            normal,
            point,
            penetration,
            isTrigger
        };
        for (const auto& component : m_components)
        {
            if (component->m_enabled)
            {
                if (isTrigger)
                {
                    component->OnTriggerStay(event);
                }
                else
                {
                    component->OnCollisionStay(event);
                }
            }
        }
    }

    void GameObject::NotifyCollisionExit(
        GameObject& other,
        const bool isTrigger)
    {
        const CollisionEvent event{
            other,
            DirectX::XMFLOAT3{ 0.0f, 0.0f, 0.0f },
            DirectX::XMFLOAT3{ 0.0f, 0.0f, 0.0f },
            0.0f,
            isTrigger
        };
        for (const auto& component : m_components)
        {
            if (component->m_enabled)
            {
                if (isTrigger)
                {
                    component->OnTriggerExit(event);
                }
                else
                {
                    component->OnCollisionExit(event);
                }
            }
        }
    }

    void GameObject::Update(GraphicsDevice& graphics, const float deltaTime)
    {
        if (!IsActiveInHierarchy())
        {
            return;
        }

        // ユーザーコードを呼ぶ走査は添字で回します。ScriptのStartは
        // ここから呼ばれるので、Startの中でAddComponentするとm_components
        // が再確保され、範囲forのイテレーターが無効になります。
        // （Startで自分にAudioSourceを足すのはドキュメントが勧めている
        //   書き方なので、ここが壊れると素直な使い方で落ちます）
        for (std::size_t index = 0;
            index < m_components.size();
            ++index)
        {
            const auto& component = m_components[index];
            component->InitializeIfNeeded(graphics);

            if (component->m_enabled)
            {
                component->OnUpdate(deltaTime);
            }
        }
    }

    void GameObject::LateUpdate(
        GraphicsDevice& graphics,
        const float deltaTime)
    {
        if (!IsActiveInHierarchy())
        {
            return;
        }

        for (std::size_t index = 0;
            index < m_components.size();
            ++index)
        {
            const auto& component = m_components[index];
            component->InitializeIfNeeded(graphics);
            if (component->m_enabled)
            {
                component->OnLateUpdate(deltaTime);
            }
        }
    }

    void GameObject::FixedUpdate(
        GraphicsDevice& graphics,
        const float fixedDeltaTime)
    {
        if (!IsActiveInHierarchy())
        {
            return;
        }

        for (std::size_t index = 0;
            index < m_components.size();
            ++index)
        {
            const auto& component = m_components[index];
            component->InitializeIfNeeded(graphics);
            if (component->m_enabled)
            {
                component->OnFixedUpdate(
                    fixedDeltaTime);
            }
        }
    }

    void GameObject::Render3D(
        GraphicsDevice& graphics,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        if (!IsActiveInHierarchy())
        {
            return;
        }

        for (const auto& component : m_components)
        {
            component->InitializeIfNeeded(graphics);

            if (component->m_enabled)
            {
                component->OnRender3D(view, projection);
            }
        }
    }

    bool GameObject::HasPreRender3DPass(
        GraphicsDevice& graphics)
    {
        if (!IsActiveInHierarchy())
        {
            return false;
        }

        for (const auto& component : m_components)
        {
            component->InitializeIfNeeded(graphics);
            if (component->m_enabled
                && component->HasPreRender3DPass())
            {
                return true;
            }
        }
        return false;
    }

    bool GameObject::HasAlphaBlended3DPass(
        GraphicsDevice& graphics)
    {
        if (!IsActiveInHierarchy())
        {
            return false;
        }

        for (const auto& component : m_components)
        {
            component->InitializeIfNeeded(graphics);
            if (component->m_enabled
                && component->IsAlphaBlended3D())
            {
                return true;
            }
        }
        return false;
    }

    void GameObject::RenderPre3D(
        GraphicsDevice& graphics,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        if (!IsActiveInHierarchy())
        {
            return;
        }

        for (const auto& component : m_components)
        {
            component->InitializeIfNeeded(graphics);
            if (component->m_enabled
                && component->HasPreRender3DPass())
            {
                component->OnPreRender3D(view, projection);
            }
        }
    }

    void GameObject::Render2D(
        GraphicsDevice& graphics,
        DirectX::SpriteBatch& spriteBatch,
        ID3D11ShaderResourceView* whiteTexture)
    {
        if (!IsActiveInHierarchy())
        {
            return;
        }

        for (const auto& component : m_components)
        {
            component->InitializeIfNeeded(graphics);

            if (component->m_enabled)
            {
                component->OnRender2D(spriteBatch, whiteTexture);
            }
        }
    }

    int GameObject::Render2DSortOrder() const noexcept
    {
        int sortOrder{};
        for (const auto& component : m_components)
        {
            if (component->m_enabled)
            {
                sortOrder = std::max(
                    sortOrder,
                    component->RenderSortOrder());
            }
        }
        return sortOrder;
    }

    void GameObject::RenderDebug3D(
        GraphicsDevice& graphics,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        if (!IsActiveInHierarchy())
        {
            return;
        }

        for (const auto& component : m_components)
        {
            component->InitializeIfNeeded(graphics);

            if (component->m_enabled)
            {
                component->OnRenderDebug3D(graphics, view, projection);
            }
        }
    }
}
