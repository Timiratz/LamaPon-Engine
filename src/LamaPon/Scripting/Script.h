#pragma once

#include "LamaPon/Core/PlayerPrefs.h"
#include "LamaPon/Scripting/Coroutine.h"
#include "LamaPon/Scripting/GameModule.h"
// 初心者向けショートカット（GetComponent/Find/Instantiate等）を
// ヘッダー内で定義するため、GameObjectとSceneの完全型が必要です。
// Script.hはゲームモジュール側でのみコンパイルされます。
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
    class GraphicsDevice;
    struct CollisionEvent;

    namespace Detail
    {
        template<typename TScript>
        struct ScriptBridge;
    }

    class Script
    {
    public:
        virtual ~Script()
        {
            StopAllCoroutines();
            // イベント購読を自動解除します。
            for (const auto& subscription :
                m_eventSubscriptions)
            {
                subscription.first->Unsubscribe(
                    subscription.second);
            }
        }

        // インスタンス生成とシリアライズ済みプロパティの読み込み
        // 直後に一度だけ呼ばれます。
        virtual void Awake()
        {
        }

        // 最初のUpdateの直前に一度だけ呼ばれます。
        virtual void Start()
        {
        }

        // スクリプトが実効的にアクティブになった時に呼ばれます
        // （自身のenabled、Component、祖先GameObjectがすべて有効）。
        virtual void OnEnable()
        {
        }

        // スクリプトが実効的に非アクティブになった時に呼ばれます。
        virtual void OnDisable()
        {
        }

        // インスタンス破棄の直前に呼ばれます。
        virtual void OnDestroy()
        {
        }

        virtual void Update(float)
        {
        }

        // 毎フレーム、全ComponentのUpdate後に呼ばれます。
        virtual void LateUpdate(float)
        {
        }

        virtual void FixedUpdate(float)
        {
        }

        virtual void OnCollisionEnter(const CollisionEvent&)
        {
        }

        virtual void OnCollisionStay(const CollisionEvent&)
        {
        }

        virtual void OnCollisionExit(const CollisionEvent&)
        {
        }

        // トリガー接触（isTriggerコライダー）はCollisionコール
        // バックの代わりにこちらへ届きます。
        virtual void OnTriggerEnter(const CollisionEvent&)
        {
        }

        virtual void OnTriggerStay(const CollisionEvent&)
        {
        }

        virtual void OnTriggerExit(const CollisionEvent&)
        {
        }

    protected:
        Script() = default;

        // delaySeconds後に一度だけcallbackを実行します。
        // 経過はUpdateと同じスケール済み時間で進みます
        // （timeScale=0中は停止）。戻り値はCancelInvoke用。
        std::uint64_t Invoke(
            const float delaySeconds,
            std::function<void()> callback)
        {
            const auto id = m_nextTimerId++;
            m_timers.push_back({
                id,
                std::max(delaySeconds, 0.0f),
                -1.0f,
                std::move(callback) });
            return id;
        }

        // delaySeconds後に開始し、以後intervalSecondsごとに
        // callbackを繰り返します。
        std::uint64_t InvokeRepeating(
            const float delaySeconds,
            const float intervalSeconds,
            std::function<void()> callback)
        {
            const auto id = m_nextTimerId++;
            m_timers.push_back({
                id,
                std::max(delaySeconds, 0.0f),
                std::max(intervalSeconds, 0.001f),
                std::move(callback) });
            return id;
        }

        void CancelInvoke(const std::uint64_t handle)
        {
            std::erase_if(
                m_timers,
                [handle](const TimerEntry& entry)
                {
                    return entry.id == handle;
                });
        }

        void CancelAllInvokes()
        {
            m_timers.clear();
        }

        // ---- コルーチン ----
        // Coroutineを返すメンバー関数をco_awaitで書き、
        // StartCoroutineで開始します。最初の
        // co_awaitまでは即座に実行されます）。
        //
        //   LamaPon::Coroutine Intro()
        //   {
        //       co_await LamaPon::WaitForSeconds{ 1.0f };
        //       Find("ドア")->SetEnabled(false);
        //   }
        //   void Start() override { StartCoroutine(Intro()); }
        std::uint64_t StartCoroutine(Coroutine coroutine)
        {
            const auto handle = coroutine.Release();
            if (!handle)
            {
                return 0;
            }
            handle.resume();
            if (handle.done())
            {
                const auto exception =
                    handle.promise().exception;
                handle.destroy();
                if (exception)
                {
                    std::rethrow_exception(exception);
                }
                return 0;
            }
            const auto id = m_nextCoroutineId++;
            m_coroutines.push_back(
                { id, handle, false });
            return id;
        }

        // StartCoroutineが返したハンドルで停止します。
        void StopCoroutine(const std::uint64_t handle)
        {
            for (auto& entry : m_coroutines)
            {
                if (entry.id != handle || entry.id == 0)
                {
                    continue;
                }
                // 自分自身の実行中に呼ばれた場合は、resumeが
                // 戻った後にTickCoroutinesが破棄します。
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

        void StopAllCoroutines()
        {
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
            std::erase_if(
                m_coroutines,
                [](const CoroutineEntry& entry)
                {
                    return entry.id == 0;
                });
        }

        [[nodiscard]] GameObject& Owner() noexcept
        {
            assert(m_owner != nullptr);
            return *m_owner;
        }

        [[nodiscard]] const GameObject& Owner() const noexcept
        {
            assert(m_owner != nullptr);
            return *m_owner;
        }

        [[nodiscard]] GraphicsDevice& Graphics() noexcept
        {
            assert(m_graphics != nullptr);
            return *m_graphics;
        }

        [[nodiscard]] const GraphicsDevice& Graphics() const noexcept
        {
            assert(m_graphics != nullptr);
            return *m_graphics;
        }

        // ---- 初心者向けショートカット ----
        // Owner()やGetScene()を書かずに主要な操作を直接呼べます。

        // 自分のGameObjectからコンポーネントを取得します。
        template<typename T>
        [[nodiscard]] T* GetComponent() noexcept
        {
            return Owner().GetComponent<T>();
        }

        template<typename T>
        [[nodiscard]] const T* GetComponent() const noexcept
        {
            return Owner().GetComponent<T>();
        }

        // 同じGameObject上のスクリプトを型で引きます。
        // 自作インターフェースでも引けます。
        //   if (auto* target = GetScript<IDamageable>()) { ... }
        template<typename T>
        [[nodiscard]] T* GetScript() const noexcept
        {
            return Owner().GetScript<T>();
        }

        template<typename T>
        [[nodiscard]] T* GetScriptInChildren(
            const bool includeInactive = false) const noexcept
        {
            return Owner().GetScriptInChildren<T>(
                includeInactive);
        }

        template<typename T>
        [[nodiscard]] T* GetComponentInChildren(
            const bool includeInactive = false) noexcept
        {
            return Owner().GetComponentInChildren<T>(
                includeInactive);
        }

        // 自分から親方向へ遡ってコンポーネントを探します。
        template<typename T>
        [[nodiscard]] T* GetComponentInParent(
            const bool includeInactive = false) noexcept
        {
            return Owner().GetComponentInParent<T>(
                includeInactive);
        }

        // 自分のGameObjectへコンポーネントを追加します。
        template<typename T, typename... Args>
        T& AddComponent(Args&&... args)
        {
            return Owner().AddComponent<T>(
                std::forward<Args>(args)...);
        }

        // 自分のTransform（位置・回転・拡縮）です。
        [[nodiscard]] Transform& GetTransform() noexcept
        {
            return Owner().GetTransform();
        }

        [[nodiscard]] const Transform&
            GetTransform() const noexcept
        {
            return Owner().GetTransform();
        }

        // 自分が属しているSceneです（constメンバー関数からも
        // 呼べるよう、GameObject::GetScene()と同じくconstです）。
        [[nodiscard]] Scene& GetScene() const noexcept
        {
            return Owner().GetScene();
        }

        // シーン全体から名前でGameObjectを探します。
        [[nodiscard]] GameObject* Find(
            const std::string_view name) const noexcept
        {
            return Owner().GetScene()
                .FindGameObjectByName(name);
        }

        // シーン全体からタグでGameObjectを探します。
        [[nodiscard]] GameObject* FindWithTag(
            const std::string_view tag) const noexcept
        {
            return Owner().GetScene()
                .FindGameObjectByTag(tag);
        }

        [[nodiscard]] std::vector<GameObject*>
            FindObjectsWithTag(
                const std::string_view tag) const
        {
            return Owner().GetScene()
                .FindGameObjectsByTag(tag);
        }

        // 新しいGameObjectをシーンへ作成します。
        GameObject& CreateGameObject(std::string name)
        {
            return Owner().GetScene().CreateGameObject(
                std::move(name));
        }

        // Prefab（"prefabs/enemy.prefab.json"等）を生成します。
        GameObject& Instantiate(
            const std::filesystem::path& prefabPath,
            GameObject* parent = nullptr)
        {
            return Owner().GetScene().InstantiatePrefab(
                prefabPath,
                parent);
        }

        // データアセット（"data/cards/fire.asset.json"等）を
        // 読み込みます。UnityのScriptableObjectに相当する、
        // GameObjectへぶら下がらないデータの入れ物です。
        //
        //   const auto card = LoadDataAsset(m_cardPath);
        //   const int cost = card->GetInt("cost");
        //
        // 読めなかった場合も空のDataAssetが返るため、nullptr判定は
        // 不要です（値は既定値のまま返ります）。
        [[nodiscard]] std::shared_ptr<const DataAsset>
            LoadDataAsset(
                const std::filesystem::path& path) const
        {
            return Owner().GetScene().LoadDataAsset(path);
        }

        // GameObjectを削除します（自分のGameObjectも可）。
        bool Destroy(GameObject& gameObject)
        {
            return Owner().GetScene().DestroyGameObject(
                gameObject);
        }

        // ---- イベント（名前で連携するシグナル） ----
        // 「敵が倒された」のような出来事を名前で購読・発行でき、
        // コンポーネント同士が直接参照せずに連携できます。
        // UIButtonの「クリックイベント名」もここへ届きます。
        //
        //   void Start() override
        //   {
        //       On("EnemyDied",
        //           [this](const LamaPon::EventArgs& args)
        //           {
        //               m_score += (int)args.number;
        //           });
        //   }
        //   // 発行側: Emit("EnemyDied", { nullptr, 100.0f });
        //
        // 購読はScriptの破棄時に自動解除されます。
        std::uint64_t On(
            const std::string_view eventName,
            std::function<void(const EventArgs&)> handler)
        {
            auto& events = Owner().GetScene().Events();
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

        // 引数を使わない場合の省略形。
        std::uint64_t On(
            const std::string_view eventName,
            std::function<void()> handler)
        {
            return On(
                eventName,
                [callback = std::move(handler)](
                    const EventArgs&)
                {
                    callback();
                });
        }

        void Off(const std::uint64_t handle)
        {
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
            std::erase_if(
                m_eventSubscriptions,
                [handle](const auto& subscription)
                {
                    return subscription.second == handle;
                });
        }

        // イベントを発行します（senderは自動で自分になります）。
        void Emit(const std::string_view eventName)
        {
            EventArgs eventArgs;
            eventArgs.sender = &Owner();
            Owner().GetScene().Events().Publish(
                eventName,
                eventArgs);
        }

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

        // ---- 設定値の保存（アプリを終了しても残ります）----------
        //
        // ハイスコアや「音量」「クリアしたステージ」のような小さな値を
        // 保存します。保存先は
        // `%LOCALAPPDATA%/LamaPon/<ゲーム名>/PlayerPrefs.json`で、
        // エディターの「セーブデータ」タブから中身を確認できます。
        //
        // ⚠️ **書くたびにファイルへ保存します。毎フレーム呼ばないで
        // ください**（ゲームオーバー時など、区切りで呼ぶ想定です）。
        // 大きな進行状況はSaveDataStore（JSONスロット）向きです。
        void SaveInteger(
            const std::string_view key,
            const std::int64_t value) const
        {
            if (auto* prefs = ActivePlayerPrefs())
            {
                prefs->SetInteger(std::string(key), value);
                prefs->Save();
            }
        }

        [[nodiscard]] std::int64_t LoadInteger(
            const std::string_view key,
            const std::int64_t defaultValue = 0) const
        {
            const auto* prefs = ActivePlayerPrefs();
            return prefs != nullptr
                ? prefs->GetInteger(key, defaultValue)
                : defaultValue;
        }

        void SaveNumber(
            const std::string_view key,
            const double value) const
        {
            if (auto* prefs = ActivePlayerPrefs())
            {
                prefs->SetNumber(std::string(key), value);
                prefs->Save();
            }
        }

        [[nodiscard]] double LoadNumber(
            const std::string_view key,
            const double defaultValue = 0.0) const
        {
            const auto* prefs = ActivePlayerPrefs();
            return prefs != nullptr
                ? prefs->GetNumber(key, defaultValue)
                : defaultValue;
        }

        void SaveText(
            const std::string_view key,
            std::string value) const
        {
            if (auto* prefs = ActivePlayerPrefs())
            {
                prefs->SetString(
                    std::string(key),
                    std::move(value));
                prefs->Save();
            }
        }

        [[nodiscard]] std::string LoadText(
            const std::string_view key,
            std::string defaultValue = {}) const
        {
            const auto* prefs = ActivePlayerPrefs();
            return prefs != nullptr
                ? prefs->GetString(
                    key,
                    std::move(defaultValue))
                : defaultValue;
        }

        // 保存済みかどうか。「初回起動か」の判定などに使えます。
        [[nodiscard]] bool HasSaved(
            const std::string_view key) const
        {
            const auto* prefs = ActivePlayerPrefs();
            return prefs != nullptr && prefs->HasKey(key);
        }

        void DeleteSaved(const std::string_view key) const
        {
            if (auto* prefs = ActivePlayerPrefs())
            {
                prefs->DeleteKey(key);
                prefs->Save();
            }
        }

        virtual void LoadProperties(std::string_view)
        {
        }

        [[nodiscard]] virtual std::string SaveProperties() const
        {
            return m_propertiesJson;
        }

    private:
        template<typename TScript>
        friend struct Detail::ScriptBridge;

        struct TimerEntry final
        {
            std::uint64_t id{};
            float remaining{};
            // 負なら1回のみ、正なら繰り返し間隔。
            float interval{ -1.0f };
            std::function<void()> callback;
        };

        // Updateの直前にブリッジから呼ばれます。コールバック内での
        // Invoke追加に耐えるよう添字ループで処理します。
        void TickTimers(const float deltaTime)
        {
            for (std::size_t index = 0;
                index < m_timers.size();
                ++index)
            {
                m_timers[index].remaining -= deltaTime;
                if (m_timers[index].remaining > 0.0f)
                {
                    continue;
                }
                // コールバックがm_timersを変更しても安全なよう
                // 呼び出し前にコピーします。
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
            std::erase_if(
                m_timers,
                [](const TimerEntry& entry)
                {
                    return entry.id == 0;
                });
        }

        void Attach(
            GameObject& owner,
            GraphicsDevice& graphics) noexcept
        {
            m_owner = &owner;
            m_graphics = &graphics;
        }

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

        [[nodiscard]] const char* SerializeProperties()
        {
            auto properties = SaveProperties();
            m_propertiesJson = properties.empty()
                ? "{}"
                : std::move(properties);
            return m_propertiesJson.c_str();
        }

        struct CoroutineEntry final
        {
            std::uint64_t id{};
            Coroutine::Handle handle;
            // 自分自身のresume中にStopされたときの遅延破棄フラグ。
            bool stopped{};
        };

        [[nodiscard]] CoroutineEntry* FindCoroutineEntry(
            const std::uint64_t id) noexcept
        {
            for (auto& entry : m_coroutines)
            {
                if (entry.id == id)
                {
                    return &entry;
                }
            }
            return nullptr;
        }

        // Updateの直前にブリッジから呼ばれます。待機が明けた
        // コルーチンを再開します。resume中のStartCoroutineによる
        // vector再割り当てに耐えるため、添字ループ＋ID再検索で
        // 処理します。
        void TickCoroutines(const float deltaTime)
        {
            for (std::size_t index = 0;
                index < m_coroutines.size();
                ++index)
            {
                const auto id = m_coroutines[index].id;
                if (id == 0
                    || m_coroutines[index].stopped)
                {
                    continue;
                }
                const auto handle =
                    m_coroutines[index].handle;
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

                auto* entry = FindCoroutineEntry(id);
                if (entry == nullptr)
                {
                    continue;
                }
                if (entry->stopped || handle.done())
                {
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
            std::erase_if(
                m_coroutines,
                [](const CoroutineEntry& entry)
                {
                    return entry.id == 0;
                });
        }

        GameObject* m_owner{};
        GraphicsDevice* m_graphics{};
        std::string m_propertiesJson{ "{}" };
        std::vector<TimerEntry> m_timers;
        std::uint64_t m_nextTimerId{ 1 };
        std::vector<CoroutineEntry> m_coroutines;
        std::uint64_t m_nextCoroutineId{ 1 };
        std::uint64_t m_resumingCoroutineId{};
        // On()で登録したイベント購読（破棄時に自動解除）。
        std::vector<std::pair<EventBus*, std::uint64_t>>
            m_eventSubscriptions;
    };

    namespace GameModuleScripts
    {
        void Register(NativeScriptTypeDescriptor descriptor);

        [[nodiscard]] const std::vector<NativeScriptTypeDescriptor>&
            RegisteredScripts() noexcept;

        class AutoRegister final
        {
        public:
            explicit AutoRegister(NativeScriptTypeDescriptor descriptor);
        };
    }

    // データアセットの型宣言（LAMAPON_DATA_ASSET）の置き場です。
    // Scriptと同じく、DLL側の静的初期化でここへ集まります。
    namespace GameModuleDataAssets
    {
        void Register(NativeDataAssetTypeDescriptor descriptor);

        [[nodiscard]] const std::vector<
            NativeDataAssetTypeDescriptor>&
            RegisteredDataAssets() noexcept;

        class AutoRegister final
        {
        public:
            explicit AutoRegister(
                NativeDataAssetTypeDescriptor descriptor);
        };
    }

    namespace Detail
    {
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

            static void* Create(
                GameObject* owner,
                GraphicsDevice* graphics,
                const char* propertiesJson)
            {
                if (owner == nullptr || graphics == nullptr)
                {
                    return nullptr;
                }

                auto script = std::make_unique<TScript>();
                script->Attach(*owner, *graphics);
                script->LoadSerializedProperties(propertiesJson);
                script->Awake();
                return script.release();
            }

            static void Destroy(void* instance)
            {
                auto* script = static_cast<TScript*>(instance);
                script->OnDestroy();
                delete script;
            }

            // instanceをScript*へ上位変換します。ここでは具体型
            // TScriptが分かるので、多重継承（Scriptと自作
            // インターフェースの併用）でも必要なポインタ調整が
            // 正しく入ります。呼び出し側でvoid*から直接
            // static_cast<Script*>すると調整が入らず、Scriptを
            // 先頭以外に継承した型で静かに壊れます。
            static Script* AsScript(void* instance)
            {
                return static_cast<TScript*>(instance);
            }

            static void Start(void* instance)
            {
                static_cast<TScript*>(instance)->Start();
            }

            static void SetActive(
                void* instance,
                const bool active)
            {
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

            static void Update(void* instance, const float deltaTime)
            {
                auto* script = static_cast<TScript*>(instance);
                script->TickTimers(deltaTime);
                script->TickCoroutines(deltaTime);
                script->Update(deltaTime);
            }

            static void LateUpdate(
                void* instance,
                const float deltaTime)
            {
                static_cast<TScript*>(instance)->LateUpdate(
                    deltaTime);
            }

            static void FixedUpdate(
                void* instance,
                const float fixedDeltaTime)
            {
                static_cast<TScript*>(instance)->FixedUpdate(
                    fixedDeltaTime);
            }

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

            static const char* Serialize(void* instance)
            {
                return static_cast<TScript*>(instance)
                    ->SerializeProperties();
            }
        };

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
// 現れ、選ぶと`<名前>.asset.json`が作られます。値はInspectorで
// 編集し、ゲームからは`LoadDataAsset("...")`で読みます。
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
