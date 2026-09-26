#include "LamaPon/Online/NetworkSceneBridge.h"

#include "LamaPon/Components/NetworkIdentityComponent.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace LamaPon
{
    namespace
    {
        NetworkSceneBridge* activeBridge{};

        NetworkTransform Capture(const Transform& transform)
        {
            return { { transform.position.x, transform.position.y, transform.position.z },
                { transform.rotationQuaternion.x, transform.rotationQuaternion.y,
                    transform.rotationQuaternion.z, transform.rotationQuaternion.w },
                { transform.scale.x, transform.scale.y, transform.scale.z } };
        }
        void Apply(Transform& transform, const NetworkTransform& target, const float alpha)
        {
            using namespace DirectX;
            const XMFLOAT3 position{ target.position[0], target.position[1], target.position[2] };
            const XMFLOAT3 scale{ target.scale[0], target.scale[1], target.scale[2] };
            const XMFLOAT4 rotation{ target.rotation[0], target.rotation[1], target.rotation[2], target.rotation[3] };
            XMStoreFloat3(&transform.position, XMVectorLerp(XMLoadFloat3(&transform.position), XMLoadFloat3(&position), alpha));
            XMStoreFloat3(&transform.scale, XMVectorLerp(XMLoadFloat3(&transform.scale), XMLoadFloat3(&scale), alpha));
            transform.SetRotationVector(XMQuaternionSlerp(transform.RotationVector(), XMLoadFloat4(&rotation), alpha));
        }
        bool SimulationComponent(const std::string_view type)
        {
            return type == "Rigidbody" || type == "InputMover" || type == "CharacterController"
                || type == "Rotator" || type == "TransformAnimator" || type == "NavMeshAgent"
                || type == "NativeScript" || type == "Joint";
        }
    }

    struct NetworkSceneBridge::Implementation final
    {
        struct Binding final
        {
            struct Disabled final
            {
                GameObjectId object{};
                std::size_t index{};
                std::string type;
            };
            GameObjectId object{};
            bool dynamic{};
            bool enabled{};
            NetworkTransform original;
            std::vector<Disabled> disabled;
            bool retired{};
            std::string originalData;
        };
        Scene& scene;
        NetworkSession& session;
        std::uint64_t revision{};
        std::uint64_t generation{};
        std::map<NetworkObjectId, Binding> bindings;
        // ホストが削除した固定オブジェクトを同じ接続中に再登録しません。
        std::vector<std::string> retiredKeys;

        Implementation(Scene& value, NetworkSession& network)
            : scene(value), session(network), revision(value.ContentRevision()), generation(network.Generation()) {}

        void DisableSimulation(GameObject& object, Binding& binding)
        {
            const auto* identity = object.GetComponent<NetworkIdentityComponent>();
            if (!identity || !identity->HostOnlySimulation()) return;
            DisableTree(object, binding);
        }

        void DisableTree(GameObject& object, Binding& binding)
        {
            for (std::size_t index = 0; index < object.Components().size(); ++index)
            {
                auto& component = object.Components()[index];
                if ((SimulationComponent(component->TypeName()) || component->ScriptInstance() != nullptr)
                    && component->IsEnabled())
                {
                    if (std::ranges::none_of(binding.disabled, [index, &object](const auto& entry)
                        { return entry.index == index && entry.object == object.Id(); }))
                        binding.disabled.push_back({ object.Id(), index, std::string(component->TypeName()) });
                    component->SetEnabled(false);
                }
            }
            for (auto* child : object.Children()) DisableTree(*child, binding);
        }

        GameObject* Instantiate(const NetworkObjectState& state)
        {
            const auto& prefabs = session.Configuration().prefabs;
            const auto found = std::ranges::find(prefabs, state.prefabKey, &NetworkPrefabRegistration::key);
            if (found == prefabs.end()) throw std::runtime_error("同期Prefabがプロジェクトへ登録されていません: " + state.prefabKey);
            auto& object = scene.InstantiatePrefab(Utf8ToWide(found->assetPath));
            if (object.Parent() != nullptr) throw std::runtime_error("同期Prefabのrootに親を設定できません。");
            auto* identity = object.GetComponent<NetworkIdentityComponent>();
            if (!identity) identity = &object.AddComponent<NetworkIdentityComponent>();
            identity->SetSceneKey({});
            identity->BindNetworkId(state.id, state.owner);
            Apply(object.GetTransform(), state.transform, 1);
            object.SetEnabled(state.enabled);
            identity->SetReplicatedData(state.data);
            return &object;
        }

        void Synchronize(const float seconds, const bool advanceInterpolation)
        {
            if (revision != scene.ContentRevision())
            {
                // Scene切り替えでGameObject IDが再利用される前に、古い対応を破棄します。
                session.Stop(); bindings.clear(); retiredKeys.clear();
                revision = scene.ContentRevision();
                generation = session.Generation();
                Logger::Instance().Warning("Sceneが切り替わったため、P2P接続を終了しました。");
                return;
            }
            // Stopから再接続までScene更新が無くても、再利用した通信IDを
            // 前のセッションのGameObjectへ結び付けません。
            if (generation != session.Generation()) Restore();
            if (session.State() == NetworkState::Stopped || session.State() == NetworkState::Error)
            {
                Restore();
                return;
            }
            if (!session.IsHost() && session.State() != NetworkState::Connected) return;

            if (session.IsHost())
            {
                for (const auto& object : scene.GameObjects())
                {
                    auto* identity = object->GetComponent<NetworkIdentityComponent>();
                    if (!identity || !identity->IsEnabled() || identity->SceneKey().empty()
                        || identity->NetworkId() != 0
                        || std::ranges::find(retiredKeys, identity->SceneKey()) != retiredKeys.end()) continue;
                    if (object->Parent() != nullptr) throw std::runtime_error("固定同期オブジェクトはSceneのrootに配置してください。");
                    NetworkObjectState state;
                    state.sceneKey = identity->SceneKey(); state.transform = Capture(object->GetTransform());
                    state.enabled = object->IsEnabled(); state.data = identity->ReplicatedData();
                    const auto id = session.Spawn(state);
                    if (id == 0) throw std::runtime_error("Scene Keyが重複しているか、同期オブジェクトの上限を超えています。");
                    identity->BindNetworkId(id, 1);
                    auto binding = Binding{ object->Id(), false, object->IsEnabled(), Capture(object->GetTransform()), {} };
                    binding.originalData = identity->ReplicatedData();
                    bindings.emplace(id, std::move(binding));
                }
            }
            for (const auto& state : session.Objects())
            {
                auto binding = bindings.find(state.id);
                if (binding == bindings.end())
                {
                    GameObject* object{};
                    if (!state.sceneKey.empty())
                    {
                        for (const auto& candidate : scene.GameObjects())
                        {
                            auto* identity = candidate->GetComponent<NetworkIdentityComponent>();
                            if (identity && identity->SceneKey() == state.sceneKey)
                            {
                                if (object != nullptr) throw std::runtime_error("Scene Keyが重複しています: " + state.sceneKey);
                                object = candidate.get();
                            }
                        }
                        if (!object || object->Parent() != nullptr) throw std::runtime_error("対応する固定同期オブジェクトがありません: " + state.sceneKey);
                    }
                    else object = Instantiate(state);
                    auto* identity = object->GetComponent<NetworkIdentityComponent>();
                    identity->BindNetworkId(state.id, state.owner);
                    binding = bindings.emplace(state.id, Binding{ object->Id(), state.sceneKey.empty(),
                        object->IsEnabled(), Capture(object->GetTransform()), {} }).first;
                    binding->second.originalData = identity->ReplicatedData();
                    if (!session.IsHost()) Apply(object->GetTransform(), state.transform, 1);
                }
                auto* object = scene.FindGameObject(binding->second.object);
                if (!object) continue;
                auto* identity = object->GetComponent<NetworkIdentityComponent>();
                if (!identity) throw std::runtime_error("接続中にNetwork Identityを削除できません。");
                identity->BindNetworkId(state.id, state.owner);
                if (!session.IsHost())
                {
                    DisableSimulation(*object, binding->second);
                    if (advanceInterpolation)
                    {
                        const float interval = identity->InterpolationSeconds();
                        const float alpha = interval <= 0 ? 1.0f : 1.0f - std::exp(-seconds / interval);
                        Apply(object->GetTransform(), state.transform, alpha);
                    }
                    object->SetEnabled(state.enabled);
                    identity->SetReplicatedData(state.data);
                }
            }
            std::vector<NetworkObjectId> lost;
            for (const auto& [id, binding] : bindings)
            {
                if (!binding.retired && (!session.FindObject(id)
                    || (session.IsHost() && !scene.FindGameObject(binding.object)))) lost.push_back(id);
            }
            for (const auto id : lost)
            {
                const auto binding = bindings.at(id);
                if (const auto* state = session.FindObject(id); state && !state->sceneKey.empty()) retiredKeys.push_back(state->sceneKey);
                if (session.IsHost()) session.Despawn(id);
                if (auto* object = scene.FindGameObject(binding.object))
                {
                    if (binding.dynamic) scene.DestroyGameObject(*object);
                    else
                    {
                        auto* identity = object->GetComponent<NetworkIdentityComponent>();
                        if (identity) retiredKeys.push_back(identity->SceneKey());
                        object->SetEnabled(false);
                    }
                }
                // 固定オブジェクトは切断時の復元に必要なので対応を残します。
                if (binding.dynamic || !scene.FindGameObject(binding.object)) bindings.erase(id);
                else bindings.at(id).retired = true;
            }
        }

        void Restore()
        {
            if (revision == scene.ContentRevision())
            {
                for (const auto& [id, binding] : bindings)
                {
                    static_cast<void>(id);
                    auto* object = scene.FindGameObject(binding.object);
                    if (!object) continue;
                    if (binding.dynamic) { scene.DestroyGameObject(*object); continue; }
                    Apply(object->GetTransform(), binding.original, 1);
                    object->SetEnabled(binding.enabled);
                    for (const auto& disabled : binding.disabled)
                    {
                        auto* target = scene.FindGameObject(disabled.object);
                        if (target && disabled.index < target->Components().size()
                            && target->Components()[disabled.index]->TypeName() == disabled.type)
                            target->Components()[disabled.index]->SetEnabled(true);
                    }
                    if (auto* identity = object->GetComponent<NetworkIdentityComponent>())
                    {
                        identity->BindNetworkId(0, 1);
                        identity->SetReplicatedData(binding.originalData);
                    }
                }
            }
            bindings.clear(); retiredKeys.clear(); revision = scene.ContentRevision();
            generation = session.Generation();
        }
    };

    NetworkSceneBridge::NetworkSceneBridge(Scene& scene, NetworkSession& session)
        : m_impl(std::make_unique<Implementation>(scene, session)) {}
    NetworkSceneBridge::~NetworkSceneBridge()
    {
        if (activeBridge == this) activeBridge = nullptr;
    }
    void NetworkSceneBridge::BeforeSimulation(const float seconds)
    {
        try { m_impl->Synchronize(seconds, true); }
        catch (const std::exception& error)
        {
            Logger::Instance().Error(std::string("P2P Scene同期を終了しました: ") + error.what());
            m_impl->session.Abort(error.what()); m_impl->Restore();
        }
    }
    void NetworkSceneBridge::AfterSimulation(const float seconds)
    {
        try
        {
            m_impl->Synchronize(seconds, false);
            if (!m_impl->session.IsHost()) return;
            // SetObjectは次の通信tickで送ります。GameObjectの生成と削除を
            // 同じフレームで反映し、Scriptが返った後の最終Transformを採ります。
            for (const auto& [id, binding] : m_impl->bindings)
            {
                auto* object = m_impl->scene.FindGameObject(binding.object);
                const auto* current = m_impl->session.FindObject(id);
                if (!object || !current) continue;
                auto state = *current;
                state.transform = Capture(object->GetTransform()); state.enabled = object->IsEnabled();
                if (const auto* identity = object->GetComponent<NetworkIdentityComponent>()) state.data = identity->ReplicatedData();
                if (!m_impl->session.SetObject(std::move(state))) throw std::runtime_error("同期対象のTransformまたはデータが無効です。");
            }
        }
        catch (const std::exception& error)
        {
            Logger::Instance().Error(std::string("P2P Scene同期を終了しました: ") + error.what());
            m_impl->session.Abort(error.what()); m_impl->Restore();
        }
    }
    void NetworkSceneBridge::Reset() { m_impl->session.Stop(); m_impl->Restore(); }
    GameObject* NetworkSceneBridge::Spawn(const std::string_view prefabKey,
        const NetworkTransform& transform, const NetworkPeerId owner)
    {
        if (!m_impl->session.IsHost()) return nullptr;
        NetworkObjectState state;
        state.prefabKey = prefabKey; state.transform = transform; state.owner = owner;
        // Prefabを先に作り、失敗した生成を参加者へ配信しません。
        GameObject* object{};
        try
        {
            object = m_impl->Instantiate(state);
            const auto id = m_impl->session.Spawn(state);
            if (id == 0) { m_impl->scene.DestroyGameObject(*object); return nullptr; }
            object->GetComponent<NetworkIdentityComponent>()->BindNetworkId(id, owner);
            m_impl->bindings.emplace(id, Implementation::Binding{ object->Id(), true, object->IsEnabled(), Capture(object->GetTransform()), {} });
            return object;
        }
        catch (const std::exception& error)
        {
            if (object) m_impl->scene.DestroyGameObject(*object);
            Logger::Instance().Error(std::string("NetworkSpawnに失敗しました: ") + error.what());
            return nullptr;
        }
    }
    bool NetworkSceneBridge::Despawn(const NetworkObjectId id) { return m_impl->session.Despawn(id); }
    GameObject* NetworkSceneBridge::Find(const NetworkObjectId id) const noexcept
    {
        const auto found = m_impl->bindings.find(id);
        return found == m_impl->bindings.end() ? nullptr : m_impl->scene.FindGameObject(found->second.object);
    }
    NetworkSceneBridge* ActiveNetworkSceneBridge() noexcept { return activeBridge; }
    void SetActiveNetworkSceneBridge(NetworkSceneBridge* bridge) noexcept { activeBridge = bridge; }
}
