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
        // アクティブな同期の借用先
        NetworkSceneBridge* activeBridge{};

        // 位置・クォータニオン・倍率を通信形式へ複写する(transform: 元のシーン変換)。
        NetworkTransform Capture(const Transform& transform)
        {
            return { { transform.position.x, transform.position.y, transform.position.z },
                { transform.rotationQuaternion.x, transform.rotationQuaternion.y,
                    transform.rotationQuaternion.z, transform.rotationQuaternion.w },
                { transform.scale.x, transform.scale.y, transform.scale.z } };
        }
        // 位置と倍率を線形・回転を球面補間する(transform: 反映先の変換, target: 目標の通信変換, alpha: 反映割合)。
        void Apply(Transform& transform, const NetworkTransform& target, const float alpha)
        {
            using namespace DirectX;
            // 補間先の位置
            const XMFLOAT3 position{ target.position[0], target.position[1], target.position[2] };
            // 補間先の倍率
            const XMFLOAT3 scale{ target.scale[0], target.scale[1], target.scale[2] };
            // 補間先の回転クォータニオン
            const XMFLOAT4 rotation{ target.rotation[0], target.rotation[1], target.rotation[2], target.rotation[3] };
            XMStoreFloat3(&transform.position, XMVectorLerp(XMLoadFloat3(&transform.position), XMLoadFloat3(&position), alpha));
            XMStoreFloat3(&transform.scale, XMVectorLerp(XMLoadFloat3(&transform.scale), XMLoadFloat3(&scale), alpha));
            transform.SetRotationVector(XMQuaternionSlerp(transform.RotationVector(), XMLoadFloat4(&rotation), alpha));
        }
        // 参加側で無効化するシミュレーション型かを判定する(type: Component型名)。
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
                // Componentを復元する対象のID
                GameObjectId object{};
                // Componentの格納位置
                std::size_t index{};
                // 復元時に照合する型名
                std::string type;
            };
            // 対応するシーン内オブジェクトID
            GameObjectId object{};
            // Prefabから動的に生成したか
            bool dynamic{};
            // 接続前の有効状態
            bool enabled{};
            // 接続前の位置と回転と倍率
            NetworkTransform original;
            // 復元するComponentの記録
            std::vector<Disabled> disabled;
            // 削除済みの固定同期対象か
            bool retired{};
            // 接続前の同期データ
            std::string originalData;
        };
        // 借用する同期対象シーン
        Scene& scene;
        // 借用する通信セッション
        NetworkSession& session;
        // 対応を作ったシーンの変更番号
        std::uint64_t revision{};
        // 対応を作った通信の世代番号
        std::uint64_t generation{};
        // 通信IDとシーン内の対応一覧
        std::map<NetworkObjectId, Binding> bindings;
        // ホストが削除した固定オブジェクトを同じ接続中に再登録しません。
        // 再登録しない固定対象の識別子
        std::vector<std::string> retiredKeys;

        // シーン変更番号と通信世代を記録する(value: 借用するシーン, network: 借用する通信状態)。
        Implementation(Scene& value, NetworkSession& network)
            : scene(value), session(network), revision(value.ContentRevision()), generation(network.Generation()) {}

        // ホスト専用の同期対象のシミュレーションを止める(object: 同期対象のroot, binding: 復元状態の出力先)。
        void DisableSimulation(GameObject& object, Binding& binding)
        {
            // 対象のNetworkIdentity
            const auto* identity = object.GetComponent<NetworkIdentityComponent>();
            if (!identity || !identity->HostOnlySimulation()) return;
            DisableTree(object, binding);
        }

        // 子を含む有効なシミュレーションを止めて復元位置を記録する(object: 処理するオブジェクト, binding: 復元状態の出力先)。
        void DisableTree(GameObject& object, Binding& binding)
        {
            // Componentの格納位置
            for (std::size_t index = 0; index < object.Components().size(); ++index)
            {
                // 現在処理するComponent
                auto& component = object.Components()[index];
                if ((SimulationComponent(component->TypeName()) || component->ScriptInstance() != nullptr)
                    && component->IsEnabled())
                {
                    // 無効化の重複を調べる(entry: 記録済みのComponent位置)。
                    if (std::ranges::none_of(binding.disabled, [index, &object](const auto& entry)
                        { return entry.index == index && entry.object == object.Id(); }))
                        binding.disabled.push_back({ object.Id(), index, std::string(component->TypeName()) });
                    component->SetEnabled(false);
                }
            }
            // 無効化対象の子オブジェクト
            for (auto* child : object.Children()) DisableTree(*child, binding);
        }

        // 登録済みPrefabをrootとして生成して初期状態を反映する(state: 通信オブジェクト状態)。
        GameObject* Instantiate(const NetworkObjectState& state)
        {
            // 登録済みの同期Prefab一覧
            const auto& prefabs = session.Configuration().prefabs;
            // 登録または対応の検索結果
            const auto found = std::ranges::find(prefabs, state.prefabKey, &NetworkPrefabRegistration::key);
            if (found == prefabs.end()) throw std::runtime_error("同期Prefabがプロジェクトへ登録されていません: " + state.prefabKey);
            // 同期対象のシーン内オブジェクト
            auto& object = scene.InstantiatePrefab(Utf8ToWide(found->assetPath));
            if (object.Parent() != nullptr) throw std::runtime_error("同期Prefabのrootに親を設定できません。");
            // 対象のNetworkIdentity
            auto* identity = object.GetComponent<NetworkIdentityComponent>();
            if (!identity) identity = &object.AddComponent<NetworkIdentityComponent>();
            identity->SetSceneKey({});
            identity->BindNetworkId(state.id, state.owner);
            Apply(object.GetTransform(), state.transform, 1);
            object.SetEnabled(state.enabled);
            identity->SetReplicatedData(state.data);
            return &object;
        }

        // 世代を照合して生成・削除・参加側の状態を反映する(seconds: 経過秒数, advanceInterpolation: 補間を進めるか)。
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
            // 再接続で通信IDが再利用されても、古いGameObjectとの対応は復元して破棄します。
            if (generation != session.Generation()) Restore();
            if (session.State() == NetworkState::Stopped || session.State() == NetworkState::Error)
            {
                Restore();
                return;
            }
            if (!session.IsHost() && session.State() != NetworkState::Connected) return;

            if (session.IsHost())
            {
                // 同期対象のシーン内オブジェクト
                for (const auto& object : scene.GameObjects())
                {
                    // 対象のNetworkIdentity
                    auto* identity = object->GetComponent<NetworkIdentityComponent>();
                    if (!identity || !identity->IsEnabled() || identity->SceneKey().empty()
                        || identity->NetworkId() != 0
                        || std::ranges::find(retiredKeys, identity->SceneKey()) != retiredKeys.end()) continue;
                    if (object->Parent() != nullptr) throw std::runtime_error("固定同期オブジェクトはSceneのrootに配置してください。");
                    // 通信上のオブジェクト状態
                    NetworkObjectState state;
                    state.sceneKey = identity->SceneKey(); state.transform = Capture(object->GetTransform());
                    state.enabled = object->IsEnabled(); state.data = identity->ReplicatedData();
                    // 通信オブジェクトID
                    const auto id = session.Spawn(state);
                    if (id == 0) throw std::runtime_error("Scene Keyが重複しているか、同期オブジェクトの上限を超えています。");
                    identity->BindNetworkId(id, 1);
                    // 通信IDとシーン内の対応情報
                    auto binding = Binding{ object->Id(), false, object->IsEnabled(), Capture(object->GetTransform()), {} };
                    binding.originalData = identity->ReplicatedData();
                    bindings.emplace(id, std::move(binding));
                }
            }
            // 通信上のオブジェクト状態
            for (const auto& state : session.Objects())
            {
                // 通信IDとシーン内の対応情報
                auto binding = bindings.find(state.id);
                if (binding == bindings.end())
                {
                    // 同期対象のシーン内オブジェクト
                    GameObject* object{};
                    if (!state.sceneKey.empty())
                    {
                        // 固定対象の照合候補
                        for (const auto& candidate : scene.GameObjects())
                        {
                            // 対象のNetworkIdentity
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
                    // 対象のNetworkIdentity
                    auto* identity = object->GetComponent<NetworkIdentityComponent>();
                    identity->BindNetworkId(state.id, state.owner);
                    binding = bindings.emplace(state.id, Binding{ object->Id(), state.sceneKey.empty(),
                        object->IsEnabled(), Capture(object->GetTransform()), {} }).first;
                    binding->second.originalData = identity->ReplicatedData();
                    if (!session.IsHost()) Apply(object->GetTransform(), state.transform, 1);
                }
                // 同期対象のシーン内オブジェクト
                auto* object = scene.FindGameObject(binding->second.object);
                if (!object) continue;
                // 対象のNetworkIdentity
                auto* identity = object->GetComponent<NetworkIdentityComponent>();
                if (!identity) throw std::runtime_error("接続中にNetwork Identityを削除できません。");
                identity->BindNetworkId(state.id, state.owner);
                if (!session.IsHost())
                {
                    DisableSimulation(*object, binding->second);
                    if (advanceInterpolation)
                    {
                        // 補間の時定数・秒
                        const float interval = identity->InterpolationSeconds();
                        // 指数補間の反映割合
                        const float alpha = interval <= 0 ? 1.0f : 1.0f - std::exp(-seconds / interval);
                        Apply(object->GetTransform(), state.transform, alpha);
                    }
                    object->SetEnabled(state.enabled);
                    identity->SetReplicatedData(state.data);
                }
            }
            // 削除または退場した通信ID
            std::vector<NetworkObjectId> lost;
            // 通信IDとシーン内の対応
            for (const auto& [id, binding] : bindings)
            {
                if (!binding.retired && (!session.FindObject(id)
                    || (session.IsHost() && !scene.FindGameObject(binding.object)))) lost.push_back(id);
            }
            // 通信オブジェクトID
            for (const auto id : lost)
            {
                // 通信IDとシーン内の対応情報
                const auto binding = bindings.at(id);
                // 通信上のオブジェクト状態
                if (const auto* state = session.FindObject(id); state && !state->sceneKey.empty()) retiredKeys.push_back(state->sceneKey);
                if (session.IsHost()) session.Despawn(id);
                // 同期対象のシーン内オブジェクト
                if (auto* object = scene.FindGameObject(binding.object))
                {
                    if (binding.dynamic) scene.DestroyGameObject(*object);
                    else
                    {
                        // 対象のNetworkIdentity
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

        // 固定対象とComponentを復元し、動的対象と通信IDの対応を破棄する。
        void Restore()
        {
            if (revision == scene.ContentRevision())
            {
                // 通信IDとシーン内の対応
                for (const auto& [id, binding] : bindings)
                {
                    static_cast<void>(id);
                    // 同期対象のシーン内オブジェクト
                    auto* object = scene.FindGameObject(binding.object);
                    if (!object) continue;
                    if (binding.dynamic) { scene.DestroyGameObject(*object); continue; }
                    Apply(object->GetTransform(), binding.original, 1);
                    object->SetEnabled(binding.enabled);
                    // 復元するComponentの記録
                    for (const auto& disabled : binding.disabled)
                    {
                        // 復元対象のシーンオブジェクト
                        auto* target = scene.FindGameObject(disabled.object);
                        if (target && disabled.index < target->Components().size()
                            && target->Components()[disabled.index]->TypeName() == disabled.type)
                            target->Components()[disabled.index]->SetEnabled(true);
                    }
                    // 対象のNetworkIdentity
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
    // 通信状態を反映して参加側の補間を進める(seconds: 経過秒数)。
    void NetworkSceneBridge::BeforeSimulation(const float seconds)
    {
        try { m_impl->Synchronize(seconds, true); }
        // 同期の例外(error: 失敗理由)。
        catch (const std::exception& error)
        {
            Logger::Instance().Error(std::string("P2P Scene同期を終了しました: ") + error.what());
            m_impl->session.Abort(error.what()); m_impl->Restore();
        }
    }
    // Script終了後のホスト状態を次の送信へ登録する(seconds: 経過秒数)。
    void NetworkSceneBridge::AfterSimulation(const float seconds)
    {
        try
        {
            m_impl->Synchronize(seconds, false);
            if (!m_impl->session.IsHost()) return;
            // Script終了後のTransformと生成・削除を同じフレームで採り、SetObjectは次の通信tickで送ります。
            // 通信IDとシーン内の対応
            for (const auto& [id, binding] : m_impl->bindings)
            {
                // 同期対象のシーン内オブジェクト
                auto* object = m_impl->scene.FindGameObject(binding.object);
                // 現在の通信オブジェクト状態
                const auto* current = m_impl->session.FindObject(id);
                if (!object || !current) continue;
                // 通信上のオブジェクト状態
                auto state = *current;
                state.transform = Capture(object->GetTransform()); state.enabled = object->IsEnabled();
                // 対象のNetworkIdentity
                if (const auto* identity = object->GetComponent<NetworkIdentityComponent>()) state.data = identity->ReplicatedData();
                if (!m_impl->session.SetObject(std::move(state))) throw std::runtime_error("同期対象のTransformまたはデータが無効です。");
            }
        }
        // 同期の例外(error: 失敗理由)。
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
        // 通信上のオブジェクト状態
        NetworkObjectState state;
        state.prefabKey = prefabKey; state.transform = transform; state.owner = owner;
        // Prefabを先に作り、失敗した生成を参加者へ配信しません。
        // 同期対象のシーン内オブジェクト
        GameObject* object{};
        try
        {
            object = m_impl->Instantiate(state);
            // 通信オブジェクトID
            const auto id = m_impl->session.Spawn(state);
            if (id == 0) { m_impl->scene.DestroyGameObject(*object); return nullptr; }
            object->GetComponent<NetworkIdentityComponent>()->BindNetworkId(id, owner);
            m_impl->bindings.emplace(id, Implementation::Binding{ object->Id(), true, object->IsEnabled(), Capture(object->GetTransform()), {} });
            return object;
        }
        // 同期の例外(error: 失敗理由)。
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
        // 登録または対応の検索結果
        const auto found = m_impl->bindings.find(id);
        return found == m_impl->bindings.end() ? nullptr : m_impl->scene.FindGameObject(found->second.object);
    }
    NetworkSceneBridge* ActiveNetworkSceneBridge() noexcept { return activeBridge; }
    void SetActiveNetworkSceneBridge(NetworkSceneBridge* bridge) noexcept { activeBridge = bridge; }
}
