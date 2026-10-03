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
    class SpriteDrawContext;
    enum class FrameDebugEventKind : std::uint8_t;
    using GameObjectId = std::uint64_t;
    // シーン番号は0が主シーン、1以上が追加読み込みしたシーンです。
    using SceneHandle = std::uint32_t;

    class GameObject final
    {
    public:
        // 識別番号と名前を設定して生成します(id: オブジェクト番号, name: 表示名)。
        GameObject(GameObjectId id, std::string name);

        // コンポーネントの所有権を維持するためコピーを禁止します。
        GameObject(const GameObject&) = delete;
        // コンポーネントの所有権を維持するためコピー代入を禁止します。
        GameObject& operator=(const GameObject&) = delete;

        // オブジェクトの識別番号を返します。
        [[nodiscard]] GameObjectId Id() const noexcept { return m_id; }
        // 表示名への参照を返します。
        [[nodiscard]] const std::string& Name() const noexcept { return m_name; }
        // 表示名を置き換えます(name: 新しい表示名)。
        void SetName(std::string name) { m_name = std::move(name); }
        // 分類タグへの参照を返します。
        [[nodiscard]] const std::string& Tag() const noexcept { return m_tag; }
        // 分類タグを置き換えます(tag: 新しいタグ)。
        void SetTag(std::string tag) { m_tag = std::move(tag); }
        // 分類タグが完全一致するか返します(tag: 比較対象のタグ)。
        [[nodiscard]] bool CompareTag(
            const std::string_view tag) const noexcept
        {
            return m_tag == tag;
        }

        // 親を基準とするローカル変換への参照を返します。
        [[nodiscard]] Transform& GetTransform() noexcept { return m_transform; }
        // 親を基準とするローカル変換への読み取り参照を返します。
        [[nodiscard]] const Transform& GetTransform() const noexcept { return m_transform; }
        // 祖先の変換を合成し、補間描画中は補間したワールド行列を返します。
        [[nodiscard]] DirectX::XMMATRIX WorldMatrix() const noexcept;
        // 物理状態を補間し祖先と合成した行列を返します(alpha: 0〜1に制限する補間率)。
        [[nodiscard]] DirectX::XMMATRIX
            InterpolatedWorldMatrix(
                float alpha) const noexcept;
        // ワールド座標の移動をローカル位置へ加えます(displacement: ワールド移動量)。
        // 親が存在する場合は、そのワールド行列が可逆である必要があります。
        void TranslateWorld(const DirectX::XMFLOAT3& displacement) noexcept;
        // ワールド軸を基準に回転を加えます(radians: ワールド回転軸×ラジアン角度)。
        // 非有限・微小角度や行列分解失敗時は回転を変更しません。
        void RotateWorld(
            const DirectX::XMFLOAT3& radians) noexcept;

        // ローカル変換を維持して親を変更します(parent: 新しい親、nullptrで解除)。
        // 自己参照と循環する親指定はinvalid_argumentを送出します。
        void SetParent(GameObject* parent);
        // 親への非所有ポインターを返し、親がなければnullptrを返します。
        [[nodiscard]] GameObject* Parent() const noexcept { return m_parent; }
        // 直下の子の非所有ポインター一覧を返します。
        [[nodiscard]] const std::vector<GameObject*>& Children() const noexcept { return m_children; }
        // 所属シーンへの参照を返し、シーンへの登録済みである必要があります。
        [[nodiscard]] Scene& GetScene() const noexcept
        {
            return *m_scene;
        }

        // 自身に設定された有効状態を返します。
        [[nodiscard]] bool IsEnabled() const noexcept { return m_enabled; }
        // 自身の有効状態を変更し子孫へ通知します(enabled: 新しい有効状態)。
        void SetEnabled(bool enabled);
        // 有効なカリング設定が常時表示を指定するか返し、設定がなければfalseを返します。
        [[nodiscard]] bool IsAlwaysVisible() const noexcept;
        // 有効なカリング設定の境界余白を返し、設定がなければ0を返します。
        [[nodiscard]] float CullingMargin() const noexcept;
        // 自身とすべての祖先が有効な場合にtrueを返します。
        [[nodiscard]] bool IsActiveInHierarchy() const noexcept;
        // シーン切り替え後も保持する指定か返します。
        [[nodiscard]] bool IsPersistent() const noexcept
        {
            return m_persistent;
        }
        // シーンをまたぐ保持に使う識別キーへの参照を返します。
        [[nodiscard]] const std::string&
            PersistenceKey() const noexcept
        {
            return m_persistenceKey;
        }
        // このGameObjectがどのシーン由来かを返します。
        // 0は主シーン、1以上はLoadAdditiveで足したシーンです。
        // 追加シーンを破棄すると、同じ番号のGameObjectだけがまとめて消えます。
        // 生成元のシーン番号を返し、0は主シーンを表します。
        [[nodiscard]] SceneHandle
            SourceScene() const noexcept
        {
            return m_sourceScene;
        }
        // プレハブのアセットパスが設定されたルートか返します。
        [[nodiscard]] bool IsPrefabInstanceRoot() const noexcept
        {
            return !m_prefabAssetPath.empty();
        }
        // 生成元プレハブのアセットパスへの参照を返します。
        [[nodiscard]] const std::filesystem::path&
            PrefabAssetPath() const noexcept
        {
            return m_prefabAssetPath;
        }
        // 生成元プレハブのアセットパスを設定します(path: プレハブの保存先)。
        void SetPrefabAssetPath(std::filesystem::path path)
        {
            m_prefabAssetPath = std::move(path);
        }

        // Tを所有するコンポーネントとして追加します(args: Tのコンストラクター引数)。
        // 初期化は後続の更新・描画時に行い、削除まで参照を利用できます。
        template<typename T, typename... Args>
        T& AddComponent(Args&&... args)
        {
            static_assert(std::is_base_of_v<Component, T>, "T must derive from LamaPon::Component.");

            // 所有するコンポーネント
            auto component = std::make_unique<T>(std::forward<Args>(args)...);
            component->m_owner = this;

            // 追加したコンポーネント
            auto* result = component.get();
            m_components.emplace_back(std::move(component));
            return *result;
        }

        // 自身の最初のTコンポーネントを返し、未登録ならnullptrを返します。
        template<typename T>
        [[nodiscard]] T* GetComponent() noexcept
        {
            static_assert(std::is_base_of_v<Component, T>, "T must derive from LamaPon::Component.");

            // 所有するコンポーネント
            for (const auto& component : m_components)
            {
                // 型が一致する検索結果
                if (auto* match = dynamic_cast<T*>(component.get()))
                {
                    return match;
                }
            }

            return nullptr;
        }

        // 自身の最初のTスクリプトを返し、未登録ならnullptrを返します。
        // Tはスクリプトの基底インターフェースでもよく、検索は呼び出し側モジュールで実体化します。
        template<typename T>
        [[nodiscard]] T* GetScript() const noexcept
        {
            // 所有するコンポーネント
            for (const auto& component : m_components)
            {
                if (component == nullptr)
                {
                    continue;
                }
                // 実行中のスクリプト
                if (auto* script = component->ScriptInstance())
                {
                    // 型が一致する検索結果
                    if (auto* match = dynamic_cast<T*>(script))
                    {
                        return match;
                    }
                }
            }
            return nullptr;
        }

        // 自身から子孫へ深さ優先で最初のTスクリプトを探します(includeInactive: 無効な階層も検索)。
        template<typename T>
        [[nodiscard]] T* GetScriptInChildren(
            const bool includeInactive = false) const noexcept
        {
            if (!includeInactive && !IsActiveInHierarchy())
            {
                return nullptr;
            }
            // 型が一致する検索結果
            if (auto* match = GetScript<T>())
            {
                return match;
            }
            // 検索する子オブジェクト
            for (const auto* child : m_children)
            {
                if (child == nullptr)
                {
                    continue;
                }
                // 型が一致する検索結果
                if (auto* match =
                    child->GetScriptInChildren<T>(
                        includeInactive))
                {
                    return match;
                }
            }
            return nullptr;
        }

        // 自身から子孫へ深さ優先で最初のTを探します(includeInactive: 無効な階層も検索)。
        template<typename T>
        [[nodiscard]] T* GetComponentInChildren(
            const bool includeInactive = false) noexcept
        {
            static_assert(std::is_base_of_v<Component, T>, "T must derive from LamaPon::Component.");

            if (includeInactive || IsActiveInHierarchy())
            {
                // 型が一致する検索結果
                if (auto* match = GetComponent<T>())
                {
                    return match;
                }
                // 検索する子オブジェクト
                for (auto* child : m_children)
                {
                    // 型が一致する検索結果
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

        // 自身と子孫のTを深さ優先で収集します(includeInactive: 無効な階層も検索)。
        template<typename T>
        [[nodiscard]] std::vector<T*>
            GetComponentsInChildren(
                const bool includeInactive = false)
        {
            static_assert(std::is_base_of_v<Component, T>, "T must derive from LamaPon::Component.");

            // 収集したコンポーネント
            std::vector<T*> results;
            CollectComponentsInChildren<T>(
                includeInactive,
                results);
            return results;
        }

        // 自身から祖先へ最初のTを探します(includeInactive: 無効なオブジェクトも検索)。
        template<typename T>
        [[nodiscard]] T* GetComponentInParent(
            const bool includeInactive = false) noexcept
        {
            static_assert(std::is_base_of_v<Component, T>, "T must derive from LamaPon::Component.");

            // 検索中の自身または祖先
            for (auto* current = this;
                current != nullptr;
                current = current->m_parent)
            {
                if (!includeInactive
                    && !current->IsActiveInHierarchy())
                {
                    continue;
                }
                // 型が一致する検索結果
                if (auto* match =
                    current->GetComponent<T>())
                {
                    return match;
                }
            }
            return nullptr;
        }

        // 自身から祖先へ各オブジェクトの最初のTを収集します(includeInactive: 無効なオブジェクトも検索)。
        template<typename T>
        [[nodiscard]] std::vector<T*>
            GetComponentsInParent(
                const bool includeInactive = false)
        {
            static_assert(std::is_base_of_v<Component, T>, "T must derive from LamaPon::Component.");

            // 収集したコンポーネント
            std::vector<T*> results;
            // 検索中の自身または祖先
            for (auto* current = this;
                current != nullptr;
                current = current->m_parent)
            {
                if (!includeInactive
                    && !current->IsActiveInHierarchy())
                {
                    continue;
                }
                // 型が一致する検索結果
                if (auto* match =
                    current->GetComponent<T>())
                {
                    results.push_back(match);
                }
            }
            return results;
        }

        // 名前をたどり子孫を返し、未発見や空の区間ならnullptrを返します(path: スラッシュ区切りの名前)。
        // 無効な子も検索し、同名の兄弟は最初の一致を採用します。
        [[nodiscard]] GameObject* FindChild(
            std::string_view path) const noexcept;

        // 自身が所有するコンポーネント一覧への参照を返します。
        [[nodiscard]] const std::vector<std::unique_ptr<Component>>& Components() const noexcept
        {
            return m_components;
        }
        // 基準の前後へコンポーネントを移します(moved: 移動対象, reference: 基準, insertAfter: trueで直後)。
        // 同一対象や所属外の指定はfalseを返し、並びは表示・更新・保存・同順序の2D描画に反映します。
        bool ReorderComponent(
            const Component& moved,
            const Component& reference,
            bool insertAfter);
        // 自身の最初のTコンポーネントを読み取り用に返し、未登録ならnullptrを返します。
        template<typename T>
        [[nodiscard]] const T* GetComponent() const noexcept
        {
            static_assert(std::is_base_of_v<Component, T>, "T must derive from LamaPon::Component.");

            // 所有するコンポーネント
            for (const auto& component : m_components)
            {
                // 型が一致する検索結果
                if (const auto* match = dynamic_cast<const T*>(component.get()))
                {
                    return match;
                }
            }

            return nullptr;
        }

        // 有効なコンポーネントを初期化して毎フレーム更新します(graphics: 描画機器, deltaTime: 経過秒数)。
        void Update(GraphicsDevice& graphics, float deltaTime);
        // 有効なコンポーネントを初期化してフレーム終盤に更新します(graphics: 描画機器, deltaTime: 経過秒数)。
        void LateUpdate(
            GraphicsDevice& graphics,
            float deltaTime);
        // 有効なコンポーネントを初期化して固定周期で更新します(graphics: 描画機器, fixedDeltaTime: 固定刻み秒数)。
        void FixedUpdate(
            GraphicsDevice& graphics,
            float fixedDeltaTime);
        // 有効なコンポーネントを初期化して3D描画します(graphics: 描画機器, view: ビュー行列, projection: 投影行列)。
        void Render3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);
        // 初期化後の有効なコンポーネントに3D事前描画があるか返します(graphics: 描画機器)。
        [[nodiscard]] bool HasPreRender3DPass(
            GraphicsDevice& graphics);
        // 初期化後の有効なコンポーネントにアルファ合成描画があるか返します(graphics: 描画機器)。
        // アルファ合成は不透明描画の後で、カメラから遠い順に振り分けます。
        [[nodiscard]] bool HasAlphaBlended3DPass(
            GraphicsDevice& graphics);
        // 有効な事前描画パスを実行します(graphics: 描画機器, view: ビュー行列, projection: 投影行列)。
        void RenderPre3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);
        // 有効なコンポーネントの3D補助表示を描画します(graphics: 描画機器, view: ビュー行列, projection: 投影行列)。
        void RenderDebug3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);
        // 有効なコンポーネントを初期化して2D描画します(graphics: 描画機器, sprites: スプライト描画状態)。
        void Render2D(
            GraphicsDevice& graphics,
            const SpriteDrawContext& sprites);
        // 0と有効なコンポーネントの描画順序の最大値を返し、値が大きいほど2D描画で手前になります。
        [[nodiscard]] int Render2DSortOrder() const noexcept;

    private:
        friend class Scene;

        // 描画イベントを登録します(graphics: 描画機器, component: 描画対象, kind: イベント種別)。
        // falseならデバッガーの表示上限を超えるため描画を飛ばします。
        [[nodiscard]] bool SubmitFrameDebugEvent(
            GraphicsDevice& graphics,
            const Component& component,
            FrameDebugEventKind kind);

        // 自身と子孫のTを深さ優先で追記します(includeInactive: 無効な階層も検索, results: 収集先)。
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
            // 所有するコンポーネント
            for (const auto& component : m_components)
            {
                // 型が一致する検索結果
                if (auto* match =
                    dynamic_cast<T*>(component.get()))
                {
                    results.push_back(match);
                }
            }
            // 検索する子オブジェクト
            for (auto* child : m_children)
            {
                child->CollectComponentsInChildren<T>(
                    includeInactive,
                    results);
            }
        }

        // 自身が所有する対象を破棄し、削除できたか返します(component: 削除対象)。
        bool RemoveComponent(Component& component) noexcept;
        // 自身と子孫のコンポーネントへ階層の有効状態を通知します。
        void PropagateActiveState();
        // 有効なコンポーネントへ接触開始を通知します(other: 接触相手, normal: 接触法線, point: 接触点, penetration: 貫入量, isTrigger: トリガー接触)。
        void NotifyCollisionEnter(
            GameObject& other,
            const DirectX::XMFLOAT3& normal,
            const DirectX::XMFLOAT3& point,
            float penetration,
            bool isTrigger);
        // 有効なコンポーネントへ接触継続を通知します(other: 接触相手, normal: 接触法線, point: 接触点, penetration: 貫入量, isTrigger: トリガー接触)。
        void NotifyCollisionStay(
            GameObject& other,
            const DirectX::XMFLOAT3& normal,
            const DirectX::XMFLOAT3& point,
            float penetration,
            bool isTrigger);
        // 法線・接触点・貫入量を0として接触終了を通知します(other: 接触相手, isTrigger: トリガー接触)。
        void NotifyCollisionExit(GameObject& other, bool isTrigger);
        // 物理補間を切り替え、有効化時に履歴を初期化します(active: 補間の有効状態)。
        void SetPhysicsInterpolationActive(
            bool active) noexcept;
        // 固定更新の直前に現在の物理変換を前回の状態へ移します。
        void BeginPhysicsInterpolationStep() noexcept;
        // 固定更新の直後に現在の変換を物理補間の終点へ保存します。
        void EndPhysicsInterpolationStep() noexcept;
        // 物理更新外の変換変更を検出して補間履歴を現在値へそろえます。
        void SynchronizePhysicsInterpolation() noexcept;
        // 物理補間の始点と終点を現在の変換へそろえます。
        void ResetPhysicsInterpolation() noexcept;
        // 位置と拡縮を線形補間し回転を球面補間します(alpha: 0〜1に制限する補間率)。
        [[nodiscard]] DirectX::XMMATRIX
            InterpolatedLocalMatrix(
                float alpha) const noexcept;

        // オブジェクトの識別番号
        GameObjectId m_id;
        // オブジェクトの表示名
        std::string m_name;
        // オブジェクトの分類タグ
        std::string m_tag;
        // 親基準のローカル変換
        Transform m_transform;
        // 前回固定更新の変換
        Transform m_previousPhysicsTransform;
        // 今回固定更新の変換
        Transform m_currentPhysicsTransform;
        // 所有するコンポーネント
        std::vector<std::unique_ptr<Component>> m_components;
        // 非所有の親オブジェクト
        GameObject* m_parent{};
        // 非所有の子オブジェクト
        std::vector<GameObject*> m_children;
        // 自身の有効状態
        bool m_enabled{ true };
        // 物理補間の有効状態
        bool m_physicsInterpolationActive{};
        // 物理補間履歴の初期化状態
        bool m_physicsInterpolationInitialized{};
        // シーンをまたぐ保持指定
        bool m_persistent{};
        // 保持する対象の識別キー
        std::string m_persistenceKey;
        // 生成元のシーン番号
        SceneHandle m_sourceScene{};
        // 生成元プレハブのパス
        std::filesystem::path m_prefabAssetPath;
        // 非所有の所属シーン
        Scene* m_scene{};
    };
}
