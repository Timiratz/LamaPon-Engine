#pragma once

#include "LamaPon/Core/PlayerPrefs.h"
#include "LamaPon/Online/OnlineServices.h"
#include "LamaPon/Online/NetworkSession.h"
#include "LamaPon/Online/NetworkSceneBridge.h"
#include "LamaPon/Components/NetworkIdentityComponent.h"
#include "LamaPon/Scripting/Coroutine.h"
#include "LamaPon/Scripting/GameModule.h"

#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/Scene.h"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace LamaPon
{
    class DataAsset;
    class NativeScriptComponent;
    class GraphicsDevice;
    struct CollisionEvent;

    namespace Detail
    {
        template<typename TScript>
        struct ScriptBridge;
    }

    // 所有物体・描画サービス・EventBusはスクリプトより長く存続させ、操作はメインスレッドで行う。
    class Script
    {
    public:
        // 予約したコルーチンを破棄し、存続するEventBusの購読を解除する。
        virtual ~Script()
        {
            StopAllCoroutines();
            // イベント購読を自動解除します。
            // 所有スクリプトのイベント購読
            for (const auto& subscription :
                m_eventSubscriptions)
            {
                subscription.first->Unsubscribe(
                    subscription.second);
            }
        }

        // 生成と初期プロパティ読み込みの直後に1回だけ呼ばれる。
        virtual void Awake()
        {
        }

        // 最初のUpdate直前に1回だけ呼ばれる。
        virtual void Start()
        {
        }

        // 実効アクティブ状態が有効へ変わったとき呼ばれる。
        virtual void OnEnable()
        {
        }

        // 実効アクティブ状態が無効へ変わったとき呼ばれる。
        virtual void OnDisable()
        {
        }

        // インスタンス破棄直前に呼ばれる。
        virtual void OnDestroy()
        {
        }

        // フレーム入力やUIを更新し、この後に0回以上の固定更新が続く。
        virtual void Update(float)
        {
        }

        // カメラなどの追従は物理の補間時刻を参照して描画と合わせる。
        // 全更新と固定更新・物理計算の後に呼ばれる。
        virtual void LateUpdate(float)
        {
        }

        // プロジェクトで指定した固定刻みで物理操作などを更新する。
        virtual void FixedUpdate(float)
        {
        }

        // 通常コライダーの接触開始時に呼ばれる。
        virtual void OnCollisionEnter(const CollisionEvent&)
        {
        }

        // 通常コライダーの接触継続時に呼ばれる。
        virtual void OnCollisionStay(const CollisionEvent&)
        {
        }

        // 通常コライダーの接触終了時に呼ばれる。
        virtual void OnCollisionExit(const CollisionEvent&)
        {
        }

        // トリガーコライダーの接触開始時に呼ばれる。
        virtual void OnTriggerEnter(const CollisionEvent&)
        {
        }

        // トリガーコライダーの接触継続時に呼ばれる。
        virtual void OnTriggerStay(const CollisionEvent&)
        {
        }

        // トリガーコライダーの接触終了時に呼ばれる。
        virtual void OnTriggerExit(const CollisionEvent&)
        {
        }

    protected:
        // DLL側の具体型から構築するスクリプトを作る。
        Script() = default;

        // 通知はUpdate直前に処理し、倍率適用後のゲーム時間で待つ。
        // ゲーム時間で1回の実行を予約し解除番号を返す(delaySeconds: 待機秒数, callback: 実行する処理)。
        std::uint64_t Invoke(
            const float delaySeconds,
            std::function<void()> callback)
        {
            // タイマーまたは継続の識別番号
            const auto id = m_nextTimerId++;
            m_timers.push_back({
                id,
                std::max(delaySeconds, 0.0f),
                -1.0f,
                std::move(callback) });
            return id;
        }

        // ゲーム時間で繰り返し実行を予約する(delaySeconds: 初回待機秒数, intervalSeconds: 最低0.001秒の間隔, callback: 実行する処理)。
        std::uint64_t InvokeRepeating(
            const float delaySeconds,
            const float intervalSeconds,
            std::function<void()> callback)
        {
            // タイマーまたは継続の識別番号
            const auto id = m_nextTimerId++;
            m_timers.push_back({
                id,
                std::max(delaySeconds, 0.0f),
                std::max(intervalSeconds, 0.001f),
                std::move(callback) });
            return id;
        }

        // 指定番号の実行予約を取り除く(handle: Invoke系が返す予約番号)。
        void CancelInvoke(const std::uint64_t handle)
        {
            // 指定番号の予約を除去する(entry: 確認対象のタイマー)。
            std::erase_if(
                m_timers,
                [handle](const TimerEntry& entry)
                {
                    return entry.id == handle;
                });
        }

        // 全タイマー予約を取り除く。
        void CancelAllInvokes()
        {
            m_timers.clear();
        }

        // 最初の中断まで即時実行し継続番号を返す(coroutine: 所有を移すコルーチン)。
        std::uint64_t StartCoroutine(Coroutine coroutine)
        {
            // 予約・購読・本体の操作番号
            const auto handle = coroutine.Release();
            if (!handle)
            {
                return 0;
            }
            handle.resume();
            if (handle.done())
            {
                // 本体が保持した再送出例外
                const auto exception =
                    handle.promise().exception;
                handle.destroy();
                if (exception)
                {
                    std::rethrow_exception(exception);
                }
                return 0;
            }
            // タイマーまたは継続の識別番号
            const auto id = m_nextCoroutineId++;
            m_coroutines.push_back(
                { id, handle, false });
            return id;
        }

        // 再開中の自身の破棄を遅らせて停止する(handle: StartCoroutineが返す番号)。
        void StopCoroutine(const std::uint64_t handle)
        {
            // 走査または再検索する実行項目
            for (auto& entry : m_coroutines)
            {
                if (entry.id != handle || entry.id == 0)
                {
                    continue;
                }

                if (entry.id == m_resumingCoroutineId)
                {
                    entry.stopped = true;
                }
                else
                {
                    entry.handle.destroy();
                    entry.id = 0;
                }
                return;
            }
        }

        // 再開中の自身を除き全コルーチンを破棄し、自身は再開後に破棄する。
        void StopAllCoroutines()
        {
            // 走査または再検索する実行項目
            for (auto& entry : m_coroutines)
            {
                if (entry.id == 0)
                {
                    continue;
                }
                if (entry.id == m_resumingCoroutineId)
                {
                    entry.stopped = true;
                }
                else
                {
                    entry.handle.destroy();
                    entry.id = 0;
                }
            }
            // 破棄済みの本体を実行列から取り除く(entry: 確認対象の本体)。
            std::erase_if(
                m_coroutines,
                [](const CoroutineEntry& entry)
                {
                    return entry.id == 0;
                });
        }

        // 初期化済みの所有GameObjectを借用参照する。
        [[nodiscard]] GameObject& Owner() noexcept
        {
            assert(m_owner != nullptr);
            return *m_owner;
        }

        // 初期化済みの所有GameObjectを読み取り参照する。
        [[nodiscard]] const GameObject& Owner() const noexcept
        {
            assert(m_owner != nullptr);
            return *m_owner;
        }

        // 初期化済みの描画サービスを借用参照する。
        [[nodiscard]] GraphicsDevice& Graphics() noexcept
        {
            assert(m_graphics != nullptr);
            return *m_graphics;
        }

        // 初期化済みの描画サービスを読み取り参照する。
        [[nodiscard]] const GraphicsDevice& Graphics() const noexcept
        {
            assert(m_graphics != nullptr);
            return *m_graphics;
        }

        // ゲーム画面の論理解像度を変更する(width: 横幅px, height: 高さpx)。
        [[nodiscard]] bool SetWindowSize(
            const std::uint32_t width,
            const std::uint32_t height)
        {
            return GetScene().SetWindowSize(width, height);
        }

        // ゲーム画面の論理解像度を返す。
        [[nodiscard]] std::pair<std::uint32_t, std::uint32_t>
            WindowSize() const
        {
            return GetScene().WindowSize();
        }



        // 自分のGameObjectから指定型の成分を借用する。
        template<typename T>
        [[nodiscard]] T* GetComponent() noexcept
        {
            return Owner().GetComponent<T>();
        }

        // 自分のGameObjectから指定型の成分を読み取り借用する。
        template<typename T>
        [[nodiscard]] const T* GetComponent() const noexcept
        {
            return Owner().GetComponent<T>();
        }

        // 自分のGameObjectから基底やインターフェースでスクリプトを借用する。
        template<typename T>
        [[nodiscard]] T* GetScript() const noexcept
        {
            return Owner().GetScript<T>();
        }

        // 自分と子孫からスクリプトを借用する(includeInactive: 無効な物体も検索するか)。
        template<typename T>
        [[nodiscard]] T* GetScriptInChildren(
            const bool includeInactive = false) const noexcept
        {
            return Owner().GetScriptInChildren<T>(
                includeInactive);
        }

        // 自分と子孫から成分を借用する(includeInactive: 無効な物体も検索するか)。
        template<typename T>
        [[nodiscard]] T* GetComponentInChildren(
            const bool includeInactive = false) noexcept
        {
            return Owner().GetComponentInChildren<T>(
                includeInactive);
        }

        // 自分と祖先から成分を借用する(includeInactive: 無効な物体も検索するか)。
        template<typename T>
        [[nodiscard]] T* GetComponentInParent(
            const bool includeInactive = false) noexcept
        {
            return Owner().GetComponentInParent<T>(
                includeInactive);
        }

        // 自分のGameObjectへ指定型の成分を作る(args: 成分の構築引数)。
        template<typename T, typename... Args>
        T& AddComponent(Args&&... args)
        {
            return Owner().AddComponent<T>(
                std::forward<Args>(args)...);
        }

        // 自分のTransformを借用参照する。
        [[nodiscard]] Transform& GetTransform() noexcept
        {
            return Owner().GetTransform();
        }

        // 自分のTransformを読み取り借用する。
        [[nodiscard]] const Transform&
            GetTransform() const noexcept
        {
            return Owner().GetTransform();
        }

        // 自分が属するSceneを借用参照する。
        [[nodiscard]] Scene& GetScene() const noexcept
        {
            return Owner().GetScene();
        }

        // Scene内で名前の一致する最初の物体を借用する(name: 検索する名前)。
        [[nodiscard]] GameObject* Find(
            const std::string_view name) const noexcept
        {
            return Owner().GetScene()
                .FindGameObjectByName(name);
        }

        // Scene内でタグの一致する最初の物体を借用する(tag: 検索するタグ)。
        [[nodiscard]] GameObject* FindWithTag(
            const std::string_view tag) const noexcept
        {
            return Owner().GetScene()
                .FindGameObjectByTag(tag);
        }

        // Scene内でタグの一致する物体の借用列を返す(tag: 検索するタグ)。
        [[nodiscard]] std::vector<GameObject*>
            FindObjectsWithTag(
                const std::string_view tag) const
        {
            return Owner().GetScene()
                .FindGameObjectsByTag(tag);
        }

        // 新しいGameObjectをSceneへ作る(name: 新しい物体名)。
        GameObject& CreateGameObject(std::string name)
        {
            return Owner().GetScene().CreateGameObject(
                std::move(name));
        }

        // PrefabをSceneへ生成する(prefabPath: Prefabファイル, parent: 任意の親物体)。
        GameObject& Instantiate(
            const std::filesystem::path& prefabPath,
            GameObject* parent = nullptr)
        {
            return Owner().GetScene().InstantiatePrefab(
                prefabPath,
                parent);
        }

        // 空パスや読み込みのstd::exceptionでは共有する空データを返す。
        // Sceneの経路からデータアセットを共有する(path: 対象ファイル)。
        [[nodiscard]] std::shared_ptr<const DataAsset>
            LoadDataAsset(
                const std::filesystem::path& path) const
        {
            return Owner().GetScene().LoadDataAsset(path);
        }

        // SceneへGameObjectの削除を要求する(gameObject: 自身も指定可能な削除対象)。
        bool Destroy(GameObject& gameObject)
        {
            return Owner().GetScene().DestroyGameObject(
                gameObject);
        }

        // 破棄時に解除するイベント購読を作る(eventName: イベント名, handler: 同期通知の受け手)。
        std::uint64_t On(
            const std::string_view eventName,
            std::function<void(const EventArgs&)> handler)
        {
            // 所有Sceneの借用EventBus
            auto& events = Owner().GetScene().Events();
            // 予約・購読・本体の操作番号
            const auto handle = events.Subscribe(
                eventName,
                std::move(handler));
            if (handle != 0)
            {
                m_eventSubscriptions.push_back(
                    { &events, handle });
            }
            return handle;
        }

        // 引数不要のイベント購読を作る(eventName: イベント名, handler: 通知時の処理)。
        std::uint64_t On(
            const std::string_view eventName,
            std::function<void()> handler)
        {
            // イベント引数を省略して呼ぶ(callback: 元のイベント処理)。
            return On(
                eventName,
                [callback = std::move(handler)](
                    const EventArgs&)
                {
                    callback();
                });
        }

        // 記録したイベント購読を解除する(handle: Onが返す購読番号)。
        void Off(const std::uint64_t handle)
        {
            // 所有スクリプトのイベント購読
            for (const auto& subscription :
                m_eventSubscriptions)
            {
                if (subscription.second == handle)
                {
                    subscription.first->Unsubscribe(
                        handle);
                    break;
                }
            }
            // 指定購読の記録を除去する(subscription: 確認対象の購読)。
            std::erase_if(
                m_eventSubscriptions,
                [handle](const auto& subscription)
                {
                    return subscription.second == handle;
                });
        }

        // 自身を送信元としてイベントを同期発行する(eventName: イベント名)。
        void Emit(const std::string_view eventName)
        {
            // 自分を送信元にする通知内容
            EventArgs eventArgs;
            eventArgs.sender = &Owner();
            Owner().GetScene().Events().Publish(
                eventName,
                eventArgs);
        }

        // 送信元が空なら自身を補って同期発行する(eventName: イベント名, eventArgs: 通知内容)。
        void Emit(
            const std::string_view eventName,
            EventArgs eventArgs)
        {
            if (eventArgs.sender == nullptr)
            {
                eventArgs.sender = &Owner();
            }
            Owner().GetScene().Events().Publish(
                eventName,
                eventArgs);
        }

        // 非同期Discordログインを開始しOnlineStateで進捗を取得する。
        [[nodiscard]] bool SignInWithDiscord() const
        {
            // 借用するオンラインサービス
            auto* online = ActiveOnlineServices();
            return online != nullptr
                && online->BeginDiscordSignIn();
        }

        // 有効なオンラインサービスがあればログインを中止する。
        void CancelDiscordSignIn() const noexcept
        {
            // 借用するオンラインサービス
            if (auto* online = ActiveOnlineServices())
            {
                online->CancelDiscordSignIn();
            }
        }

        // Discordとは独立した通信サービスを借用し、未設定なら空とする。
        [[nodiscard]] NetworkSession* Network() const noexcept { return ActiveNetworkSession(); }
        // 有効な通信サービスがホスト側か調べる。
        [[nodiscard]] bool IsNetworkHost() const noexcept
        {
            // 借用する通信サービス
            const auto* session = Network();
            return session && session->IsHost();
        }
        // 通信サービスがあればホストを開始する(name: ホスト表示名, address: 接続方式別の待受指定)。
        bool HostNetwork(std::string name = "Host", std::string address = "127.0.0.1") const
        {
            // 借用する通信サービス
            auto* session = Network();
            return session && session->Host(std::move(name), std::move(address));
        }
        // 通信サービスがあれば参加を開始する(address: 接続方式別の接続先, name: プレイヤー表示名)。
        bool JoinNetwork(std::string address, std::string name = "Player") const
        {
            // 借用する通信サービス
            auto* session = Network();
            return session && session->Join(std::move(address), std::move(name));
        }
        // 直接接続の参加を開始する(endpoint: ホスト接続先, accessKey: 招待用アクセスキー, name: プレイヤー表示名)。
        bool JoinDirectNetwork(std::string endpoint, std::string accessKey, std::string name = "Player") const
        {
            // 借用する通信サービス
            auto* session = Network();
            return session && session->JoinDirect(std::move(endpoint), std::move(accessKey), std::move(name));
        }
        // 部屋情報から参加を開始する(room: 接続先の部屋, name: プレイヤー表示名)。
        bool JoinNetworkRoom(const NetworkRoom& room, std::string name = "Player") const
        {
            // 借用する通信サービス
            auto* session = Network();
            return session && session->JoinRoom(room, std::move(name));
        }
        // 通信サービスへコマンドを送る(name: コマンド名, data: 通信する文字列データ)。
        bool SendNetworkCommand(std::string name, std::string data) const
        {
            // 借用する通信サービス
            auto* session = Network();
            return session && session->SendCommand(std::move(name), std::move(data));
        }
        // 通信サービスへ共有状態を設定する(data: 状態の文字列データ)。
        bool SetNetworkSessionState(std::string data) const
        {
            // 借用する通信サービス
            auto* session = Network();
            return session && session->SetSessionState(std::move(data));
        }
        // 通信を停止し同期Prefabの復元は更新後に委ねる。
        void StopNetwork() const
        {

            // 借用する通信サービス
            if (auto* session = Network()) session->Stop();
        }
        // 通信ブリッジから同期物体を生成する(prefabKey: 登録Prefabキー, transform: 初期姿勢, owner: 所有ピア番号)。
        [[nodiscard]] GameObject* NetworkSpawn(std::string_view prefabKey,
            const NetworkTransform& transform = {}, NetworkPeerId owner = 1) const
        {
            // 借用するScene通信ブリッジ
            auto* bridge = ActiveNetworkSceneBridge();
            return bridge ? bridge->Spawn(prefabKey, transform, owner) : nullptr;
        }
        // 通信ブリッジへ同期物体の削除を要求する(id: ネットワーク物体番号)。
        bool NetworkDespawn(NetworkObjectId id) const
        {
            // 借用するScene通信ブリッジ
            auto* bridge = ActiveNetworkSceneBridge();
            return bridge && bridge->Despawn(id);
        }
        // 通信ブリッジから同期物体を借用する(id: ネットワーク物体番号)。
        [[nodiscard]] GameObject* FindNetworkObject(NetworkObjectId id) const noexcept
        {
            // 借用するScene通信ブリッジ
            auto* bridge = ActiveNetworkSceneBridge();
            return bridge ? bridge->Find(id) : nullptr;
        }

        // 有効なオンラインサービスからログアウトする。
        void SignOutOnline() const
        {
            // 借用するオンラインサービス
            if (auto* online = ActiveOnlineServices())
            {
                online->SignOut();
            }
        }

        // 現在の認証状態を返し未設定ならUnconfiguredとする。
        [[nodiscard]] OnlineAccountState OnlineState() const noexcept
        {
            // 借用するオンラインサービス
            const auto* online = ActiveOnlineServices();
            return online != nullptr
                ? online->State()
                : OnlineAccountState::Unconfigured;
        }

        // 有効なオンラインサービスでログイン済みか調べる。
        [[nodiscard]] bool IsOnlineSignedIn() const noexcept
        {
            // 借用するオンラインサービス
            const auto* online = ActiveOnlineServices();
            return online != nullptr && online->IsSignedIn();
        }

        // プロフィールのIDを返しサービス未設定なら空とする。
        [[nodiscard]] std::string OnlinePlayerId() const
        {
            // 借用するオンラインサービス
            const auto* online = ActiveOnlineServices();
            return online != nullptr
                ? online->Player().playerId
                : std::string{};
        }

        // プロフィールの表示名を返しサービス未設定なら空とする。
        [[nodiscard]] std::string OnlinePlayerName() const
        {
            // 借用するオンラインサービス
            const auto* online = ActiveOnlineServices();
            return online != nullptr
                ? online->Player().displayName
                : std::string{};
        }

        // ログイン用URLを返しサービス未設定なら空とする。
        [[nodiscard]] std::string OnlineAuthorizationUrl() const
        {
            // 借用するオンラインサービス
            const auto* online = ActiveOnlineServices();
            return online != nullptr
                ? online->AuthorizationUrl()
                : std::string{};
        }

        // 認証エラーを返しサービス未設定なら空とする。
        [[nodiscard]] std::string OnlineError() const
        {
            // 借用するオンラインサービス
            const auto* online = ActiveOnlineServices();
            return online != nullptr
                ? online->LastError()
                : std::string{};
        }

        // 有効なオンラインサービスのクラウド同期状態を返す。
        [[nodiscard]] OnlineCloudSyncStatus CloudSyncStatus() const noexcept
        {
            // 借用するオンラインサービス
            const auto* online = ActiveOnlineServices();
            return online != nullptr
                ? online->CloudSyncStatus()
                : OnlineCloudSyncStatus{};
        }

        // 有効なオンラインサービスの同期競合一覧を複製する。
        [[nodiscard]] std::vector<OnlineCloudConflict>
            CloudConflicts() const
        {
            // 借用するオンラインサービス
            const auto* online = ActiveOnlineServices();
            return online != nullptr
                ? online->CloudConflicts()
                : std::vector<OnlineCloudConflict>{};
        }

        // 同期開始を要求しサービス未設定ならUnavailableとする。
        [[nodiscard]] OnlinePersistenceOperationResult
            RequestCloudSync() const noexcept
        {
            // 借用するオンラインサービス
            auto* online = ActiveOnlineServices();
            return online != nullptr
                ? online->RequestCloudSync()
                : OnlinePersistenceOperationResult::Unavailable;
        }

        // 競合の解決方法を要求する(conflictId: 現在の競合識別子, resolution: 採用する解決方法)。
        [[nodiscard]] OnlinePersistenceOperationResult
            ResolveCloudConflict(
            const std::string_view conflictId,
            const OnlineCloudConflictResolution resolution) const noexcept
        {
            // 借用するオンラインサービス
            auto* online = ActiveOnlineServices();
            return online != nullptr
                ? online->ResolveCloudConflict(conflictId, resolution)
                : OnlinePersistenceOperationResult::Unavailable;
        }

        // 有効なオンラインサービスの永続データ回復状態を返す。
        [[nodiscard]] OnlinePersistenceRecoveryStatus
            PersistenceRecoveryStatus() const noexcept
        {
            // 借用するオンラインサービス
            const auto* online = ActiveOnlineServices();
            return online != nullptr
                ? online->PersistenceRecoveryStatus()
                : OnlinePersistenceRecoveryStatus{};
        }

        // 指定版の回復候補を復元する(expectedRevision: 状態取得時の回復版番号)。
        [[nodiscard]] OnlinePersistenceOperationResult
            RestorePersistence(
            const std::uint64_t expectedRevision) const noexcept
        {
            // 借用するオンラインサービス
            auto* online = ActiveOnlineServices();
            return online != nullptr
                ? online->RestorePersistence(expectedRevision)
                : OnlinePersistenceOperationResult::Unavailable;
        }

        // 指定版の回復候補を破棄する(expectedRevision: 状態取得時の回復版番号)。
        [[nodiscard]] OnlinePersistenceOperationResult
            DiscardPersistence(
            const std::uint64_t expectedRevision) const noexcept
        {
            // 借用するオンラインサービス
            auto* online = ActiveOnlineServices();
            return online != nullptr
                ? online->DiscardPersistence(expectedRevision)
                : OnlinePersistenceOperationResult::Unavailable;
        }

        // ログインとは独立したプレイ状況表示を更新する(activity: 表示するプレイ状況)。
        [[nodiscard]] bool SetDiscordActivity(
            const DiscordActivity& activity) const noexcept
        {
            // 借用するオンラインサービス
            auto* online = ActiveOnlineServices();
            return online != nullptr
                && online->Presence().SetActivity(activity);
        }

        // プレイ状況の2行表示を更新する(details: 詳細行, state: 状態行)。
        [[nodiscard]] bool SetDiscordActivity(
            const std::string_view details,
            const std::string_view state) const noexcept
        {
            // 借用するオンラインサービス
            auto* online = ActiveOnlineServices();
            return online != nullptr
                && online->Presence().SetActivity(details, state);
        }

        // 有効なオンラインサービスのプレイ状況表示を解除する。
        void ClearDiscordActivity() const noexcept
        {
            // 借用するオンラインサービス
            if (auto* online = ActiveOnlineServices())
            {
                online->Presence().ClearActivity();
            }
        }

        // 有効なサービスでDiscord表示へ接続できるか調べる。
        [[nodiscard]] bool
            IsDiscordPresenceAvailable() const noexcept
        {
            // 借用するオンラインサービス
            const auto* online = ActiveOnlineServices();
            return online != nullptr
                && online->Presence().IsAvailable();
        }

        // プレイ状況表示の状態を返しサービス未設定ならDisabledとする。
        [[nodiscard]] DiscordPresenceState
            DiscordPresenceStatus() const noexcept
        {
            // 借用するオンラインサービス
            const auto* online = ActiveOnlineServices();
            return online != nullptr
                ? online->Presence().State()
                : DiscordPresenceState::Disabled;
        }

        // プレイ状況表示のエラーを返しサービス未設定なら空とする。
        [[nodiscard]] std::string DiscordPresenceError() const
        {
            // 借用するオンラインサービス
            const auto* online = ActiveOnlineServices();
            return online != nullptr
                ? online->Presence().LastError()
                : std::string{};
        }

        // Save系は毎回ファイルへ保存するため、状態確定時などの区切りで呼ぶ。
        // 有効な保存サービスへ整数を書き即時保存する(key: 項目名, value: 保存する整数)。
        void SaveInteger(
            const std::string_view key,
            const std::int64_t value) const
        {
            // 借用する設定保存サービス
            if (auto* prefs = ActivePlayerPrefs())
            {
                prefs->SetInteger(std::string(key), value);
                prefs->Save();
            }
        }

        // 保存した整数を返し取得不可なら既定値とする(key: 項目名, defaultValue: 不在時の値)。
        [[nodiscard]] std::int64_t LoadInteger(
            const std::string_view key,
            const std::int64_t defaultValue = 0) const
        {
            // 借用する設定保存サービス
            const auto* prefs = ActivePlayerPrefs();
            return prefs != nullptr
                ? prefs->GetInteger(key, defaultValue)
                : defaultValue;
        }

        // 有効な保存サービスへ実数を書き即時保存する(key: 項目名, value: 保存する実数)。
        void SaveNumber(
            const std::string_view key,
            const double value) const
        {
            // 借用する設定保存サービス
            if (auto* prefs = ActivePlayerPrefs())
            {
                prefs->SetNumber(std::string(key), value);
                prefs->Save();
            }
        }

        // 保存した実数を返し取得不可なら既定値とする(key: 項目名, defaultValue: 不在時の値)。
        [[nodiscard]] double LoadNumber(
            const std::string_view key,
            const double defaultValue = 0.0) const
        {
            // 借用する設定保存サービス
            const auto* prefs = ActivePlayerPrefs();
            return prefs != nullptr
                ? prefs->GetNumber(key, defaultValue)
                : defaultValue;
        }

        // 有効な保存サービスへ文字列を書き即時保存する(key: 項目名, value: 保存する文字列)。
        void SaveText(
            const std::string_view key,
            std::string value) const
        {
            // 借用する設定保存サービス
            if (auto* prefs = ActivePlayerPrefs())
            {
                prefs->SetString(
                    std::string(key),
                    std::move(value));
                prefs->Save();
            }
        }

        // 保存した文字列を返し取得不可なら既定値とする(key: 項目名, defaultValue: 不在時の文字列)。
        [[nodiscard]] std::string LoadText(
            const std::string_view key,
            std::string defaultValue = {}) const
        {
            // 借用する設定保存サービス
            const auto* prefs = ActivePlayerPrefs();
            return prefs != nullptr
                ? prefs->GetString(
                    key,
                    std::move(defaultValue))
                : defaultValue;
        }

        // 有効な保存サービスに項目が存在するか調べる(key: 項目名)。
        [[nodiscard]] bool HasSaved(
            const std::string_view key) const
        {
            // 借用する設定保存サービス
            const auto* prefs = ActivePlayerPrefs();
            return prefs != nullptr && prefs->HasKey(key);
        }

        // 有効な保存サービスから項目を消し即時保存する(key: 項目名)。
        void DeleteSaved(const std::string_view key) const
        {
            // 借用する設定保存サービス
            if (auto* prefs = ActivePlayerPrefs())
            {
                prefs->DeleteKey(key);
                prefs->Save();
            }
        }

        // 派生クラスで保存済みJSONを読み取る。
        virtual void LoadProperties(std::string_view)
        {
        }

        // 派生クラスの保存JSONを返し、既定では受領済みJSONを返す。
        [[nodiscard]] virtual std::string SaveProperties() const
        {
            return m_propertiesJson;
        }

    private:
        friend class NativeScriptComponent;
        template<typename TScript>
        friend struct Detail::ScriptBridge;

        struct TimerEntry final
        {
            // タイマー予約の識別番号
            std::uint64_t id{};
            // 実行までの残りゲーム秒数
            float remaining{};
            // 負値は単発の繰り返し秒数
            float interval{ -1.0f };
            // 実行するタイマー処理
            std::function<void()> callback;
        };

        // 更新中に追加したタイマーも同じ走査で処理する。
        // Update直前にタイマー予約を進める(deltaTime: 倍率適用後の経過秒数)。
        void TickTimers(const float deltaTime)
        {
            // 予約処理中の項目番号
            for (std::size_t index = 0;
                index < m_timers.size();
                ++index)
            {
                m_timers[index].remaining -= deltaTime;
                if (m_timers[index].remaining > 0.0f)
                {
                    continue;
                }

                // 予約変更前に複製した処理
                const auto callback =
                    m_timers[index].callback;
                if (m_timers[index].interval > 0.0f)
                {
                    m_timers[index].remaining +=
                        m_timers[index].interval;
                }
                else
                {
                    m_timers[index].id = 0;
                }
                if (callback)
                {
                    callback();
                }
            }
            // 実行済みの単発予約を取り除く(entry: 確認対象のタイマー)。
            std::erase_if(
                m_timers,
                [](const TimerEntry& entry)
                {
                    return entry.id == 0;
                });
        }

        // 所有物体と描画サービスを借用する(owner: 存続する所有物体, graphics: 存続する描画サービス)。
        void Attach(
            GameObject& owner,
            GraphicsDevice& graphics) noexcept
        {
            m_owner = &owner;
            m_graphics = &graphics;
        }

        // 空の設定は空JSONへ補い派生クラスへ渡す(propertiesJson: 初期設定JSON)。
        void LoadSerializedProperties(const char* propertiesJson)
        {
            m_propertiesJson = propertiesJson != nullptr
                ? propertiesJson
                : "{}";
            if (m_propertiesJson.empty())
            {
                m_propertiesJson = "{}";
            }
            LoadProperties(m_propertiesJson);
        }

        // 次の設定変更・保存処理まで有効なJSONを借用で返す。
        [[nodiscard]] const char* SerializeProperties()
        {
            // 派生クラスが返す保存JSON
            auto properties = SaveProperties();
            m_propertiesJson = properties.empty()
                ? "{}"
                : std::move(properties);
            return m_propertiesJson.c_str();
        }

        struct CoroutineEntry final
        {
            // 本体の継続識別番号
            std::uint64_t id{};
            // 所有するコルーチン本体
            Coroutine::Handle handle;
            // 再開中本体の遅延破棄
            bool stopped{};
        };

        // 継続番号から実行項目を検索する(id: コルーチンの継続番号)。
        [[nodiscard]] CoroutineEntry* FindCoroutineEntry(
            const std::uint64_t id) noexcept
        {
            // 走査または再検索する実行項目
            for (auto& entry : m_coroutines)
            {
                if (entry.id == id)
                {
                    return &entry;
                }
            }
            return nullptr;
        }

        // Update直前に待機が明けた本体を再開する(deltaTime: 倍率適用後の経過秒数)。
        void TickCoroutines(const float deltaTime)
        {
            // 予約処理中の項目番号
            for (std::size_t index = 0;
                index < m_coroutines.size();
                ++index)
            {
                // タイマーまたは継続の識別番号
                const auto id = m_coroutines[index].id;
                if (id == 0
                    || m_coroutines[index].stopped)
                {
                    continue;
                }
                // 予約・購読・本体の操作番号
                const auto handle =
                    m_coroutines[index].handle;
                // 再開条件と例外の借用状態
                auto& promise = handle.promise();
                if (promise.remainingSeconds > 0.0f)
                {
                    promise.remainingSeconds -=
                        deltaTime;
                    if (promise.remainingSeconds > 0.0f)
                    {
                        continue;
                    }
                    promise.remainingSeconds = 0.0f;
                }
                else if (promise.condition)
                {
                    if (!promise.condition())
                    {
                        continue;
                    }
                    promise.condition = nullptr;
                }

                m_resumingCoroutineId = id;
                handle.resume();
                m_resumingCoroutineId = 0;

                // 再開中の追加で再配置され得るため、番号から再検索する。
                // 走査または再検索する実行項目
                auto* entry = FindCoroutineEntry(id);
                if (entry == nullptr)
                {
                    continue;
                }
                if (entry->stopped || handle.done())
                {
                    // 本体が保持した再送出例外
                    const auto exception =
                        handle.promise().exception;
                    handle.destroy();
                    entry->id = 0;
                    entry->stopped = false;
                    if (exception)
                    {
                        std::rethrow_exception(
                            exception);
                    }
                }
            }
            // 破棄済みの本体を実行列から取り除く(entry: 確認対象の本体)。
            std::erase_if(
                m_coroutines,
                [](const CoroutineEntry& entry)
                {
                    return entry.id == 0;
                });
        }

        // 借用する所有GameObject
        GameObject* m_owner{};
        // 借用する描画サービス
        GraphicsDevice* m_graphics{};
        // 借用で返す保存JSONの所有
        std::string m_propertiesJson{ "{}" };
        // 所有するタイマー予約列
        std::vector<TimerEntry> m_timers;
        // 次のタイマー予約番号
        std::uint64_t m_nextTimerId{ 1 };
        // 所有するコルーチン実行列
        std::vector<CoroutineEntry> m_coroutines;
        // 次のコルーチン継続番号
        std::uint64_t m_nextCoroutineId{ 1 };
        // 現在再開中の本体の番号
        std::uint64_t m_resumingCoroutineId{};

        // 破棄時に解除する借用購読
        std::vector<std::pair<EventBus*, std::uint64_t>>
            m_eventSubscriptions;
    };

    namespace GameModuleScripts
    {
        // DLLが所有するスクリプト型を登録する(descriptor: 型名と具体型ブリッジ)。
        void Register(NativeScriptTypeDescriptor descriptor);

        // DLL内の静的初期化で登録したスクリプト型一覧を参照する。
        [[nodiscard]] const std::vector<NativeScriptTypeDescriptor>&
            RegisteredScripts() noexcept;

        class AutoRegister final
        {
        public:
            // 静的初期化でスクリプト型を登録する(descriptor: DLLが所有する型記述)。
            explicit AutoRegister(NativeScriptTypeDescriptor descriptor);
        };
    }

    // データアセットの型宣言（LAMAPON_DATA_ASSET）の置き場です。
    // Scriptと同じく、DLL側の静的初期化でここへ集まります。
    namespace GameModuleDataAssets
    {
        // DLLが所有するデータ型の定義を登録する(descriptor: 型名と入力スキーマ)。
        void Register(NativeDataAssetTypeDescriptor descriptor);

        // DLL内の静的初期化で登録したデータ型一覧を参照する。
        [[nodiscard]] const std::vector<
            NativeDataAssetTypeDescriptor>&
            RegisteredDataAssets() noexcept;

        class AutoRegister final
        {
        public:
            // 静的初期化でデータ型を登録する(descriptor: DLLが所有する型定義)。
            explicit AutoRegister(
                NativeDataAssetTypeDescriptor descriptor);
        };
    }

    namespace Detail
    {
        // 入力項目のない既定スキーマ
        inline constexpr char EmptyScriptPropertiesSchema[] =
            R"({"fields":[]})";

        template<typename TScript>
        struct ScriptBridge final
        {
            static_assert(
                std::is_base_of_v<Script, TScript>,
                "LAMAPON_SCRIPT type must inherit from LamaPon::Script.");
            static_assert(
                std::is_default_constructible_v<TScript>,
                "LAMAPON_SCRIPT type must have a default constructor.");

            // 初期設定を読み込んでAwakeを呼ぶ(owner: 必須の所有物体, graphics: 必須の描画サービス, propertiesJson: 初期設定JSON)。
            static void* Create(
                GameObject* owner,
                GraphicsDevice* graphics,
                const char* propertiesJson)
            {
                if (owner == nullptr || graphics == nullptr)
                {
                    return nullptr;
                }

                // DLL側の具体型スクリプト
                auto script = std::make_unique<TScript>();
                script->Attach(*owner, *graphics);
                script->LoadSerializedProperties(propertiesJson);
                script->Awake();
                return script.release();
            }

            // DLL側の具体型で終了通知と破棄を行う(instance: 同じ型の生成済みインスタンス)。
            static void Destroy(void* instance)
            {
                // 終了通知が例外でも具体型の所有を解放し、購読とコルーチンを破棄する。
                // DLL側の具体型スクリプト
                std::unique_ptr<TScript> script{
                    static_cast<TScript*>(instance) };
                if (script)
                {
                    script->OnDestroy();
                }
            }

            // 具体型でポインターを調整して基底へ変換する(instance: 同じ型の生成済みインスタンス)。
            static Script* AsScript(void* instance)
            {
                return static_cast<TScript*>(instance);
            }

            // DLL側の具体型で開始処理を呼ぶ(instance: 開始対象)。
            static void Start(void* instance)
            {
                static_cast<TScript*>(instance)->Start();
            }

            // DLL側の具体型へ有効状態の遷移を伝える(instance: 対象スクリプト, active: 実効アクティブ状態)。
            static void SetActive(
                void* instance,
                const bool active)
            {
                // DLL側の具体型スクリプト
                auto* script = static_cast<TScript*>(instance);
                if (active)
                {
                    script->OnEnable();
                }
                else
                {
                    script->OnDisable();
                }
            }

            // タイマー・コルーチンを進めて更新する(instance: 対象スクリプト, deltaTime: 倍率適用後の経過秒数)。
            static void Update(void* instance, const float deltaTime)
            {
                // DLL側の具体型スクリプト
                auto* script = static_cast<TScript*>(instance);
                script->TickTimers(deltaTime);
                script->TickCoroutines(deltaTime);
                script->Update(deltaTime);
            }

            // DLL側の具体型で物理計算後の更新を呼ぶ(instance: 更新対象, deltaTime: 今回の経過秒数)。
            static void LateUpdate(
                void* instance,
                const float deltaTime)
            {
                static_cast<TScript*>(instance)->LateUpdate(
                    deltaTime);
            }

            // DLL側の具体型で固定更新を呼ぶ(instance: 更新対象, fixedDeltaTime: 固定刻みの秒数)。
            static void FixedUpdate(
                void* instance,
                const float fixedDeltaTime)
            {
                static_cast<TScript*>(instance)->FixedUpdate(
                    fixedDeltaTime);
            }

            // 空でない接触開始を具体型へ渡す(instance: 対象スクリプト, event: 通知中に借用する接触)。
            static void CollisionEnter(
                void* instance,
                const CollisionEvent* event)
            {
                if (event != nullptr)
                {
                    static_cast<TScript*>(instance)->OnCollisionEnter(
                        *event);
                }
            }

            // 空でない接触継続を具体型へ渡す(instance: 対象スクリプト, event: 通知中に借用する接触)。
            static void CollisionStay(
                void* instance,
                const CollisionEvent* event)
            {
                if (event != nullptr)
                {
                    static_cast<TScript*>(instance)->OnCollisionStay(
                        *event);
                }
            }

            // 空でない接触終了を具体型へ渡す(instance: 対象スクリプト, event: 通知中に借用する接触)。
            static void CollisionExit(
                void* instance,
                const CollisionEvent* event)
            {
                if (event != nullptr)
                {
                    static_cast<TScript*>(instance)->OnCollisionExit(
                        *event);
                }
            }

            // 空でないトリガー開始を具体型へ渡す(instance: 対象スクリプト, event: 通知中に借用する接触)。
            static void TriggerEnter(
                void* instance,
                const CollisionEvent* event)
            {
                if (event != nullptr)
                {
                    static_cast<TScript*>(instance)->OnTriggerEnter(
                        *event);
                }
            }

            // 空でないトリガー継続を具体型へ渡す(instance: 対象スクリプト, event: 通知中に借用する接触)。
            static void TriggerStay(
                void* instance,
                const CollisionEvent* event)
            {
                if (event != nullptr)
                {
                    static_cast<TScript*>(instance)->OnTriggerStay(
                        *event);
                }
            }

            // 空でないトリガー終了を具体型へ渡す(instance: 対象スクリプト, event: 通知中に借用する接触)。
            static void TriggerExit(
                void* instance,
                const CollisionEvent* event)
            {
                if (event != nullptr)
                {
                    static_cast<TScript*>(instance)->OnTriggerExit(
                        *event);
                }
            }

            // 次の設定変更・保存まで有効なJSONを返す(instance: 保存対象のスクリプト)。
            static const char* Serialize(void* instance)
            {
                return static_cast<TScript*>(instance)
                    ->SerializeProperties();
            }
        };

        // DLLが所有する型名と関数を登録記述へまとめる(typeName: 登録型名, displayName: 型表示名, propertiesSchemaJson: 型付き入力スキーマ)。
        template<typename TScript>
        [[nodiscard]] NativeScriptTypeDescriptor MakeScriptDescriptor(
            const char* typeName,
            const char* displayName,
            const char* propertiesSchemaJson =
                EmptyScriptPropertiesSchema)
        {
            using Bridge = ScriptBridge<TScript>;
            return NativeScriptTypeDescriptor{
                typeName,
                displayName,
                &Bridge::Create,
                &Bridge::Destroy,
                &Bridge::Update,
                &Bridge::CollisionEnter,
                &Bridge::CollisionStay,
                &Bridge::CollisionExit,
                &Bridge::Serialize,
                &Bridge::FixedUpdate,
                propertiesSchemaJson,
                &Bridge::Start,
                &Bridge::LateUpdate,
                &Bridge::SetActive,
                &Bridge::TriggerEnter,
                &Bridge::TriggerStay,
                &Bridge::TriggerExit,
                &Bridge::AsScript
            };
        }
    }
}

#define LAMAPON_DETAIL_JOIN_INNER(a, b) a##b
#define LAMAPON_DETAIL_JOIN(a, b) LAMAPON_DETAIL_JOIN_INNER(a, b)

#define LAMAPON_DETAIL_REGISTER_SCRIPT(type, typeName, displayName, schema, id) \
    namespace \
    { \
        const LamaPon::GameModuleScripts::AutoRegister \
            LAMAPON_DETAIL_JOIN(LamaPonScriptRegistration_, id){ \
                LamaPon::Detail::MakeScriptDescriptor<type>( \
                    typeName, \
                    displayName, \
                    schema) \
            }; \
    }

#define LAMAPON_SCRIPT(type) \
    LAMAPON_DETAIL_REGISTER_SCRIPT( \
        type, \
        "Game." #type, \
        #type, \
        LamaPon::Detail::EmptyScriptPropertiesSchema, \
        __COUNTER__)

#define LAMAPON_SCRIPT_NAMED(type, typeName, displayName) \
    LAMAPON_DETAIL_REGISTER_SCRIPT( \
        type, \
        typeName, \
        displayName, \
        LamaPon::Detail::EmptyScriptPropertiesSchema, \
        __COUNTER__)

#define LAMAPON_SCRIPT_WITH_SCHEMA(type, typeName, displayName, schema) \
    LAMAPON_DETAIL_REGISTER_SCRIPT( \
        type, \
        typeName, \
        displayName, \
        schema, \
        __COUNTER__)

#define LAMAPON_DETAIL_REGISTER_DATA_ASSET(typeName, displayName, schema, id) \
    namespace \
    { \
        const LamaPon::GameModuleDataAssets::AutoRegister \
            LAMAPON_DETAIL_JOIN(LamaPonDataAssetRegistration_, id){ \
                LamaPon::NativeDataAssetTypeDescriptor{ \
                    typeName, \
                    displayName, \
                    schema \
                } \
            }; \
    }

// データアセット（UnityのScriptableObject相当）の型を宣言します。
// エディターのアセットウィンドウに「新規データアセット」として
// 現れ、選ぶと<名前>.asset.jsonが作られます。値はInspectorで
// 編集し、ゲームからはLoadDataAsset("...")で読みます。
//
//   constexpr char CardSchema[] = R"({
//       "fields": [
//           { "name": "cost", "displayName": "コスト",
//             "type": "int", "default": 1, "min": 0, "max": 10 }
//       ]
//   })";
//   LAMAPON_DATA_ASSET("Game.CardData", "カードデータ", CardSchema)
#define LAMAPON_DATA_ASSET(typeName, displayName, schema) \
    LAMAPON_DETAIL_REGISTER_DATA_ASSET( \
        typeName, \
        displayName, \
        schema, \
        __COUNTER__)
