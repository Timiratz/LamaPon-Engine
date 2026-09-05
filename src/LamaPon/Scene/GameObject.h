#pragma once

#include "LamaPon/Scene/Component.h"
#include "LamaPon/Scene/Transform.h"

#include <memory>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace LamaPon
{
    class GraphicsDevice;
    class Scene;
    using GameObjectId = std::uint64_t;
    // 追加読み込み（Additive）したシーンを識別する番号です。
    // 0は最初に読み込んだ主シーンを指します。
    using SceneHandle = std::uint32_t;

    class GameObject final
    {
    public:
        GameObject(GameObjectId id, std::string name);

        GameObject(const GameObject&) = delete;
        GameObject& operator=(const GameObject&) = delete;

        [[nodiscard]] GameObjectId Id() const noexcept { return m_id; }
        [[nodiscard]] const std::string& Name() const noexcept { return m_name; }
        void SetName(std::string name) { m_name = std::move(name); }
        [[nodiscard]] const std::string& Tag() const noexcept { return m_tag; }
        void SetTag(std::string tag) { m_tag = std::move(tag); }
        [[nodiscard]] bool CompareTag(
            const std::string_view tag) const noexcept
        {
            return m_tag == tag;
        }

        [[nodiscard]] Transform& GetTransform() noexcept { return m_transform; }
        [[nodiscard]] const Transform& GetTransform() const noexcept { return m_transform; }
        [[nodiscard]] DirectX::XMMATRIX WorldMatrix() const noexcept;
        [[nodiscard]] DirectX::XMMATRIX
            InterpolatedWorldMatrix(
                float alpha) const noexcept;
        void TranslateWorld(const DirectX::XMFLOAT3& displacement) noexcept;
        void RotateWorld(
            const DirectX::XMFLOAT3& radians) noexcept;

        void SetParent(GameObject* parent);
        [[nodiscard]] GameObject* Parent() const noexcept { return m_parent; }
        [[nodiscard]] const std::vector<GameObject*>& Children() const noexcept { return m_children; }
        [[nodiscard]] Scene& GetScene() const noexcept
        {
            return *m_scene;
        }

        [[nodiscard]] bool IsEnabled() const noexcept { return m_enabled; }
        void SetEnabled(bool enabled);
        // true の GameObject は、カメラ外判定と遮蔽判定で非表示にしません。
        // 描画カリングの個別設定はRenderCullingComponentが持ちます。
        // ここは描画側が毎フレーム呼ぶ読み口で、
        // コンポーネントが無ければ既定（通常どおりカリング）です。
        [[nodiscard]] bool IsAlwaysVisible() const noexcept;
        [[nodiscard]] float CullingMargin() const noexcept;
        // 自身と祖先のGameObjectがすべて有効な時にtrueになります。
        [[nodiscard]] bool IsActiveInHierarchy() const noexcept;
        [[nodiscard]] bool IsPersistent() const noexcept
        {
            return m_persistent;
        }
        [[nodiscard]] const std::string&
            PersistenceKey() const noexcept
        {
            return m_persistenceKey;
        }
        // このGameObjectがどのシーン由来かを返します。0は主シーン、
        // 1以上はLoadAdditiveで足したシーンです。追加シーンを
        // 破棄すると、同じ番号のGameObjectだけがまとめて消えます。
        [[nodiscard]] SceneHandle
            SourceScene() const noexcept
        {
            return m_sourceScene;
        }
        [[nodiscard]] bool IsPrefabInstanceRoot() const noexcept
        {
            return !m_prefabAssetPath.empty();
        }
        [[nodiscard]] const std::filesystem::path&
            PrefabAssetPath() const noexcept
        {
            return m_prefabAssetPath;
        }
        void SetPrefabAssetPath(std::filesystem::path path)
        {
            m_prefabAssetPath = std::move(path);
        }

        template<typename T, typename... Args>
        T& AddComponent(Args&&... args)
        {
            static_assert(std::is_base_of_v<Component, T>, "T must derive from LamaPon::Component.");

            auto component = std::make_unique<T>(std::forward<Args>(args)...);
            component->m_owner = this;

            auto* result = component.get();
            m_components.emplace_back(std::move(component));
            return *result;
        }

        template<typename T>
        [[nodiscard]] T* GetComponent() noexcept
        {
            static_assert(std::is_base_of_v<Component, T>, "T must derive from LamaPon::Component.");

            for (const auto& component : m_components)
            {
                if (auto* match = dynamic_cast<T*>(component.get()))
                {
                    return match;
                }
            }

            return nullptr;
        }

        // C++スクリプトを型で引きます。GetComponentと違い、Tは
        // Componentでなくてよく、自作のインターフェース（抽象基底）
        // でも構いません。スクリプト実体はNativeScriptComponentが
        // void*で保持しているため、Componentの仮想フック経由で
        // Script*を取り出してからdynamic_castします。
        //
        //   struct IDamageable { virtual void ApplyDamage(int) = 0; ... };
        //   class Player : public LamaPon::Script, public IDamageable { ... };
        //   if (auto* target = other.GetScript<IDamageable>()) { ... }
        //
        // Tの定義はGame Module内にしか無くて構いません。この
        // テンプレートは呼び出し側（Game Module）で実体化され、
        // dynamic_castも同じモジュール内で完結します。
        template<typename T>
        [[nodiscard]] T* GetScript() const noexcept
        {
            for (const auto& component : m_components)
            {
                if (component == nullptr)
                {
                    continue;
                }
                if (auto* script = component->ScriptInstance())
                {
                    if (auto* match = dynamic_cast<T*>(script))
                    {
                        return match;
                    }
                }
            }
            return nullptr;
        }

        // 子孫まで探します（自分自身も含みます）。
        template<typename T>
        [[nodiscard]] T* GetScriptInChildren(
            const bool includeInactive = false) const noexcept
        {
            if (!includeInactive && !IsActiveInHierarchy())
            {
                return nullptr;
            }
            if (auto* match = GetScript<T>())
            {
                return match;
            }
            for (const auto* child : m_children)
            {
                if (child == nullptr)
                {
                    continue;
                }
                if (auto* match =
                    child->GetScriptInChildren<T>(
                        includeInactive))
                {
                    return match;
                }
            }
            return nullptr;
        }

        // 自分自身と子孫からコンポーネントを深さ優先で探します
        // 自分自身も含みます。includeInactiveが
        // falseの間は非アクティブ階層をスキップします。
        template<typename T>
        [[nodiscard]] T* GetComponentInChildren(
            const bool includeInactive = false) noexcept
        {
            static_assert(std::is_base_of_v<Component, T>, "T must derive from LamaPon::Component.");

            if (includeInactive || IsActiveInHierarchy())
            {
                if (auto* match = GetComponent<T>())
                {
                    return match;
                }
                for (auto* child : m_children)
                {
                    if (auto* match =
                        child->GetComponentInChildren<T>(
                            includeInactive))
                    {
                        return match;
                    }
                }
            }
            return nullptr;
        }

        template<typename T>
        [[nodiscard]] std::vector<T*>
            GetComponentsInChildren(
                const bool includeInactive = false)
        {
            static_assert(std::is_base_of_v<Component, T>, "T must derive from LamaPon::Component.");

            std::vector<T*> results;
            CollectComponentsInChildren<T>(
                includeInactive,
                results);
            return results;
        }

        // 自分自身から親方向へ遡ってコンポーネントを探します。
        template<typename T>
        [[nodiscard]] T* GetComponentInParent(
            const bool includeInactive = false) noexcept
        {
            static_assert(std::is_base_of_v<Component, T>, "T must derive from LamaPon::Component.");

            for (auto* current = this;
                current != nullptr;
                current = current->m_parent)
            {
                if (!includeInactive
                    && !current->IsActiveInHierarchy())
                {
                    continue;
                }
                if (auto* match =
                    current->GetComponent<T>())
                {
                    return match;
                }
            }
            return nullptr;
        }

        template<typename T>
        [[nodiscard]] std::vector<T*>
            GetComponentsInParent(
                const bool includeInactive = false)
        {
            static_assert(std::is_base_of_v<Component, T>, "T must derive from LamaPon::Component.");

            std::vector<T*> results;
            for (auto* current = this;
                current != nullptr;
                current = current->m_parent)
            {
                if (!includeInactive
                    && !current->IsActiveInHierarchy())
                {
                    continue;
                }
                if (auto* match =
                    current->GetComponent<T>())
                {
                    results.push_back(match);
                }
            }
            return results;
        }

        // 直下の子を名前で取得します。"腕/手/武器"のように
        // スラッシュ区切りのパスで孫以降もたどれます
        // 非アクティブな子も返します。
        [[nodiscard]] GameObject* FindChild(
            std::string_view path) const noexcept;

        [[nodiscard]] const std::vector<std::unique_ptr<Component>>& Components() const noexcept
        {
            return m_components;
        }
        // コンポーネントの並びを変えます。movedをreferenceの直前
        // （insertAfter=falseのとき）または直後へ移します。
        // 並びはインスペクターの表示順であり、Update・保存・
        // 2D描画（同じsortOrder同士）の順でもあります。
        // どちらかがこのGameObjectのものでなければfalseを返します。
        bool ReorderComponent(
            const Component& moved,
            const Component& reference,
            bool insertAfter);
        template<typename T>
        [[nodiscard]] const T* GetComponent() const noexcept
        {
            static_assert(std::is_base_of_v<Component, T>, "T must derive from LamaPon::Component.");

            for (const auto& component : m_components)
            {
                if (const auto* match = dynamic_cast<const T*>(component.get()))
                {
                    return match;
                }
            }

            return nullptr;
        }

        void Update(GraphicsDevice& graphics, float deltaTime);
        void LateUpdate(
            GraphicsDevice& graphics,
            float deltaTime);
        void FixedUpdate(
            GraphicsDevice& graphics,
            float fixedDeltaTime);
        void Render3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);
        [[nodiscard]] bool HasPreRender3DPass(
            GraphicsDevice& graphics);
        // アルファ合成の3D描画を持つか。不透明の後、カメラから
        // 遠い順に描くための振り分けに使います。
        [[nodiscard]] bool HasAlphaBlended3DPass(
            GraphicsDevice& graphics);
        void RenderPre3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);
        void RenderDebug3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);
        void Render2D(
            GraphicsDevice& graphics,
            DirectX::SpriteBatch& spriteBatch,
            ID3D11ShaderResourceView* whiteTexture);
        // このオブジェクトで有効なコンポーネントのRenderSortOrder()の
        // 最大値です。2D/UIスプライトパスでは、この値が大きいものほど
        // 後から手前に描画します。
        [[nodiscard]] int Render2DSortOrder() const noexcept;

    private:
        friend class Scene;

        // GetComponentsInChildrenの再帰本体（深さ優先で収集）。
        template<typename T>
        void CollectComponentsInChildren(
            const bool includeInactive,
            std::vector<T*>& results)
        {
            if (!includeInactive
                && !IsActiveInHierarchy())
            {
                return;
            }
            for (const auto& component : m_components)
            {
                if (auto* match =
                    dynamic_cast<T*>(component.get()))
                {
                    results.push_back(match);
                }
            }
            for (auto* child : m_children)
            {
                child->CollectComponentsInChildren<T>(
                    includeInactive,
                    results);
            }
        }

        bool RemoveComponent(Component& component) noexcept;
        void PropagateActiveState();
        void NotifyCollisionEnter(
            GameObject& other,
            const DirectX::XMFLOAT3& normal,
            const DirectX::XMFLOAT3& point,
            float penetration,
            bool isTrigger);
        void NotifyCollisionStay(
            GameObject& other,
            const DirectX::XMFLOAT3& normal,
            const DirectX::XMFLOAT3& point,
            float penetration,
            bool isTrigger);
        void NotifyCollisionExit(GameObject& other, bool isTrigger);
        void SetPhysicsInterpolationActive(
            bool active) noexcept;
        void BeginPhysicsInterpolationStep() noexcept;
        void EndPhysicsInterpolationStep() noexcept;
        void SynchronizePhysicsInterpolation() noexcept;
        void ResetPhysicsInterpolation() noexcept;
        [[nodiscard]] DirectX::XMMATRIX
            InterpolatedLocalMatrix(
                float alpha) const noexcept;

        GameObjectId m_id;
        std::string m_name;
        std::string m_tag;
        Transform m_transform;
        Transform m_previousPhysicsTransform;
        Transform m_currentPhysicsTransform;
        std::vector<std::unique_ptr<Component>> m_components;
        GameObject* m_parent{};
        std::vector<GameObject*> m_children;
        bool m_enabled{ true };
        bool m_physicsInterpolationActive{};
        bool m_physicsInterpolationInitialized{};
        bool m_persistent{};
        std::string m_persistenceKey;
        SceneHandle m_sourceScene{};
        std::filesystem::path m_prefabAssetPath;
        Scene* m_scene{};
    };
}
