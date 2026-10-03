#include "LamaPon/Scene/GameObject.h"

#include "LamaPon/Components/RenderCullingComponent.h"
#include "LamaPon/Graphics/FrameDebugger.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace
{
    // 変換の各成分が完全一致するか返します(left: 比較元, right: 比較先)。
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

namespace
{
    // 現在の深度パスに対応するデバッグ描画区分を返します(graphics: 描画機器)。
    [[nodiscard]] LamaPon::FrameDebugPass CurrentFrameDebugPass(
        const LamaPon::GraphicsDevice& graphics) noexcept
    {
        switch (graphics.DepthPass())
        {
        case LamaPon::DepthPassKind::Shadow:
            return LamaPon::FrameDebugPass::ShadowDepth;
        case LamaPon::DepthPassKind::Prepass:
            return LamaPon::FrameDebugPass::DepthPrepass;
        case LamaPon::DepthPassKind::None:
            break;
        }
        return LamaPon::FrameDebugPass::Color;
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
        // 親基準のローカル行列
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
        // 制限した物理補間率
        const float amount =
            std::clamp(alpha, 0.0f, 1.0f);
        // 成分を同じ補間率で線形補間します(from: 始点の成分, to: 終点の成分)。
        const auto lerp = [amount](
            const float from,
            const float to) noexcept
        {
            return from
                + (to - from) * amount;
        };
        // 補間後のローカル位置
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
        // 補間後のローカル拡縮
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
        // 補間後のクォータニオン
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
        // 親基準のローカル行列
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

        // 親基準へ変換する移動量
        XMVECTOR localDisplacement = XMLoadFloat3(&displacement);
        if (m_parent != nullptr)
        {
            // 親行列の行列式
            XMVECTOR determinant{};
            // 親のワールド行列の逆行列
            const XMMATRIX inverseParent = XMMatrixInverse(
                &determinant,
                m_parent->WorldMatrix());
            localDisplacement = XMVector3TransformNormal(
                localDisplacement,
                inverseParent);
        }

        // 親基準の移動成分
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
        // ワールド軸の回転ベクトル
        const XMVECTOR worldRotationVector =
            XMLoadFloat3(&radians);
        // 加える回転角のラジアン値
        const float angle =
            XMVectorGetX(
                XMVector3Length(worldRotationVector));
        if (!std::isfinite(angle)
            || angle <= 1.0e-6f)
        {
            return;
        }

        // 追加するワールド回転
        const XMVECTOR worldDelta =
            XMQuaternionRotationAxis(
                XMVector3Normalize(worldRotationVector),
                angle);

        // 分解したワールド拡縮
        XMVECTOR worldScale{};
        // 分解したワールド回転
        XMVECTOR worldRotation{};
        // 分解したワールド位置
        XMVECTOR worldTranslation{};
        if (!XMMatrixDecompose(
                &worldScale,
                &worldRotation,
                &worldTranslation,
                WorldMatrix()))
        {
            return;
        }

        // 親基準へ変換する回転行列
        XMMATRIX newWorldRotation =
            XMMatrixRotationQuaternion(worldRotation)
            * XMMatrixRotationQuaternion(worldDelta);

        if (m_parent != nullptr)
        {
            // 分解した親のワールド拡縮
            XMVECTOR parentScale{};
            // 分解した親のワールド回転
            XMVECTOR parentRotation{};
            // 分解した親のワールド位置
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
        // 有効性を調べるカリング設定
        const auto* culling =
            GetComponent<RenderCullingComponent>();
        return culling != nullptr
            && culling->IsEnabled()
            && culling->AlwaysVisible();
    }

    float GameObject::CullingMargin() const noexcept
    {
        // 有効性を調べるカリング設定
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
        // 移動対象の格納位置
        // 移動対象に一致するか調べます(candidate: 所有するコンポーネント)。
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
        // 取り外す前の格納添字
        const auto movedIndex = static_cast<std::size_t>(
            movedPosition - m_components.begin());
        // 取り外したコンポーネント
        auto detached = std::move(*movedPosition);
        m_components.erase(movedPosition);
        // 基準コンポーネントの格納位置
        // 基準に一致するか調べます(candidate: 所有するコンポーネント)。
        const auto referencePosition = std::ranges::find_if(
            m_components,
            [&reference](
                const std::unique_ptr<Component>& candidate)
            {
                return candidate.get() == &reference;
            });
        if (referencePosition == m_components.end())
        {
            // 基準が所属外なら取り外した対象を元の位置へ戻します。
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

        // 循環を調べる祖先
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

        // 祖先チェーンが変わると、このサブツリーの実効アクティブ状態も変わり得ます。
        PropagateActiveState();
    }

    GameObject* GameObject::FindChild(
        const std::string_view path) const noexcept
    {
        if (path.empty())
        {
            return nullptr;
        }

        // 現在の区間で探す子の一覧
        const std::vector<GameObject*>* children =
            &m_children;
        // 検索区間の開始位置
        std::size_t start = 0;
        for (;;)
        {
            // 次の区間を分ける位置
            const auto separator = path.find('/', start);
            // 今回探す子の名前
            const auto name =
                separator == std::string_view::npos
                    ? path.substr(start)
                    : path.substr(start, separator - start);
            if (name.empty())
            {
                return nullptr;
            }

            // 名前が一致した子
            GameObject* found = nullptr;
            // 走査する子オブジェクト
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
        // 有効状態を調べる自身か祖先
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
        // 処理するコンポーネント
        for (const auto& component : m_components)
        {
            component->RefreshActiveState();
        }
        // 走査する子オブジェクト
        for (auto* child : m_children)
        {
            child->PropagateActiveState();
        }
    }

    bool GameObject::RemoveComponent(Component& component) noexcept
    {
        // 削除前のコンポーネント数
        const auto originalSize = m_components.size();
        // 削除対象に一致するか調べます(candidate: 所有するコンポーネント)。
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
        // コンポーネントへ通知する接触
        const CollisionEvent event{
            other,
            normal,
            point,
            penetration,
            isTrigger
        };
        // 処理するコンポーネント
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
        // コンポーネントへ通知する接触
        const CollisionEvent event{
            other,
            normal,
            point,
            penetration,
            isTrigger
        };
        // 処理するコンポーネント
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
        // コンポーネントへ通知する接触
        const CollisionEvent event{
            other,
            DirectX::XMFLOAT3{ 0.0f, 0.0f, 0.0f },
            DirectX::XMFLOAT3{ 0.0f, 0.0f, 0.0f },
            0.0f,
            isTrigger
        };
        // 処理するコンポーネント
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

        // 初期化中の追加も走査対象へ含めるため添字で走査します。
        // 更新するコンポーネント添字
        for (std::size_t index = 0;
            index < m_components.size();
            ++index)
        {
            // 処理するコンポーネント
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

        // 更新するコンポーネント添字
        for (std::size_t index = 0;
            index < m_components.size();
            ++index)
        {
            // 処理するコンポーネント
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

        // 更新するコンポーネント添字
        for (std::size_t index = 0;
            index < m_components.size();
            ++index)
        {
            // 処理するコンポーネント
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

        // 処理するコンポーネント
        for (const auto& component : m_components)
        {
            component->InitializeIfNeeded(graphics);

            if (component->m_enabled
                && SubmitFrameDebugEvent(
                    graphics,
                    *component,
                    FrameDebugEventKind::Draw3D))
            {
                component->OnRender3D(view, projection);
            }
        }
    }

    bool GameObject::SubmitFrameDebugEvent(
        GraphicsDevice& graphics,
        const Component& component,
        const FrameDebugEventKind kind)
    {
        // 描画イベントの収集先
        auto& frameDebugger = graphics.FrameDebug();
        if (!frameDebugger.IsEnabled())
        {
            return true;
        }
        // 登録する描画の説明
        FrameDebugDrawDescription description;
        try
        {
            if (!component.DescribeDrawEvent(description))
            {
                return true;
            }
        }
        catch (...)
        {
            // 説明の作成に失敗しても描画は通常どおり行い、項目だけ空のイベントとして数えます。
            description = {};
        }
        return frameDebugger.SubmitDrawEvent(
            kind,
            CurrentFrameDebugPass(graphics),
            Id(),
            Name(),
            component.TypeName(),
            std::move(description));
    }

    bool GameObject::HasPreRender3DPass(
        GraphicsDevice& graphics)
    {
        if (!IsActiveInHierarchy())
        {
            return false;
        }

        // 処理するコンポーネント
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

        // 処理するコンポーネント
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

        // 処理するコンポーネント
        for (const auto& component : m_components)
        {
            component->InitializeIfNeeded(graphics);
            if (component->m_enabled
                && component->HasPreRender3DPass()
                && SubmitFrameDebugEvent(
                    graphics,
                    *component,
                    FrameDebugEventKind::PreRender3D))
            {
                component->OnPreRender3D(view, projection);
            }
        }
    }

    void GameObject::Render2D(
        GraphicsDevice& graphics,
        const SpriteDrawContext& sprites)
    {
        if (!IsActiveInHierarchy())
        {
            return;
        }

        // 処理するコンポーネント
        for (const auto& component : m_components)
        {
            component->InitializeIfNeeded(graphics);

            if (component->m_enabled
                && SubmitFrameDebugEvent(
                    graphics,
                    *component,
                    FrameDebugEventKind::Draw2D))
            {
                component->OnRender2D(sprites);
            }
        }
    }

    int GameObject::Render2DSortOrder() const noexcept
    {
        // 有効な描画順序と0の最大値
        int sortOrder{};
        // 処理するコンポーネント
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

        // 処理するコンポーネント
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
