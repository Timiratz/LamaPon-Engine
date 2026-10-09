#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "LamaPon/Scene/EventBus.h"

namespace DirectX
{
    // 円周率定数
    constexpr float XM_PI = 3.14159265358979323846f;

    struct XMFLOAT2 final
    {
        // X座標
        float x{};
        // Y座標
        float y{};
    };

    struct XMFLOAT3 final
    {
        // X座標
        float x{};
        // Y座標
        float y{};
        // Z座標
        float z{};
    };

    struct XMFLOAT4 final
    {
        // X成分
        float x{};
        // Y成分
        float y{};
        // Z成分
        float z{};
        // W成分
        float w{};
    };

    struct XMFLOAT4X4 final
    {
        // _11:1行1列、_12:1行2列、_13:1行3列、_14:1行4列
        float _11{ 1.0f }, _12{}, _13{}, _14{};
        // _21:2行1列、_22:2行2列、_23:2行3列、_24:2行4列
        float _21{}, _22{ 1.0f }, _23{}, _24{};
        // _31:3行1列、_32:3行2列、_33:3行3列、_34:3行4列
        float _31{}, _32{}, _33{ 1.0f }, _34{};
        // _41:4行1列、_42:4行2列、_43:4行3列、_44:4行4列
        float _41{}, _42{}, _43{}, _44{ 1.0f };
    };

    using XMMATRIX = XMFLOAT4X4;

    // 行列の値を4x4構造体へ格納します。
    // XMStoreFloat4x4(destination: 格納先, source: コピー元行列)
    inline void XMStoreFloat4x4(
        XMFLOAT4X4* destination,
        const XMMATRIX& source) noexcept
    {
        // 格納先が有効な場合
        if (destination != nullptr)
        {
            *destination = source;
        }
    }
}

namespace LamaPon::Native { class NativeInput; }

namespace LamaPon::Web
{
    class Renderer3D;
    class WebAudioRuntime;
#if defined(LAMAPON_NATIVE_RUNTIME)
    using WebInput = Native::NativeInput;
#else
    class WebInput;
#endif
}

namespace LamaPon
{
    class Component;
    class GameObject;
    class GraphicsDevice;
    class NativeScriptComponent;
    class Scene;
    class Script;

    class Logger final
    {
    public:
        // 共有ロガーを返します。
        [[nodiscard]] static Logger& Instance() noexcept
        {
            // 唯一のロガー実体
            static Logger logger;
            return logger;
        }

        // 情報レベルのメッセージを記録します。
        // Info(message: ログ本文)
        void Info(std::string_view message) const noexcept;
        // 警告レベルのメッセージを記録します。
        // Warning(message: ログ本文)
        void Warning(std::string_view message) const noexcept;
        // エラーレベルのメッセージを記録します。
        // Error(message: ログ本文)
        void Error(std::string_view message) const noexcept;
    };

    enum class AudioBus : std::uint8_t
    {
        Master,
        Music,
        Effects,
        Ui,
        Count
    };

    using GameObjectId = std::uint64_t;

    enum class PrimitiveShape : std::uint8_t
    {
        Plane,
        Cube,
        Sphere,
        Cylinder,
    };

    enum class ShaderCullMode : std::uint8_t
    {
        Back,
        Front,
        None,
    };

    enum class ParticleEmitterShape : std::uint8_t
    {
        Cone,
        Sphere,
        Box,
        Point = Cone,
    };

    enum class ParticleRenderMode : std::uint8_t
    {
        Billboard,
        Horizontal,
    };

    enum class SpriteMaskShape : std::uint8_t
    {
        Rectangle,
        Circle,
    };

    enum class SpriteMaskInteraction : std::uint8_t
    {
        None,
        VisibleInsideMask,
        VisibleOutsideMask,
    };

    enum class TextHorizontalAlignment : std::uint8_t
    {
        Left,
        Center,
        Right,
    };

    enum class TextVerticalAlignment : std::uint8_t
    {
        Top,
        Center,
        Bottom,
    };

    struct ProceduralMeshVertex final
    {
        // 頂点位置
        DirectX::XMFLOAT3 position{};
        // 頂点法線
        DirectX::XMFLOAT3 normal{ 0.0f, 1.0f, 0.0f };
        // テクスチャ座標
        DirectX::XMFLOAT2 textureCoordinate{};
    };

    struct CollisionEvent final
    {
        // 衝突相手の非所有参照
        GameObject& other;
        // 接触面の法線
        DirectX::XMFLOAT3 normal{};
        // 接触位置
        DirectX::XMFLOAT3 point{};
        // 接触時の重なり量
        float penetration{};
        // Trigger衝突か
        bool isTrigger{};
    };

    struct Ray final
    {
        // 光線の始点
        DirectX::XMFLOAT3 origin{};
        // 光線の方向
        DirectX::XMFLOAT3 direction{ 0.0f, -1.0f, 0.0f };
    };

    struct PhysicsQueryFilter final
    {
        // 判定対象のレイヤー
        std::uint32_t layerMask{ 0xffffffffu };
        // 判定から除外するGameObject
        GameObjectId ignoredGameObjectId{};
    };

    struct PhysicsHit final
    {
        // 衝突したGameObject
        GameObject* gameObject{};
        // 衝突位置
        DirectX::XMFLOAT3 point{};
        // 衝突面の法線
        DirectX::XMFLOAT3 normal{};
        // 光線始点からの距離
        float distance{};
    };

    class Transform final
    {
    public:
        // ワールド位置
        DirectX::XMFLOAT3 position{};
        // XYZオイラー角
        DirectX::XMFLOAT3 rotation{};
        // 軸ごとの拡大率
        DirectX::XMFLOAT3 scale{ 1.0f, 1.0f, 1.0f };

        // XYZオイラー角を設定します。
        // SetEulerAngles(pitch: X回転, yaw: Y回転, roll: Z回転)
        void SetEulerAngles(float pitch, float yaw, float roll) noexcept
        {
            rotation = { pitch, yaw, roll };
        }

    private:
        friend class GameObject;
        // 所有するGameObject
        GameObject* m_owner{};
    };

    class Component
    {
    public:
        // 派生コンポーネントを破棄可能にします。
        virtual ~Component() = default;

        // 所有先と有効状態の二重管理を防ぎます。
        Component(const Component&) = delete;
        Component& operator=(const Component&) = delete;

        // 所有GameObjectを返します。
        [[nodiscard]] GameObject& Owner() const noexcept { return *m_owner; }
        // 所属オブジェクトのTransformを返します。
        [[nodiscard]] Transform& GetTransform() const noexcept;
        // 有効状態を返します。
        [[nodiscard]] bool IsEnabled() const noexcept { return m_enabled; }
        // 所有オブジェクトと自身が有効な状態か返します。
        [[nodiscard]] bool IsActiveAndEnabled() const noexcept;
        // 有効状態を設定します。
        // SetEnabled(enabled: 有効状態)
        void SetEnabled(bool enabled);
        // コンポーネントの型名を返します。
        [[nodiscard]] virtual std::string_view TypeName() const noexcept
        {
            return "Component";
        }
        // NativeScriptComponentの実体を返し、それ以外はnullptrを返します。
        [[nodiscard]] virtual Script* ScriptInstance() const noexcept
        {
            return nullptr;
        }

    protected:
        // 未所属のPortable componentを構築します。
        Component() = default;
        // GameObjectへの追加後に呼ばれます。
        virtual void OnAttached() {}

    private:
        friend class GameObject;
        // 所有GameObject
        GameObject* m_owner{};
        // コンポーネント有効状態
        bool m_enabled{ true };
    };

    class RuntimeState final
    {
    public:
        // 数値をキーで保存します。
        // SetNumber(key: 保存キー, value: 数値)
        void SetNumber(std::string key, double value);
        // 整数をキーで保存します。
        // SetInteger(key: 保存キー, value: 整数)
        void SetInteger(std::string key, std::int64_t value);
        // 真偽値をキーで保存します。
        // SetBoolean(key: 保存キー, value: 真偽値)
        void SetBoolean(std::string key, bool value);
        // 文字列をキーで保存します。
        // SetString(key: 保存キー, value: 文字列)
        void SetString(std::string key, std::string value);

        // 保存値または代替数値を返します。
        // Number(key: 保存キー, fallback: 未登録時の値)
        [[nodiscard]] double Number(
            std::string_view key,
            double fallback = 0.0) const;
        // 保存値または代替整数を返します。
        // Integer(key: 保存キー, fallback: 未登録時の値)
        [[nodiscard]] std::int64_t Integer(
            std::string_view key,
            std::int64_t fallback = 0) const;
        // 保存値または代替真偽値を返します。
        // Boolean(key: 保存キー, fallback: 未登録時の値)
        [[nodiscard]] bool Boolean(
            std::string_view key,
            bool fallback = false) const;
        // 保存値または代替文字列を返します。
        // String(key: 保存キー, fallback: 未登録時の値)
        [[nodiscard]] std::string String(
            std::string_view key,
            std::string fallback = {}) const;

    private:
        // キー別の数値保存先
        std::unordered_map<std::string, double> m_numbers;
        // キー別の整数保存先
        std::unordered_map<std::string, std::int64_t> m_integers;
        // キー別の真偽値保存先
        std::unordered_map<std::string, bool> m_booleans;
        // キー別の文字列保存先
        std::unordered_map<std::string, std::string> m_strings;
    };

    class SceneCollection final
    {
    public:
        // 主シーンの切り替えを次のScene更新時に予約します(scenePath: assets内のScene path)。
        [[nodiscard]] bool RequestLoad(std::filesystem::path scenePath)
        {
            if (scenePath.empty())
            {
                m_lastError = "Scene path is empty.";
                return false;
            }
            m_pendingPath = std::move(scenePath);
            m_hasPendingLoad = true;
            m_lastError.clear();
            return true;
        }
        // 現在の主シーンを再読み込みします。読み込み前ならfalseを返します。
        [[nodiscard]] bool RequestReload()
        {
            if (m_currentPath.empty())
            {
                m_lastError = "No current scene is available to reload.";
                return false;
            }
            return RequestLoad(m_currentPath);
        }
        // 主シーン読み込みが予約されているか返します。
        [[nodiscard]] bool HasPendingLoad() const noexcept
        {
            return m_hasPendingLoad;
        }
        // 現在の主シーンpathを返します。
        [[nodiscard]] const std::filesystem::path&
            CurrentScenePath() const noexcept
        {
            return m_currentPath;
        }
        // 予約中の主シーンpathを返します。
        [[nodiscard]] const std::filesystem::path&
            PendingScenePath() const noexcept
        {
            return m_pendingPath;
        }
        // 最後に記録した読み込みエラーを返します。
        [[nodiscard]] const std::string& LastError() const noexcept
        {
            return m_lastError;
        }
        // Scene読み込みが成功するたびに増えるrevisionを返します。
        [[nodiscard]] std::uint64_t LoadRevision() const noexcept
        {
            return m_loadRevision;
        }
        // 保留中のScene読み込みを取り消します。
        void CancelPending() noexcept
        {
            m_hasPendingLoad = false;
            m_pendingPath.clear();
        }
        // 共有ランタイム状態を返します。
        [[nodiscard]] RuntimeState& State() noexcept { return m_state; }
        // 共有ランタイム状態を読み取り専用で返します。
        [[nodiscard]] const RuntimeState& State() const noexcept
        {
            return m_state;
        }

    private:
        friend class Scene;
        // Scene runtimeが保留pathを一度だけ取り出します。
        [[nodiscard]] std::filesystem::path TakePendingPath()
        {
            m_hasPendingLoad = false;
            auto path = std::move(m_pendingPath);
            m_pendingPath.clear();
            return path;
        }
        // Sceneの読み込み成功を記録します(path: 読み込んだassets path)。
        void RecordLoadSuccess(std::filesystem::path path)
        {
            m_currentPath = std::move(path);
            ++m_loadRevision;
            m_lastError.clear();
        }
        // Sceneの読み込み失敗を記録します(error: 利用者向け理由)。
        void RecordLoadFailure(std::string error)
        {
            m_lastError = std::move(error);
        }
        // コレクション内の共有状態
        RuntimeState m_state;
        // 次回Updateで置き換えるScene path
        std::filesystem::path m_pendingPath;
        // 最後に成功したScene path
        std::filesystem::path m_currentPath;
        // 最後の読み込みエラー
        std::string m_lastError;
        // 成功したScene読み込み件数
        std::uint64_t m_loadRevision{};
        // pending pathが有効か
        bool m_hasPendingLoad{};
    };

    enum class PointerButton : std::uint8_t
    {
        Left,
        Right,
        Middle,
        Extra1,
        Extra2,
        Count
    };

    struct InputPointerButtonState final
    {
        // 現在押されている状態
        bool down{};
        // 今フレームに押された状態
        bool pressed{};
        // 今フレームに離された状態
        bool released{};
    };

    struct InputPointerState final
    {
        // ビューポート内の座標
        DirectX::XMFLOAT2 position{};
        // 座標が取得できたか
        bool valid{};
        // ポインター押下中か
        bool down{};
        // 今フレームに押されたか
        bool pressed{};
        // 今フレームに離されたか
        bool released{};
        // 前フレームからの移動量
        DirectX::XMFLOAT2 delta{};
        // 縦ホイール移動量
        float wheel{};
        // 横ホイール移動量
        float wheelHorizontal{};
        // ボタン別の状態
        std::array<
            InputPointerButtonState,
            static_cast<std::size_t>(PointerButton::Count)> buttons{};

        // 指定ボタンの状態を返します。
        // Button(button: 対象ボタン)
        [[nodiscard]] const InputPointerButtonState& Button(
            PointerButton button) const noexcept
        {
            return buttons[static_cast<std::size_t>(button)];
        }
    };

    struct PortableKeyboardState final
    {
        // Spaceキーの状態
        bool Space{};
        // Rキーの状態
        bool R{};
    };

    class InputSystem final
    {
    public:
        // Web入力装置へ接続します。
        // Bind(input: Web入力装置)
        void Bind(Web::WebInput* input) noexcept { m_input = input; }
        // アクションの入力値を返します。
        // Value(action: 入力アクション名)
        [[nodiscard]] float Value(std::string_view action) const;
        // アクションの押下イベントを返します。
        // WasPressed(action: 入力アクション名)
        [[nodiscard]] bool WasPressed(std::string_view action) const;
        // 閾値以上の押下状態を返します。
        // IsDown(action: 入力アクション名, threshold: 判定閾値)
        [[nodiscard]] bool IsDown(
            std::string_view action,
            float threshold = 0.5f) const
        {
            return std::abs(Value(action)) >= threshold;
        }
        // アクションの解放イベントを返します。
        // WasReleased(action: 入力アクション名, threshold: 判定閾値)
        [[nodiscard]] bool WasReleased(
            std::string_view action,
            float threshold = 0.5f) const;
        // 現在のポインター状態を返します。
        [[nodiscard]] const InputPointerState& Pointer() const noexcept;
        // テスト用キー状態を返します。
        [[nodiscard]] const PortableKeyboardState& KeyboardState() const noexcept;
        // 押下・解放イベントの通知を切り替えます。
        // SetEdgeEventsEnabled(value: 通知の有効状態)
        void SetEdgeEventsEnabled(bool value) noexcept
        {
            m_edgeEventsEnabled = value;
        }

    private:
        // 接続中のWeb入力装置
        Web::WebInput* m_input{};
        // 更新ごとのポインター状態
        mutable InputPointerState m_pointer;
        // 更新ごとのキーボード状態
        mutable PortableKeyboardState m_keyboardState;
        // 押下・解放イベントの通知状態
        bool m_edgeEventsEnabled{ true };
    };

    class GraphicsDevice final
    {
    public:
        // 入力システムを返します。
        [[nodiscard]] InputSystem& Input() noexcept { return m_input; }
        // 入力システムを読み取り専用で返します。
        [[nodiscard]] const InputSystem& Input() const noexcept
        {
            return m_input;
        }
        // UIビューポートの幅を返します。
        [[nodiscard]] std::uint32_t UIWidth() const noexcept { return m_width; }
        // UIビューポートの高さを返します。
        [[nodiscard]] std::uint32_t UIHeight() const noexcept { return m_height; }
        // UIビューポート寸法を設定します。
        // SetUiSize(width: ビューポート幅, height: ビューポート高さ)
        void SetUiSize(std::uint32_t width, std::uint32_t height) noexcept
        {
            m_width = width;
            m_height = height;
        }

    private:
        // グラフィックスに結び付く入力
        InputSystem m_input;
        // UIビューポートの幅
        std::uint32_t m_width{ 1280 };
        // UIビューポートの高さ
        std::uint32_t m_height{ 720 };
    };

    class Script
    {
    public:
        // 派生スクリプトのイベント購読を解除してから破棄します。
        virtual ~Script()
        {
            for (const auto& subscription : m_eventSubscriptions)
            {
                subscription.first->Unsubscribe(subscription.second);
            }
        }
        // 生成と初期プロパティ読み込みの後、一度だけ呼ばれます。
        virtual void Awake() {}
        // スクリプト開始時に一度呼ばれます。
        virtual void Start() {}
        // 実効アクティブ状態が有効へ変わったとき呼ばれます。
        virtual void OnEnable() {}
        // 実効アクティブ状態が無効へ変わったとき呼ばれます。
        virtual void OnDisable() {}
        // インスタンス破棄の直前に呼ばれます。
        virtual void OnDestroy() {}
        // 固定時間刻みで呼ばれます。
        virtual void FixedUpdate(float) {}
        // フレームごとに呼ばれます。
        virtual void Update(float) {}
        // すべてのフレーム更新と物理計算の後、描画前に呼ばれます。
        virtual void LateUpdate(float) {}
        // 衝突開始時に呼ばれます。
        virtual void OnCollisionEnter(const CollisionEvent&) {}
        // 衝突継続中に呼ばれます。
        virtual void OnCollisionStay(const CollisionEvent&) {}
        // 衝突終了時に呼ばれます。
        virtual void OnCollisionExit(const CollisionEvent&) {}
        // トリガーへ入ったときに呼ばれます。
        virtual void OnTriggerEnter(const CollisionEvent&) {}
        // トリガー内の接触が続く間に呼ばれます。
        virtual void OnTriggerStay(const CollisionEvent&) {}
        // トリガーから出たときに呼ばれます。
        virtual void OnTriggerExit(const CollisionEvent&) {}
        // 保存済みプロパティを読み込みます。
        // LoadProperties(serialized: シリアライズ済みプロパティ)
        virtual void LoadProperties(std::string_view) {}
        // 保存可能なプロパティを返します。
        [[nodiscard]] virtual std::string SaveProperties() const
        {
            return "{}";
        }

    protected:
        // 名前付きSceneイベントを購読し、Script破棄時に自動解除します。
        std::uint64_t On(
            std::string_view eventName,
            std::function<void(const EventArgs&)> handler);
        // 引数を使わないSceneイベントを購読します。
        std::uint64_t On(
            std::string_view eventName,
            std::function<void()> handler);
        // Onが返した番号のイベント購読を解除します。
        void Off(std::uint64_t handle);
        // 自身を送信元としてSceneイベントを同期発行します。
        void Emit(std::string_view eventName);
        // 送信元を指定してSceneイベントを同期発行します。
        void Emit(std::string_view eventName, EventArgs eventArgs);
        // 所属シーンを返します。
        [[nodiscard]] Scene& GetScene() const noexcept;
        // 描画装置を返します。
        [[nodiscard]] GraphicsDevice& Graphics() const noexcept;
        // 所有GameObjectを返します。
        [[nodiscard]] GameObject& Owner() const noexcept;
        // 自身のGameObjectから指定型のコンポーネントを検索します。
        template<typename T>
        [[nodiscard]] T* GetComponent() noexcept;
        // 自身のGameObjectから指定型のコンポーネントを読み取ります。
        template<typename T>
        [[nodiscard]] const T* GetComponent() const noexcept;
        // 自身のGameObjectから指定型のスクリプトを検索します。
        template<typename T>
        [[nodiscard]] T* GetScript() const noexcept;
        // 自身または祖先から指定型のコンポーネントを検索します。
        template<typename T>
        [[nodiscard]] T* GetComponentInParent(
            bool includeInactive = false) noexcept;
        // 自身と子孫から指定型のコンポーネントを検索します。
        template<typename T>
        [[nodiscard]] T* GetComponentInChildren(
            bool includeInactive = false) noexcept;
        // 自身と子孫の指定型コンポーネントを深さ優先で収集します。
        template<typename T>
        [[nodiscard]] std::vector<T*> GetComponentsInChildren(
            bool includeInactive = false);
        // 自身と祖先から各GameObjectの最初の指定型コンポーネントを収集します。
        template<typename T>
        [[nodiscard]] std::vector<T*> GetComponentsInParent(
            bool includeInactive = false);
        // 自身と子孫から指定型スクリプトを検索します。
        template<typename T>
        [[nodiscard]] T* GetScriptInChildren(
            bool includeInactive = false) const noexcept;
        // 自身のGameObjectへコンポーネントを追加します。
        template<typename T, typename... Args>
        T& AddComponent(Args&&... args);
        // 自身のTransformを借用します。
        [[nodiscard]] Transform& GetTransform() noexcept;
        // 自身のTransformを読み取ります。
        [[nodiscard]] const Transform& GetTransform() const noexcept;
        // 名前でGameObjectを検索します。
        // Find(name: 検索する名前)
        [[nodiscard]] GameObject* Find(std::string_view name) const noexcept;
        // Scene内でタグの一致する最初の物体を借用します。
        // FindWithTag(tag: 検索するタグ)
        [[nodiscard]] GameObject* FindWithTag(
            std::string_view tag) const noexcept;
        // Scene内でタグの一致する物体を登録順に借用します。
        // FindObjectsWithTag(tag: 検索するタグ)
        [[nodiscard]] std::vector<GameObject*> FindObjectsWithTag(
            std::string_view tag) const;
        // GameObjectの破棄を予約します。
        // Destroy(gameObject: 破棄対象)
        bool Destroy(GameObject& gameObject);
        // PrefabをSceneへ生成します(prefabPath: Prefabアセット, parent: 任意の親GameObject)
        [[nodiscard]] GameObject& Instantiate(
            const std::filesystem::path& prefabPath,
            GameObject* parent = nullptr);
        // 永続状態から文字列を読み込みます。保存済みの空文字列もそのまま返します。
        // LoadText(key: 保存キー, fallback: 未登録時の値)
        [[nodiscard]] std::string LoadText(
            std::string_view key,
            std::string fallback = {}) const;
        // 文字列を永続状態へ保存します。保存失敗時は例外で通知します。
        // SaveText(key: 保存キー, value: 保存文字列)
        void SaveText(std::string_view key, std::string_view value) const;
        // 永続状態から整数を読み込みます。
        // LoadInteger(key: 保存キー, fallback: 未登録時の値)
        [[nodiscard]] std::int64_t LoadInteger(
            std::string_view key,
            std::int64_t fallback = 0) const;
        // 整数を永続状態へ保存します。
        // SaveInteger(key: 保存キー, value: 保存整数)
        void SaveInteger(std::string_view key, std::int64_t value) const;

    private:
        friend class NativeScriptComponent;
        friend class Scene;
        friend class GameObject;
        // スクリプト所有GameObject
        GameObject* m_owner{};
        // Startが実行されたか
        bool m_started{};
        // Awakeが実行されたか
        bool m_awake{};
        // 実効アクティブ状態
        bool m_active{};
        // Script破棄時に解除するScene event busと購読番号
        std::vector<std::pair<EventBus*, std::uint64_t>> m_eventSubscriptions;
    };

    using ScriptFactory = std::function<std::unique_ptr<Script>()>;

    // ポータブル実行用スクリプトを登録します。
    // RegisterPortableScript(id: 登録ID, displayName: 表示名, factory: 生成関数)
    bool RegisterPortableScript(
        std::string id,
        std::string displayName,
        ScriptFactory factory);
    // 登録IDからスクリプトを生成します。
    // CreatePortableScript(id: 登録ID)
    [[nodiscard]] std::unique_ptr<Script> CreatePortableScript(
        std::string_view id);

    class GameObject final
    {
    public:
        // 所属シーン、識別子、名前を指定して生成します。
        // GameObject(scene: 所属シーン, id: 一意な識別子, name: 表示名)
        GameObject(Scene& scene, GameObjectId id, std::string name);

        // 一意な識別子を返します。
        [[nodiscard]] GameObjectId Id() const noexcept { return m_id; }
        // 表示名を返します。
        [[nodiscard]] const std::string& Name() const noexcept { return m_name; }
        // 表示名を置き換えます(name: 新しい表示名)。
        void SetName(std::string name) { m_name = std::move(name); }
        // 所属シーンを返します。
        [[nodiscard]] Scene& GetScene() const noexcept { return *m_scene; }
        // Transformを変更可能な参照で返します。
        [[nodiscard]] Transform& GetTransform() noexcept { return m_transform; }
        // Transformを読み取り専用で返します。
        [[nodiscard]] const Transform& GetTransform() const noexcept
        {
            return m_transform;
        }
        // 親GameObjectを設定します。
        // SetParent(parent: 親GameObject, nullptrで親解除)
        // 自分自身・循環する親・別Sceneの親はinvalid_argumentで拒否します。
        void SetParent(GameObject* parent)
        {
            if (parent == this)
            {
                throw std::invalid_argument(
                    "A GameObject cannot be parented to itself.");
            }
            if (parent != nullptr && parent->m_scene != m_scene)
            {
                throw std::invalid_argument(
                    "A GameObject cannot be parented across scenes.");
            }
            for (auto* ancestor = parent;
                 ancestor != nullptr;
                 ancestor = ancestor->m_parent)
            {
                if (ancestor == this)
                {
                    throw std::invalid_argument(
                        "GameObject parenting would create a cycle.");
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
            RefreshScriptActiveState();
        }
        // 親GameObjectを返します。
        [[nodiscard]] GameObject* Parent() const noexcept { return m_parent; }
        // 直下の子GameObjectを登録順で返します。
        [[nodiscard]] const std::vector<GameObject*>& Children() const noexcept
        {
            return m_children;
        }
        // 有効状態を返します。
        [[nodiscard]] bool IsEnabled() const noexcept { return m_enabled; }
        // 自身と祖先がすべて有効な場合にtrueを返します。
        [[nodiscard]] bool IsActiveInHierarchy() const noexcept
        {
            for (auto* current = this;
                 current != nullptr;
                 current = current->Parent())
            {
                if (!current->IsEnabled())
                {
                    return false;
                }
            }
            return true;
        }
        // 有効状態を設定します。
        // SetEnabled(enabled: 有効状態)
        void SetEnabled(bool enabled);
        // タグを設定します。
        // SetTag(tag: 検索に使うタグ)
        void SetTag(std::string tag) { m_tag = std::move(tag); }
        // タグを返します。
        [[nodiscard]] const std::string& Tag() const noexcept { return m_tag; }
        // 分類タグが完全一致するか返します。
        [[nodiscard]] bool CompareTag(std::string_view tag) const noexcept
        {
            return m_tag == tag;
        }

        // 補間済みワールド行列を返します。
        // InterpolatedWorldMatrix(alpha: 補間率)
        [[nodiscard]] DirectX::XMMATRIX InterpolatedWorldMatrix(float) const;
        // 現在のワールド行列を返します。
        [[nodiscard]] DirectX::XMMATRIX WorldMatrix() const noexcept;

        // 有効なカリング設定が常時表示を指定するか返します。
        [[nodiscard]] bool IsAlwaysVisible() const noexcept;
        // 有効なカリング設定の境界余白を返し、設定がなければ0を返します。
        [[nodiscard]] float CullingMargin() const noexcept;

        // 指定型のコンポーネントを生成して追加します。
        // AddComponent(args: コンストラクター引数)
        template<typename T, typename... Args>
        T& AddComponent(Args&&... args)
        {
            static_assert(std::is_base_of_v<Component, T>);
            // 所有権を保持する新規コンポーネント
            auto component = std::make_unique<T>(std::forward<Args>(args)...);
            component->m_owner = this;
            // 呼び出し元へ返すコンポーネント参照
            T& reference = *component;
            m_components.push_back(std::move(component));
            static_cast<Component&>(reference).OnAttached();
            return reference;
        }

        // 指定型のコンポーネントを検索します。
        template<typename T>
        [[nodiscard]] T* GetComponent() noexcept
        {
            // 所有コンポーネントを検索
            for (const auto& component : m_components)
            {
                // 型が一致するコンポーネント
                if (auto* value = dynamic_cast<T*>(component.get()))
                {
                    return value;
                }
            }
            return nullptr;
        }

        // 指定型のコンポーネントを読み取り専用で検索します。
        template<typename T>
        [[nodiscard]] const T* GetComponent() const noexcept
        {
            // 所有コンポーネントを検索
            for (const auto& component : m_components)
            {
                // 型が一致するコンポーネント
                if (auto* value = dynamic_cast<const T*>(component.get()))
                {
                    return value;
                }
            }
            return nullptr;
        }

        // 自身または祖先から指定型のコンポーネントを検索します。
        // GetComponentInParent(includeInactive: 無効な階層も検索)
        template<typename T>
        [[nodiscard]] T* GetComponentInParent(
            bool includeInactive = false) noexcept
        {
            static_assert(std::is_base_of_v<Component, T>);
            const auto isActiveInHierarchy = [](const GameObject* candidate)
            {
                for (auto* current = candidate;
                     current != nullptr;
                     current = current->Parent())
                {
                    if (!current->IsEnabled())
                    {
                        return false;
                    }
                }
                return true;
            };
            for (auto* current = this;
                 current != nullptr;
                 current = current->Parent())
            {
                if (!includeInactive
                    && !isActiveInHierarchy(current))
                {
                    continue;
                }
                if (auto* match = current->GetComponent<T>())
                {
                    return match;
                }
            }
            return nullptr;
        }

        // 自身と子孫から指定型の最初のコンポーネントを深さ優先で検索します。
        template<typename T>
        [[nodiscard]] T* GetComponentInChildren(
            bool includeInactive = false) noexcept
        {
            static_assert(std::is_base_of_v<Component, T>);
            if (!includeInactive && !IsActiveInHierarchy())
            {
                return nullptr;
            }
            if (auto* match = GetComponent<T>())
            {
                return match;
            }
            for (auto* child : m_children)
            {
                if (auto* match = child->GetComponentInChildren<T>(
                        includeInactive))
                {
                    return match;
                }
            }
            return nullptr;
        }

        // 自身と子孫の指定型コンポーネントを深さ優先で収集します。
        template<typename T>
        [[nodiscard]] std::vector<T*> GetComponentsInChildren(
            bool includeInactive = false)
        {
            static_assert(std::is_base_of_v<Component, T>);
            std::vector<T*> results;
            const auto collect = [&](const auto& self, GameObject& current) -> void
            {
                if (!includeInactive && !current.IsActiveInHierarchy())
                {
                    return;
                }
                for (const auto& component : current.m_components)
                {
                    if (auto* match = dynamic_cast<T*>(component.get()))
                    {
                        results.push_back(match);
                    }
                }
                for (auto* child : current.m_children)
                {
                    self(self, *child);
                }
            };
            collect(collect, *this);
            return results;
        }

        // 自身と祖先から各GameObjectの最初の指定型コンポーネントを収集します。
        template<typename T>
        [[nodiscard]] std::vector<T*> GetComponentsInParent(
            bool includeInactive = false)
        {
            static_assert(std::is_base_of_v<Component, T>);
            std::vector<T*> results;
            for (auto* current = this;
                 current != nullptr;
                 current = current->Parent())
            {
                if (!includeInactive && !current->IsActiveInHierarchy())
                {
                    continue;
                }
                if (auto* match = current->GetComponent<T>())
                {
                    results.push_back(match);
                }
            }
            return results;
        }

        // 名前をたどり子孫を返し、未発見や空の区間ならnullptrを返します。
        // 無効な子も検索し、同名の兄弟は最初の一致を採用します。
        [[nodiscard]] GameObject* FindChild(
            std::string_view path) const noexcept;

        // 指定型のネイティブスクリプトを検索します。
        template<typename T>
        [[nodiscard]] T* GetScript() const noexcept;

        // 自身と子孫から指定型スクリプトを深さ優先で検索します。
        template<typename T>
        [[nodiscard]] T* GetScriptInChildren(
            bool includeInactive = false) const noexcept
        {
            if (!includeInactive && !IsActiveInHierarchy())
            {
                return nullptr;
            }
            if (auto* match = GetScript<T>())
            {
                return match;
            }
            for (auto* child : m_children)
            {
                if (auto* match = child->GetScriptInChildren<T>(
                        includeInactive))
                {
                    return match;
                }
            }
            return nullptr;
        }

        // 所有コンポーネント一覧を返します。
        [[nodiscard]] const std::vector<std::unique_ptr<Component>>& Components()
            const noexcept
        {
            return m_components;
        }
        // 基準の前後へコンポーネントを移します。
        // 同一対象や所属外の指定はfalseを返し、移動に成功するとtrueを返します。
        bool ReorderComponent(
            const Component& moved,
            const Component& reference,
            bool insertAfter);

    private:
        friend class Component;
        // 自身と子孫のScript有効状態遷移を通知します。
        void RefreshScriptActiveState();
        // 所属シーン
        Scene* m_scene{};
        // 一意なオブジェクト識別子
        GameObjectId m_id{};
        // 表示名
        std::string m_name;
        // 検索用タグ
        std::string m_tag;
        // 位置、回転、拡大率
        Transform m_transform;
        // 親GameObject
        GameObject* m_parent{};
        // 子GameObject
        std::vector<GameObject*> m_children;
        // 有効状態
        bool m_enabled{ true };
        // 所有コンポーネント一覧
        std::vector<std::unique_ptr<Component>> m_components;
    };

    // Web版の局所照明。World変換は描画時に読み直します。
    class PortableLocalLightComponent : public Component
    {
    public:
        explicit PortableLocalLightComponent(DirectX::XMFLOAT3 color = {1.0f, 0.72f, 0.42f},
            float intensity = 3.0f, float range = 8.0f) noexcept
        { SetColor(color); SetIntensity(intensity); SetRange(range); }
        void SetColor(const DirectX::XMFLOAT3& value) noexcept
        { m_color = {std::clamp(value.x, 0.0f, 1.0f), std::clamp(value.y, 0.0f, 1.0f), std::clamp(value.z, 0.0f, 1.0f)}; }
        const DirectX::XMFLOAT3& Color() const noexcept { return m_color; }
        void SetIntensity(float value) noexcept { m_intensity = std::clamp(value, 0.0f, 64.0f); }
        float Intensity() const noexcept { return m_intensity; }
        void SetRange(float value) noexcept { m_range = std::clamp(value, 0.1f, 1000.0f); }
        float Range() const noexcept { return m_range; }
        DirectX::XMFLOAT3 WorldPosition() const noexcept;
    private:
        DirectX::XMFLOAT3 m_color;
        float m_intensity{}, m_range{};
    };

    class PointLightComponent final : public PortableLocalLightComponent
    {
    public:
        using PortableLocalLightComponent::PortableLocalLightComponent;
        // Windowsと同じ保存用コンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        { return "PointLight"; }
    };

    class SpotLightComponent final : public PortableLocalLightComponent
    {
    public:
        explicit SpotLightComponent(DirectX::XMFLOAT3 color = {1.0f, 0.88f, 0.68f},
            float intensity = 5.0f, float range = 12.0f,
            float innerConeAngle = 0.3926991f, float outerConeAngle = 0.6108652f) noexcept
            : PortableLocalLightComponent(color, intensity, range)
        { SetOuterConeAngle(outerConeAngle); SetInnerConeAngle(innerConeAngle); }
        void SetInnerConeAngle(float value) noexcept
        { m_inner = std::clamp(value, 0.0174533f, m_outer); }
        void SetOuterConeAngle(float value) noexcept
        { m_outer = std::clamp(value, m_inner, 1.553343f); }
        float InnerConeAngle() const noexcept { return m_inner; }
        float OuterConeAngle() const noexcept { return m_outer; }
        DirectX::XMFLOAT3 WorldDirection() const noexcept;
        // Windowsと同じ保存用コンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        { return "SpotLight"; }
    private:
        float m_inner{0.0174533f}, m_outer{0.6108652f};
    };

    class MeshRendererComponent final : public Component
    {
    public:
        // プリミティブ形状と材質色でメッシュ描画を初期化します。
        // MeshRendererComponent(shape: 形状, color: RGBA色, albedo: アルベド画像パス)
        MeshRendererComponent(
            PrimitiveShape shape,
            DirectX::XMFLOAT4 color,
            std::filesystem::path albedo = {});

        // 裏面の描画方式を設定します。
        // SetCullMode(mode: 面カリング方式)
        void SetCullMode(ShaderCullMode mode) noexcept { m_cullMode = mode; }
        // 表面粗さを設定します。
        // SetRoughness(value: 粗さ係数)
        void SetRoughness(float value) noexcept { m_roughness = value; }
        // 金属度を設定します。
        // SetMetallic(value: 金属度係数)
        void SetMetallic(float value) noexcept { m_metallic = value; }
        // 法線画像のパスを設定します。
        // SetNormalTexturePath(path: 法線画像パス)
        void SetNormalTexturePath(std::filesystem::path path)
        {
            m_normal = std::move(path);
        }
        // 法線マップの影響度を設定します。
        // SetNormalStrength(value: 法線マップ強度)
        void SetNormalStrength(float value) noexcept { m_normalStrength = value; }
        // 粗さ画像のパスを設定します。
        // SetRoughnessTexturePath(path: 粗さ画像パス)
        void SetRoughnessTexturePath(std::filesystem::path path)
        {
            m_roughnessTexture = std::move(path);
        }
        // 金属度画像のパスを設定します。
        // SetMetallicTexturePath(path: 金属度画像パス)
        void SetMetallicTexturePath(std::filesystem::path path)
        {
            m_metallicTexture = std::move(path);
        }
        // 遮蔽画像のパスを設定します。
        // SetOcclusionTexturePath(path: 遮蔽画像パス)
        void SetOcclusionTexturePath(std::filesystem::path path)
        {
            m_occlusionTexture = std::move(path);
        }
        // 遮蔽画像の影響度を設定します。
        // SetOcclusionStrength(value: 遮蔽強度)
        void SetOcclusionStrength(float value) noexcept
        {
            m_occlusionStrength = value;
        }
        // 発光画像のパスを設定します。
        // SetEmissiveTexturePath(path: 発光画像パス)
        void SetEmissiveTexturePath(std::filesystem::path path)
        {
            m_emissiveTexture = std::move(path);
        }
        // 発光色を設定します。
        // SetEmissiveColor(value: RGB発光色)
        void SetEmissiveColor(DirectX::XMFLOAT3 value) noexcept
        {
            m_emissiveColor = value;
        }
        // 頂点列と三角形から手続きメッシュを設定します。
        // SetProceduralMesh(vertices: 頂点列, indices: 三角形頂点番号, recalculateNormals: 法線再計算設定)
        void SetProceduralMesh(
            std::vector<ProceduralMeshVertex> vertices,
            std::vector<std::uint32_t> indices,
            bool recalculateNormals);

        // Windowsと同じ保存用コンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        { return "MeshRenderer"; }
    private:
        friend class Scene;
        // メッシュの基本色
        DirectX::XMFLOAT4 m_color{};
        // アルベド画像パス
        std::filesystem::path m_albedo;
        // 法線画像パス
        std::filesystem::path m_normal;
        // 粗さ画像パス
        std::filesystem::path m_roughnessTexture;
        // 金属度画像パス
        std::filesystem::path m_metallicTexture;
        // 遮蔽画像パス
        std::filesystem::path m_occlusionTexture;
        // 発光画像パス
        std::filesystem::path m_emissiveTexture;
        // 面カリング方式
        ShaderCullMode m_cullMode{ ShaderCullMode::Back };
        // 表面粗さ係数
        float m_roughness{ 0.65f };
        // 金属度係数
        float m_metallic{};
        // 法線マップ強度
        float m_normalStrength{ 1.0f };
        // 遮蔽画像強度
        float m_occlusionStrength{ 1.0f };
        // 発光色
        DirectX::XMFLOAT3 m_emissiveColor{};
        // 描画するメッシュ頂点
        std::vector<ProceduralMeshVertex> m_vertices;
        // 三角形の頂点インデックス
        std::vector<std::uint32_t> m_indices;
        // Web側メッシュハンドル
        std::uint32_t m_webMesh{};
        // Web側アルベド画像ハンドル
        std::uint32_t m_webTexture{};
        // Web側法線画像ハンドル
        std::uint32_t m_webNormalTexture{};
        // Web側粗さ画像ハンドル
        std::uint32_t m_webRoughnessTexture{};
        // Web側金属度画像ハンドル
        std::uint32_t m_webMetallicTexture{};
        // Web側遮蔽画像ハンドル
        std::uint32_t m_webOcclusionTexture{};
        // Web側発光画像ハンドル
        std::uint32_t m_webEmissiveTexture{};
        // 描画データ再生成が必要か
        bool m_dirty{ true };
    };

    class ModelRendererComponent final : public Component
    {
    public:
        // モデルと材質設定から描画要素を初期化します。
        // ModelRendererComponent(modelPath: モデルパス, wireframe: 線描画, materialOverrideEnabled: 材質上書き, color: RGBA色, albedoTexture: アルベド画像, normalTexture: 法線画像, roughness: 粗さ, normalStrength: 法線強度)
        explicit ModelRendererComponent(
            std::filesystem::path modelPath = {},
            bool wireframe = false,
            bool materialOverrideEnabled = false,
            DirectX::XMFLOAT4 color = { 1.0f, 1.0f, 1.0f, 1.0f },
            std::filesystem::path albedoTexture = {},
            std::filesystem::path normalTexture = {},
            float roughness = 0.5f,
            float normalStrength = 1.0f);

        // 描画モデルのパスを設定します。
        // SetModelPath(path: モデルパス)
        void SetModelPath(std::filesystem::path path);
        // 現在のモデルパスを返します。
        [[nodiscard]] const std::filesystem::path& ModelPath() const noexcept
        {
            return m_modelPath;
        }
        // 描画色を設定します。
        // SetColor(value: RGBA色)
        void SetColor(const DirectX::XMFLOAT4& value) noexcept { m_color = value; }
        // アルベド画像のパスを設定します。
        // SetAlbedoTexturePath(path: アルベド画像パス)
        void SetAlbedoTexturePath(std::filesystem::path path)
        {
            m_albedoTexture = std::move(path);
        }
        // 法線画像のパスを設定します。
        // SetNormalTexturePath(path: 法線画像パス)
        void SetNormalTexturePath(std::filesystem::path path)
        {
            m_normalTexture = std::move(path);
        }
        // 表面粗さを設定します。
        // SetRoughness(value: 粗さ係数)
        void SetRoughness(float value) noexcept { m_roughness = value; }
        // 法線マップの影響度を設定します。
        // SetNormalStrength(value: 法線マップ強度)
        void SetNormalStrength(float value) noexcept { m_normalStrength = value; }
        // 金属度を設定します。
        // SetMetallic(value: 金属度係数)
        void SetMetallic(float value) noexcept { m_metallic = value; }
        // 粗さ画像のパスを設定します。
        // SetRoughnessTexturePath(path: 粗さ画像パス)
        void SetRoughnessTexturePath(std::filesystem::path path)
        {
            m_roughnessTexture = std::move(path);
        }
        // 金属度画像のパスを設定します。
        // SetMetallicTexturePath(path: 金属度画像パス)
        void SetMetallicTexturePath(std::filesystem::path path)
        {
            m_metallicTexture = std::move(path);
        }
        // 遮蔽画像のパスを設定します。
        // SetOcclusionTexturePath(path: 遮蔽画像パス)
        void SetOcclusionTexturePath(std::filesystem::path path)
        {
            m_occlusionTexture = std::move(path);
        }
        // 遮蔽画像の影響度を設定します。
        // SetOcclusionStrength(value: 遮蔽強度)
        void SetOcclusionStrength(float value) noexcept
        {
            m_occlusionStrength = value;
        }
        // 発光画像のパスを設定します。
        // SetEmissiveTexturePath(path: 発光画像パス)
        void SetEmissiveTexturePath(std::filesystem::path path)
        {
            m_emissiveTexture = std::move(path);
        }
        // 発光色を設定します。
        // SetEmissiveColor(value: RGB発光色)
        void SetEmissiveColor(DirectX::XMFLOAT3 value) noexcept
        {
            m_emissiveColor = value;
        }
        // 材質上書きの有効状態を設定します。
        // SetMaterialOverrideEnabled(value: 上書き有効状態)
        void SetMaterialOverrideEnabled(bool value) noexcept
        {
            m_materialOverrideEnabled = value;
        }
        // 描画アニメーションを番号で選択します。
        // SetAnimationIndex(index: アニメーション番号)
        void SetAnimationIndex(std::size_t index) noexcept;
        // 現在のアニメーション番号を返します。
        [[nodiscard]] std::size_t AnimationIndex() const noexcept
        {
            return m_animationIndex;
        }
        // モデル内のアニメーション数を返します。
        [[nodiscard]] std::size_t AnimationCount() const noexcept
        {
            return m_animations.size();
        }
        // 番号に対応するアニメーション名を返します。
        // AnimationName(index: アニメーション番号)
        [[nodiscard]] std::string_view AnimationName(
            std::size_t index) const noexcept;
        // 選択中アニメーションの長さを返します。
        [[nodiscard]] float AnimationDuration() const noexcept;
        // 再生速度を設定します。
        // SetAnimationSpeed(value: 再生倍率)
        void SetAnimationSpeed(float value) noexcept { m_animationSpeed = value; }
        // 現在の再生速度を返します。
        [[nodiscard]] float AnimationSpeed() const noexcept
        {
            return m_animationSpeed;
        }
        // 繰り返し再生を設定します。
        // SetAnimationLoop(value: 繰り返し設定)
        void SetAnimationLoop(bool value) noexcept { m_animationLoop = value; }
        // 繰り返し再生の設定を返します。
        [[nodiscard]] bool AnimationLoop() const noexcept
        {
            return m_animationLoop;
        }
        // 開始時の自動再生を設定します。
        // SetAnimationPlayOnStart(value: 自動再生設定)
        void SetAnimationPlayOnStart(bool value) noexcept
        {
            m_animationPlayOnStart = value;
        }
        // 開始時の自動再生設定を返します。
        [[nodiscard]] bool AnimationPlayOnStart() const noexcept
        {
            return m_animationPlayOnStart;
        }
        // 現在選択中のアニメーションを再生します。
        void PlayAnimation() noexcept { m_animationPlaying = true; }
        // アニメーションを一時停止します。
        void PauseAnimation() noexcept { m_animationPlaying = false; }
        // アニメーションを停止して再生位置を戻します。
        void StopAnimation() noexcept;
        // 再生位置を秒単位で設定します。
        // SetAnimationTime(value: 再生位置秒数)
        void SetAnimationTime(float value) noexcept;
        // 現在の再生位置を秒単位で返します。
        [[nodiscard]] float AnimationTime() const noexcept
        {
            return m_animationTime;
        }
        // アニメーションの再生状態を返します。
        [[nodiscard]] bool IsAnimationPlaying() const noexcept
        {
            return m_animationPlaying;
        }

        // Windowsと同じ保存用コンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        { return "ModelRenderer"; }
    private:
        friend class Scene;

        struct ModelNode final
        {
            // 親ノード番号、根は-1
            int parent{ -1 };
            // ノード位置
            DirectX::XMFLOAT3 translation{};
            // ノード回転
            DirectX::XMFLOAT4 rotation{ 0.0f, 0.0f, 0.0f, 1.0f };
            // ノード拡大率
            DirectX::XMFLOAT3 scale{ 1.0f, 1.0f, 1.0f };
            // 行列形式のノード変換
            std::array<float, 16> matrix{};
            // 行列変換が設定されているか
            bool hasMatrix{};
        };

        struct ModelSkin final
        {
            // スキンが参照するジョイント番号
            std::vector<std::size_t> joints;
            // ジョイント別の逆バインド行列
            std::vector<std::array<float, 16>> inverseBindMatrices;
        };

        struct ModelAnimationChannel final
        {
            // アニメーション対象のノード番号
            std::size_t nodeIndex{};
            // 位置、回転、拡大の種別
            std::uint8_t path{};
            // キーフレーム間の補間方式
            std::uint8_t interpolation{};
            // キーフレーム時刻列
            std::vector<float> times;
            // 時刻ごとのアニメーション値
            std::vector<DirectX::XMFLOAT4> values;
            // 曲線補間用の入力接線
            std::vector<DirectX::XMFLOAT4> inTangents;
            // 曲線補間用の出力接線
            std::vector<DirectX::XMFLOAT4> outTangents;
        };

        struct ModelAnimation final
        {
            // アニメーション名
            std::string name;
            // 最大キーフレーム時刻
            float duration{};
            // ノード別アニメーションチャンネル
            std::vector<ModelAnimationChannel> channels;
        };

        struct Part final
        {
            // 部品のメッシュ頂点
            std::vector<ProceduralMeshVertex> vertices;
            // スキニング前の基準頂点
            std::vector<ProceduralMeshVertex> bindVertices;
            // 三角形の頂点番号
            std::vector<std::uint32_t> indices;
            // 頂点ごとのジョイント番号
            std::vector<std::array<std::uint16_t, 4>> joints;
            // 頂点ごとのスキンウェイト
            std::vector<DirectX::XMFLOAT4> weights;
            // 部品の基本色
            DirectX::XMFLOAT4 color{ 1.0f, 1.0f, 1.0f, 1.0f };
            // アルベド画像パス
            std::filesystem::path albedoTexture;
            // 法線画像パス
            std::filesystem::path normalTexture;
            // 金属度粗さ画像パス
            std::filesystem::path metallicRoughnessTexture;
            // 粗さ画像パス
            std::filesystem::path roughnessTexture;
            // 金属度画像パス
            std::filesystem::path metallicTexture;
            // 遮蔽画像パス
            std::filesystem::path occlusionTexture;
            // 発光画像パス
            std::filesystem::path emissiveTexture;
            std::shared_ptr<const std::vector<unsigned char>> albedoImage, normalImage,
                metallicRoughnessImage, occlusionImage, emissiveImage;
            // 表面粗さ係数
            float roughness{ 0.5f };
            // 金属度係数
            float metallic{};
            // 法線マップ強度
            float normalStrength{ 1.0f };
            // 遮蔽画像強度
            float occlusionStrength{ 1.0f };
            // 発光色
            DirectX::XMFLOAT3 emissiveColor{};
            // 誘電体の反射色
            DirectX::XMFLOAT3 dielectricSpecular{
                0.04f, 0.04f, 0.04f };
            // ライティングを無効にするか
            bool unlit{};
            // 両面描画するか
            bool doubleSided{};
            // 半透明合成を使うか
            bool alphaBlended{};
            // アルファ不透明判定の閾値
            float alphaCutoff{ -1.0f };
            // 使用するスキン番号
            int skinIndex{ -1 };
            // 所属するモデルノード番号
            std::size_t meshNodeIndex{};
            // Web側メッシュハンドル
            std::uint32_t webMesh{};
            // Web側アルベド画像ハンドル
            std::uint32_t webTexture{};
            // Web側法線画像ハンドル
            std::uint32_t webNormalTexture{};
            // Web側金属度粗さ画像ハンドル
            std::uint32_t webMetallicRoughnessTexture{};
            // Web側粗さ画像ハンドル
            std::uint32_t webRoughnessTexture{};
            // Web側金属度画像ハンドル
            std::uint32_t webMetallicTexture{};
            // Web側遮蔽画像ハンドル
            std::uint32_t webOcclusionTexture{};
            // Web側発光画像ハンドル
            std::uint32_t webEmissiveTexture{};
            // Web側描画データを再生成するか
            bool dirty{ true };
        };

        // GLTFモデルをポータブル形式で読み込みます。
        [[nodiscard]] bool LoadPortableModel();
        // 経過時間分だけアニメーションを進めます。
        // AdvancePortableAnimation(deltaTime: 経過秒数)
        void AdvancePortableAnimation(float deltaTime);
        // 補間済み姿勢をノードへ適用します。
        void ApplyPortablePose();

        // 読み込んだモデルのパス
        std::filesystem::path m_modelPath;
        // モデル全体に使う色
        DirectX::XMFLOAT4 m_color{ 1.0f, 1.0f, 1.0f, 1.0f };
        // アルベド画像パス
        std::filesystem::path m_albedoTexture;
        // 法線画像パス
        std::filesystem::path m_normalTexture;
        // 粗さ画像パス
        std::filesystem::path m_roughnessTexture;
        // 金属度画像パス
        std::filesystem::path m_metallicTexture;
        // 遮蔽画像パス
        std::filesystem::path m_occlusionTexture;
        // 発光画像パス
        std::filesystem::path m_emissiveTexture;
        // 全体の表面粗さ
        float m_roughness{ 0.5f };
        // 全体の金属度
        float m_metallic{};
        // 全体の法線マップ強度
        float m_normalStrength{ 1.0f };
        // 全体の遮蔽画像強度
        float m_occlusionStrength{ 1.0f };
        // 全体の発光色
        DirectX::XMFLOAT3 m_emissiveColor{};
        // 線描画モード
        [[maybe_unused]] bool m_wireframe{};
        // 全パーツの材質上書き状態
        bool m_materialOverrideEnabled{};
        // モデル読込済み状態
        bool m_loaded{};
        // 描画するモデルパーツ
        std::vector<Part> m_parts;
        // モデル階層のノード
        std::vector<ModelNode> m_nodes;
        // 現在のアニメーション姿勢
        std::vector<ModelNode> m_poseNodes;
        // ノード別ワールド行列
        std::vector<std::array<float, 16>> m_nodeWorldMatrices;
        // スキニング用ジョイント構成
        std::vector<ModelSkin> m_skins;
        // モデル内アニメーション
        std::vector<ModelAnimation> m_animations;
        // 選択中アニメーション番号
        std::size_t m_animationIndex{};
        // アニメーション再生倍率
        float m_animationSpeed{ 1.0f };
        // アニメーション再生位置
        float m_animationTime{};
        // アニメーション繰り返し設定
        bool m_animationLoop{ true };
        // 開始時の自動再生設定
        bool m_animationPlayOnStart{ true };
        // アニメーション再生状態
        bool m_animationPlaying{};
    };

    class MeshCollider3DComponent final : public Component
    {
    public:
        // 衝突レイヤーを設定します。
        // SetLayer(layer: 衝突レイヤー番号)
        void SetLayer(std::uint32_t layer) noexcept { m_layer = layer; }
        // 頂点と三角形インデックスを設定します。
        // SetMesh(vertices: 頂点位置, indices: 三角形頂点番号)
        void SetMesh(
            std::vector<DirectX::XMFLOAT3> vertices,
            std::vector<std::uint32_t> indices)
        {
            m_vertices = std::move(vertices);
            m_indices = std::move(indices);
        }

        // Windowsと同じ保存用コンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        { return "MeshCollider3D"; }
    private:
        friend class Scene;
        // 衝突レイヤー番号
        std::uint32_t m_layer{};
        // ローカル空間のメッシュ頂点
        std::vector<DirectX::XMFLOAT3> m_vertices;
        // 三角形の頂点番号
        std::vector<std::uint32_t> m_indices;
    };

    class BoxCollider3DComponent final : public Component
    {
    public:
        // 箱形状の寸法と位置差を設定します。
        // BoxCollider3DComponent(size: 各軸の寸法, offset: 中心位置の差)
        BoxCollider3DComponent(
            DirectX::XMFLOAT3 size = { 1.0f, 1.0f, 1.0f },
            DirectX::XMFLOAT3 offset = {},
            bool trigger = false,
            std::uint32_t layer = 0,
            std::uint32_t collisionMask = 0xffffffffu)
            : m_size(size),
              m_offset(offset),
              m_layer(layer % 32u),
              m_mask(collisionMask),
              m_trigger(trigger)
        {
        }

        // ローカル箱の全幅を返します。
        [[nodiscard]] const DirectX::XMFLOAT3& Size() const noexcept
        {
            return m_size;
        }
        // ローカル箱の全幅を設定します(size: XYZ全幅)。
        void SetSize(const DirectX::XMFLOAT3& size) noexcept { m_size = size; }
        // ローカル中心位置を返します。
        [[nodiscard]] const DirectX::XMFLOAT3& Offset() const noexcept
        {
            return m_offset;
        }
        // ローカル中心位置を設定します(offset: XYZ中心位置)。
        void SetOffset(const DirectX::XMFLOAT3& offset) noexcept
        {
            m_offset = offset;
        }
        // Trigger判定を返します。
        [[nodiscard]] bool IsTrigger() const noexcept { return m_trigger; }
        // 衝突レイヤー番号を返します。
        [[nodiscard]] std::uint32_t Layer() const noexcept { return m_layer; }
        // 接触対象レイヤーマスクを返します。
        [[nodiscard]] std::uint32_t CollisionMask() const noexcept
        {
            return m_mask;
        }
        // 衝突レイヤーを設定します。
        // SetLayer(layer: 衝突レイヤー番号)
        void SetLayer(std::uint32_t layer) noexcept { m_layer = layer % 32u; }
        // 接触対象レイヤーを設定します。
        // SetCollisionMask(mask: 対象レイヤーマスク)
        void SetCollisionMask(std::uint32_t mask) noexcept { m_mask = mask; }
        // Trigger判定を設定します。
        // SetTrigger(value: Trigger有効状態)
        void SetTrigger(bool value) noexcept { m_trigger = value; }
        // 保存用コンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "BoxCollider3D";
        }

    private:
        friend class Scene;
        // 各軸の箱寸法
        DirectX::XMFLOAT3 m_size{};
        // GameObject原点からの中心位置差
        DirectX::XMFLOAT3 m_offset{};
        // 衝突レイヤー番号
        std::uint32_t m_layer{};
        // 接触対象レイヤー
        std::uint32_t m_mask{ 0xffffffffu };
        // Trigger判定の有効状態
        bool m_trigger{};
    };

    class RigidbodyComponent final : public Component
    {
    public:
        // 物理演算を固定する設定を変更します。
        // SetKinematic(value: 固定状態)
        void SetKinematic(bool value) noexcept { m_kinematic = value; }
        // 固定物体として扱うか返します。
        [[nodiscard]] bool IsKinematic() const noexcept { return m_kinematic; }
        // 重力適用を切り替えます。
        // SetUseGravity(value: 重力適用状態)
        void SetUseGravity(bool value) noexcept { m_useGravity = value; }
        // 重力を使うか返します。
        [[nodiscard]] bool UsesGravity() const noexcept { return m_useGravity; }
        // 移動速度を設定します。
        // SetVelocity(value: 速度ベクトル)
        void SetVelocity(DirectX::XMFLOAT3 value) noexcept { m_velocity = value; }
        // 現在の移動速度を返します。
        [[nodiscard]] const DirectX::XMFLOAT3& Velocity() const noexcept
        {
            return m_velocity;
        }
        // 保存用コンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "Rigidbody";
        }

    private:
        friend class Scene;
        // 固定物体として扱うか
        bool m_kinematic{};
        // 重力適用状態
        bool m_useGravity{ true };
        // 現在の移動速度
        DirectX::XMFLOAT3 m_velocity{};
    };

    class CameraComponent final : public Component
    {
    public:
        // 透視カメラの視野角と深度範囲を設定します。
        CameraComponent(
            float verticalFieldOfView = DirectX::XM_PI / 4.0f,
            float nearPlane = 0.1f,
            float farPlane = 1000.0f) noexcept
            : m_fieldOfView(verticalFieldOfView),
              m_nearPlane(nearPlane),
              m_farPlane(farPlane)
        {
        }

        // 縦視野角をラジアンで返します。
        [[nodiscard]] float VerticalFieldOfView() const noexcept
        {
            return m_fieldOfView;
        }
        // 垂直視野角をラジアンで設定します。
        // SetVerticalFieldOfView(value: 垂直視野角ラジアン)
        void SetVerticalFieldOfView(float value) noexcept { m_fieldOfView = value; }
        // 近クリップ距離を返します。
        [[nodiscard]] float NearPlane() const noexcept { return m_nearPlane; }
        // 近クリップ距離を設定します。
        // SetNearPlane(value: 近距離)
        void SetNearPlane(float value) noexcept { m_nearPlane = value; }
        // 遠クリップ距離を返します。
        [[nodiscard]] float FarPlane() const noexcept { return m_farPlane; }
        // 遠クリップ距離を設定します。
        // SetFarPlane(value: 遠距離)
        void SetFarPlane(float value) noexcept { m_farPlane = value; }

        // Windowsと同じ保存用コンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        { return "Camera"; }
    private:
        friend class Scene;
        // 垂直視野角ラジアン
        float m_fieldOfView{ 1.0471976f };
        // 近クリップ距離
        float m_nearPlane{ 0.1f };
        // 遠クリップ距離
        float m_farPlane{ 1500.0f };
    };

    class RotatorComponent final : public Component
    {
    public:
        // 毎秒の回転量を設定します。
        // RotatorComponent(angularVelocity: 毎秒のXYZ回転量)
        explicit RotatorComponent(
            DirectX::XMFLOAT3 angularVelocity = { 0.0f, 1.0f, 0.0f }) noexcept
            : m_angularVelocity(angularVelocity)
        {
        }

        // 毎秒の回転量を返します。
        [[nodiscard]] const DirectX::XMFLOAT3& AngularVelocity() const noexcept
        {
            return m_angularVelocity;
        }
        // 毎秒の回転量を設定します。
        // SetAngularVelocity(value: 毎秒のXYZ回転量)
        void SetAngularVelocity(DirectX::XMFLOAT3 value) noexcept
        {
            m_angularVelocity = value;
        }

        // Windowsと同じ保存用コンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        { return "Rotator"; }
    private:
        friend class Scene;
        // 毎秒のXYZ回転量
        DirectX::XMFLOAT3 m_angularVelocity{};
    };

    class InputMoverComponent final : public Component
    {
    public:
        // 水平・垂直入力名と毎秒速度を設定します。
        // InputMoverComponent(horizontalAction: 横入力名, verticalAction: 縦入力名, speed: 毎秒の移動量)
        InputMoverComponent(
            std::string horizontalAction = "MoveHorizontal",
            std::string verticalAction = "MoveVertical",
            float speed = 3.0f)
            : m_horizontalAction(std::move(horizontalAction)),
              m_verticalAction(std::move(verticalAction)),
              m_speed(std::max(speed, 0.0f))
        {
        }

        // 水平移動の入力アクションを設定します。
        // SetHorizontalAction(value: 入力アクション名)
        void SetHorizontalAction(std::string value)
        {
            m_horizontalAction = std::move(value);
        }
        // 水平移動の入力名を返します。
        [[nodiscard]] const std::string& HorizontalAction() const noexcept
        {
            return m_horizontalAction;
        }
        // 垂直移動の入力アクションを設定します。
        // SetVerticalAction(value: 入力アクション名)
        void SetVerticalAction(std::string value)
        {
            m_verticalAction = std::move(value);
        }
        // 垂直移動の入力名を返します。
        [[nodiscard]] const std::string& VerticalAction() const noexcept
        {
            return m_verticalAction;
        }
        // 移動速度を0以上で設定します。
        // SetSpeed(value: 毎秒の移動量)
        void SetSpeed(float value) noexcept { m_speed = std::max(value, 0.0f); }
        // 現在の移動速度を返します。
        [[nodiscard]] float Speed() const noexcept { return m_speed; }

        // Windowsと同じ保存用コンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        { return "InputMover"; }
    private:
        friend class Scene;
        // 水平移動に使う入力名
        std::string m_horizontalAction;
        // 垂直移動に使う入力名
        std::string m_verticalAction;
        // 毎秒の移動量
        float m_speed{ 3.0f };
    };

    class RenderCullingComponent final : public Component
    {
    public:
        // 表示維持設定と画面余白を指定します。
        // RenderCullingComponent(alwaysVisible: 常時表示, cullingMargin: 画面外の余白)
        RenderCullingComponent(
            bool alwaysVisible = false,
            float cullingMargin = 0.0f) noexcept
            : m_alwaysVisible(alwaysVisible),
              m_cullingMargin(std::max(cullingMargin, 0.0f))
        {
        }

        // カリングを無効にする設定を切り替えます。
        // SetAlwaysVisible(value: 常時表示設定)
        void SetAlwaysVisible(bool value) noexcept { m_alwaysVisible = value; }
        // 常時表示設定を返します。
        [[nodiscard]] bool AlwaysVisible() const noexcept
        {
            return m_alwaysVisible;
        }
        // カリング範囲の余白を設定します。
        // SetCullingMargin(value: 画面外の余白)
        void SetCullingMargin(float value) noexcept
        {
            m_cullingMargin = std::max(value, 0.0f);
        }
        // カリング範囲の余白を返します。
        [[nodiscard]] float CullingMargin() const noexcept
        {
            return m_cullingMargin;
        }

        // Windowsと同じ保存用コンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        { return "RenderCulling"; }
    private:
        // 常時表示設定
        [[maybe_unused]] bool m_alwaysVisible{};
        // カリング判定の追加余白
        [[maybe_unused]] float m_cullingMargin{};
    };

    class ParticleSystemComponent final : public Component
    {
    public:
        // 粒子上限、分布、色、形状を指定します。
        // ParticleSystemComponent(capacity: 最大粒子数, emissionRate: 毎秒の放出数, lifetime: 寿命範囲, speed: 速度範囲, size: 寸法範囲, startColor: 開始色, endColor: 終了色, shape: 発生形状, texture: 画像パス)
        ParticleSystemComponent(
            std::uint32_t capacity,
            float emissionRate,
            DirectX::XMFLOAT2 lifetime,
            DirectX::XMFLOAT2 speed,
            DirectX::XMFLOAT2 size,
            DirectX::XMFLOAT4 startColor,
            DirectX::XMFLOAT4 endColor,
            ParticleEmitterShape shape,
            std::filesystem::path texture = {});

        // 粒子へ加える重力を設定します。
        // SetGravity(gravity: 重力加速度)
        void SetGravity(DirectX::XMFLOAT3 gravity) noexcept { m_gravity = gravity; }
        // 終了時の寸法倍率を設定します。
        // SetEndSizeMultiplier(value: 寸法倍率)
        void SetEndSizeMultiplier(float value) noexcept
        {
            m_endSizeMultiplier = value;
        }
        // 粒子の描画方式を設定します。
        // SetRenderMode(value: 描画方式)
        void SetRenderMode(ParticleRenderMode value) noexcept
        {
            m_renderMode = value;
        }
        // 発生領域の寸法を設定します。
        // SetEmitterSize(value: 発生領域寸法)
        void SetEmitterSize(DirectX::XMFLOAT3 value) noexcept
        {
            m_emitterSize = value;
        }
        // 円錐形発生の角度を設定します。
        // SetConeAngle(value: 発生角度ラジアン)
        void SetConeAngle(float value) noexcept { m_coneAngle = value; }
        // 再生時間を設定します。
        // SetDuration(value: 再生時間秒数)
        void SetDuration(float value) noexcept { m_duration = value; }
        // 粒子放出の繰り返しを切り替えます。
        // SetLooping(value: 繰り返し設定)
        void SetLooping(bool value) noexcept { m_looping = value; }
        // 初回の粒子放出を開始します。
        // SetPlayOnStart(value: 再生開始設定)
        void SetPlayOnStart(bool value) noexcept { m_playing = value; }
        // 加算合成を切り替えます。
        // SetAdditive(value: 加算合成設定)
        void SetAdditive(bool value) noexcept { m_additive = value; }
        // 指定数の粒子を即時放出します。
        // Emit(count: 放出数)
        void Emit(int count);
        // 放出を停止し、必要に応じて既存粒子を消します。
        // Stop(clearParticles: 既存粒子の消去設定)
        void Stop(bool clearParticles = false);

        // Windowsと同じ保存用コンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        { return "ParticleSystem"; }
    private:
        friend class Scene;
        struct Particle final
        {
            // ワールド位置
            DirectX::XMFLOAT3 position{};
            // 移動速度
            DirectX::XMFLOAT3 velocity{};
            // 発生からの経過秒数
            float age{};
            // 粒子の寿命秒数
            float lifetime{ 1.0f };
            // 粒子寸法
            float size{ 0.5f };
            // 描画面内の回転角
            float rotation{};
        };

        // 同時に保持する最大粒子数
        std::uint32_t m_capacity{};
        // 毎秒の自動放出数
        float m_emissionRate{};
        // 粒子寿命の最小・最大秒数
        DirectX::XMFLOAT2 m_lifetime{};
        // 粒子速度の最小・最大値
        DirectX::XMFLOAT2 m_speed{};
        // 粒子寸法の最小・最大値
        DirectX::XMFLOAT2 m_size{};
        // 発生時の色
        DirectX::XMFLOAT4 m_startColor{};
        // 寿命終了時の色
        DirectX::XMFLOAT4 m_endColor{};
        // 粒子発生形状
        ParticleEmitterShape m_shape{};
        // 粒子描画方式
        ParticleRenderMode m_renderMode{};
        // 粒子画像のパス
        std::filesystem::path m_texture;
        // 粒子へ加える重力
        DirectX::XMFLOAT3 m_gravity{};
        // 発生領域寸法
        DirectX::XMFLOAT3 m_emitterSize{ 1.0f, 1.0f, 1.0f };
        // 寿命終了時の寸法倍率
        float m_endSizeMultiplier{ 1.0f };
        // 円錐形発生の角度
        float m_coneAngle{ 0.4363323f };
        // 放出継続時間
        float m_duration{ 5.0f };
        // 再生開始からの経過時間
        float m_emittingTime{};
        // 放出を繰り返すか
        bool m_looping{ true };
        // 加算合成を使うか
        bool m_additive{ true };
        // 現在生存している粒子
        std::vector<Particle> m_particles;
        // 次回放出までの累積量
        float m_emissionAccumulator{};
        // 自動放出が有効か
        bool m_playing{ true };
        // 粒子生成に使う乱数状態
        std::uint32_t m_randomState{ 0x54524944u };
        // Web側粒子メッシュハンドル
        std::uint32_t m_webMesh{};
        // Web側粒子画像ハンドル
        std::uint32_t m_webTexture{};
    };

    class AudioSourceComponent final : public Component
    {
    public:
        // 再生音声と空間化・ループ条件を初期化します。
        // AudioSourceComponent(path: 音声パス, volume: 音量, pitch: 再生ピッチ, pan: 左右定位, loop: ループ設定, playOnStart: 自動再生, spatial: 空間化設定, minimumDistance: 最小距離, maximumDistance: 最大距離)
        AudioSourceComponent(
            std::filesystem::path path = {},
            float volume = 1.0f,
            float pitch = 0.0f,
            float pan = 0.0f,
            bool loop = false,
            bool playOnStart = false,
            bool spatial = false,
            float minimumDistance = 1.0f,
            float maximumDistance = 20.0f)
            : m_path(std::move(path)),
              m_volume(std::clamp(volume, 0.0f, 1.0f)),
              m_pitch(std::clamp(pitch, -1.0f, 1.0f)),
              m_pan(std::clamp(pan, -1.0f, 1.0f)), m_loop(loop),
              m_playOnStart(playOnStart), m_spatial(spatial),
              m_minimumDistance(std::max(minimumDistance, 0.01f)),
              m_maximumDistance(std::max(
                  maximumDistance, m_minimumDistance + 0.01f))
        {
        }

        // 音源パスを取得します。
        [[nodiscard]] const std::filesystem::path& AudioPath() const noexcept
        {
            return m_path;
        }
        // 音源パスを変更し、ループ再生中なら新しい音源を再生します。
        void SetAudioPath(std::filesystem::path path)
        {
            const bool wasPlaying = m_handle != 0;
            if (wasPlaying) Stop();
            m_path = std::move(path);
            if (wasPlaying) Play();
        }
        // 音量倍率を返します。
        [[nodiscard]] float Volume() const noexcept { return m_volume; }
        // 出力先の音声バスを設定します。
        // SetBus(value: 音声バス)
        void SetBus(AudioBus value) noexcept { m_bus = value; }
        // 現在の出力先を返します。
        [[nodiscard]] AudioBus Bus() const noexcept { return m_bus; }
        // ループ再生するか返します。
        [[nodiscard]] bool Loop() const noexcept { return m_loop; }
        // ループ再生を設定します。
        // SetLoop(value: ループ設定)
        void SetLoop(bool value)
        {
            if (m_loop == value) return;
            const bool wasPlaying = m_handle != 0;
            if (wasPlaying) Stop();
            m_loop = value;
            if (wasPlaying) Play();
        }
        // 再生pitchを返します。
        [[nodiscard]] float Pitch() const noexcept { return m_pitch; }
        // 再生ピッチを設定します。
        // SetPitch(value: 再生ピッチ)
        void SetPitch(float value);
        // 左右定位を返します。
        [[nodiscard]] float Pan() const noexcept { return m_pan; }
        // 音量倍率を設定します。
        // SetVolume(value: 音量倍率)
        void SetVolume(float value);
        // 左右定位を設定します。
        // SetPan(value: 左右定位)
        void SetPan(float value);
        // 初回更新時に自動再生するか返します。
        [[nodiscard]] bool PlayOnStart() const noexcept
        {
            return m_playOnStart;
        }
        // 初回更新時の自動再生を設定します。
        void SetPlayOnStart(bool value) noexcept { m_playOnStart = value; }
        // 距離減衰を使うか返します。
        [[nodiscard]] bool IsSpatial() const noexcept { return m_spatial; }
        // 距離減衰を使う空間音声を切り替えます。
        // SetSpatial(value: 空間化設定)
        void SetSpatial(bool value)
        {
            if (m_spatial == value) return;
            const bool wasPlaying = m_handle != 0;
            if (wasPlaying) Stop();
            m_spatial = value;
            if (wasPlaying) Play();
        }
        // 空間音声の最小距離を返します。
        [[nodiscard]] float MinimumDistance() const noexcept
        {
            return m_minimumDistance;
        }
        // 空間音声の最小距離を設定します。
        // SetMinimumDistance(value: 最小距離)
        void SetMinimumDistance(float value);
        // 空間音声の最大距離を返します。
        [[nodiscard]] float MaximumDistance() const noexcept
        {
            return m_maximumDistance;
        }
        // 空間音声の最大距離を設定します。
        // SetMaximumDistance(value: 最大距離)
        void SetMaximumDistance(float value);
        // 音声を再生します。
        void Play();
        // 音声を一度だけ再生します。
        void PlayOneShot();
        // 音声再生を停止します。
        void Stop();
        // 保存用コンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "AudioSource";
        }

    private:
        friend class Scene;
        // 再生する音声ファイル
        std::filesystem::path m_path;
        // 音量倍率
        float m_volume{ 1.0f };
        // 再生ピッチ
        float m_pitch{};
        // 左右定位
        float m_pan{};
        // ループ再生設定
        bool m_loop{};
        // 開始時の自動再生設定
        bool m_playOnStart{};
        // 開始時の自動再生を実行済みか
        bool m_playOnStartConsumed{};
        // 距離減衰を使う設定
        bool m_spatial{};
        // 空間音声の最小距離
        float m_minimumDistance{ 1.0f };
        // 空間音声の最大距離
        float m_maximumDistance{ 20.0f };
        // 音声出力先
        AudioBus m_bus{ AudioBus::Effects };
        // Web側音源ハンドル
        [[maybe_unused]] std::uint32_t m_handle{};
    };

    struct UIRect final
    {
        // 矩形の左上座標
        DirectX::XMFLOAT2 minimum{};
        // 矩形の右下座標
        DirectX::XMFLOAT2 maximum{};

        // 矩形の幅と高さを返します。
        [[nodiscard]] DirectX::XMFLOAT2 Size() const noexcept
        {
            return {
                maximum.x - minimum.x,
                maximum.y - minimum.y
            };
        }
    };

    class UICanvasComponent final : public Component
    {
    public:
        explicit UICanvasComponent(DirectX::XMFLOAT2 resolution = {1280.0f, 720.0f}, float match = 0.5f) noexcept
        { SetReferenceResolution(resolution); SetMatchWidthOrHeight(match); }
        void SetReferenceResolution(const DirectX::XMFLOAT2& value) noexcept
        { m_resolution = {std::max(value.x, 1.0f), std::max(value.y, 1.0f)}; }
        void SetMatchWidthOrHeight(float value) noexcept { m_match = std::clamp(value, 0.0f, 1.0f); }
        const DirectX::XMFLOAT2& ReferenceResolution() const noexcept { return m_resolution; }
        float MatchWidthOrHeight() const noexcept { return m_match; }
        float ScaleFactor(float width, float height) const noexcept
        { return std::exp2(std::lerp(std::log2(std::max(width, 1.0f) / m_resolution.x),
            std::log2(std::max(height, 1.0f) / m_resolution.y), m_match)); }
        // Windowsと同じ保存用コンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        { return "UICanvas"; }
    private:
        DirectX::XMFLOAT2 m_resolution;
        float m_match{};
    };

    class PortableUIVisualComponent : public Component
    {
    public:
        explicit PortableUIVisualComponent(std::filesystem::path texture = {}, DirectX::XMFLOAT4 color = {1,1,1,1})
            : m_texture(std::move(texture)), m_color(color) {}
        void SetTexturePath(std::filesystem::path value) { m_texture = std::move(value); }
        const std::filesystem::path& TexturePath() const noexcept { return m_texture; }
        void SetColor(const DirectX::XMFLOAT4& value) noexcept { m_color = value; }
        const DirectX::XMFLOAT4& Color() const noexcept { return m_color; }
        void SetFallbackSize(const DirectX::XMFLOAT2& value) noexcept
        { m_size = {std::max(value.x, 1.0f), std::max(value.y, 1.0f)}; }
        const DirectX::XMFLOAT2& FallbackSize() const noexcept { return m_size; }
        void SetSortOrder(int value) noexcept { m_sortOrder = value; }
        int SortOrder() const noexcept { return m_sortOrder; }
    private:
        friend class Scene;
        std::filesystem::path m_texture;
        DirectX::XMFLOAT4 m_color;
        DirectX::XMFLOAT2 m_size{100.0f, 100.0f};
        int m_sortOrder{};
    };

    class UIImageComponent final : public PortableUIVisualComponent
    {
    public:
        using PortableUIVisualComponent::PortableUIVisualComponent;
        // Windowsと同じ保存用コンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        { return "UIImage"; }
    };

    class UIButtonComponent final : public PortableUIVisualComponent
    {
    public:
        explicit UIButtonComponent(std::string label = "ボタン", DirectX::XMFLOAT2 size = {220,56}, std::filesystem::path texture = {})
            : PortableUIVisualComponent(std::move(texture)), m_label(std::move(label)) { SetFallbackSize(size); }
        void SetLabel(std::string value) { m_label = std::move(value); }
        const std::string& Label() const noexcept { return m_label; }
        void SetFontFamily(std::string value) { m_fontFamily = std::move(value); }
        const std::string& FontFamily() const noexcept { return m_fontFamily; }
        void SetFontSize(float value) noexcept { m_fontSize = std::max(value, 1.0f); }
        float FontSize() const noexcept { return m_fontSize; }
        void SetNormalColor(const DirectX::XMFLOAT4& value) noexcept { m_normal = value; }
        void SetHoveredColor(const DirectX::XMFLOAT4& value) noexcept { m_hover = value; }
        void SetPressedColor(const DirectX::XMFLOAT4& value) noexcept { m_press = value; }
        void SetDisabledColor(const DirectX::XMFLOAT4& value) noexcept { m_disabled = value; }
        const DirectX::XMFLOAT4& NormalColor() const noexcept { return m_normal; }
        const DirectX::XMFLOAT4& HoveredColor() const noexcept { return m_hover; }
        const DirectX::XMFLOAT4& PressedColor() const noexcept { return m_press; }
        const DirectX::XMFLOAT4& DisabledColor() const noexcept { return m_disabled; }
        void SetTextColor(const DirectX::XMFLOAT4& value) noexcept { m_textColor = value; }
        const DirectX::XMFLOAT4& TextColor() const noexcept { return m_textColor; }
        void SetInteractable(bool value) noexcept { m_interactable = value; if (!value) m_hovered = m_pressed = m_clicked = m_focused = false; }
        bool Interactable() const noexcept { return m_interactable; }
        void SetNavigationEnabled(bool value) noexcept { m_navigationEnabled = value; if (!value) m_focused = false; }
        bool NavigationEnabled() const noexcept { return m_navigationEnabled; }
        bool IsFocused() const noexcept { return m_focused; }
        void SetClickEventName(std::string value)
        { m_clickEventName = std::move(value); }
        const std::string& ClickEventName() const noexcept
        { return m_clickEventName; }
        void SetCircularHitArea(bool value) noexcept { m_circular = value; }
        bool CircularHitArea() const noexcept { return m_circular; }
        bool IsHovered() const noexcept { return m_hovered; }
        bool IsPressed() const noexcept { return m_pressed; }
        bool WasClicked() const noexcept { return m_clicked; }
        bool ConsumeClick() noexcept { const bool clicked = m_clicked; m_clicked = false; return clicked; }
        // 主シーンの切替先を設定します(scenePath: assets内のScene path)。
        void SetTargetScene(std::filesystem::path path)
        { m_targetScene = std::move(path); }
        // 主シーンの切替先を返します。
        const std::filesystem::path& TargetScene() const noexcept
        { return m_targetScene; }
        // クリック時に現在の主シーンを再読込するか設定します。
        void SetReloadCurrentScene(bool value) noexcept
        { m_reloadCurrentScene = value; }
        // 現在の主シーンを再読込する設定か返します。
        bool ReloadCurrentScene() const noexcept
        { return m_reloadCurrentScene; }
        // Windowsと同じ保存用コンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        { return "UIButton"; }
    private:
        friend class Scene;
        std::string m_label, m_fontFamily{"Yu Gothic UI"};
        std::string m_clickEventName;
        std::filesystem::path m_targetScene;
        float m_fontSize{24.0f};
        DirectX::XMFLOAT4 m_normal{0.08f,0.28f,0.52f,0.96f}, m_hover{0.12f,0.42f,0.76f,1.0f},
            m_press{0.04f,0.20f,0.40f,1.0f}, m_disabled{0.18f,0.20f,0.24f,0.65f}, m_textColor{1,1,1,1};
        bool m_interactable{true}, m_circular{}, m_hovered{}, m_pressed{}, m_clicked{};
        bool m_navigationEnabled{true}, m_focused{};
        bool m_reloadCurrentScene{};
    };

    class UIRectTransformComponent final : public Component
    {
    public:
        // アンカー、基準点、位置、寸法を設定します。
        // UIRectTransformComponent(anchorMin: 最小アンカー, anchorMax: 最大アンカー, pivot: 基準点, anchoredPosition: アンカーからの位置, sizeDelta: アンカー寸法差)
        explicit UIRectTransformComponent(
            DirectX::XMFLOAT2 anchorMin = { 0.5f, 0.5f },
            DirectX::XMFLOAT2 anchorMax = { 0.5f, 0.5f },
            DirectX::XMFLOAT2 pivot = { 0.5f, 0.5f },
            DirectX::XMFLOAT2 anchoredPosition = {},
            DirectX::XMFLOAT2 sizeDelta = { 220.0f, 56.0f }) noexcept;

        // 最小アンカー座標を設定します。
        // SetAnchorMin(value: 最小アンカー)
        void SetAnchorMin(DirectX::XMFLOAT2 value) noexcept;
        // 最大アンカー座標を設定します。
        // SetAnchorMax(value: 最大アンカー)
        void SetAnchorMax(DirectX::XMFLOAT2 value) noexcept;
        // UI要素の基準点を設定します。
        // SetPivot(value: 基準点)
        void SetPivot(DirectX::XMFLOAT2 value) noexcept;
        // アンカーからの位置を設定します。
        // SetAnchoredPosition(value: 相対位置)
        void SetAnchoredPosition(DirectX::XMFLOAT2 value) noexcept
        {
            m_anchoredPosition = value;
        }
        // アンカー寸法との差を設定します。
        // SetSizeDelta(value: 寸法差)
        void SetSizeDelta(DirectX::XMFLOAT2 value) noexcept
        {
            m_sizeDelta = value;
        }
        // 最小アンカー座標を返します。
        [[nodiscard]] const DirectX::XMFLOAT2& AnchorMin() const noexcept
        { return m_anchorMin; }
        // 最大アンカー座標を返します。
        [[nodiscard]] const DirectX::XMFLOAT2& AnchorMax() const noexcept
        { return m_anchorMax; }
        // UI要素の基準点を返します。
        [[nodiscard]] const DirectX::XMFLOAT2& Pivot() const noexcept
        { return m_pivot; }
        // アンカーからの位置を返します。
        [[nodiscard]] const DirectX::XMFLOAT2& AnchoredPosition() const noexcept
        { return m_anchoredPosition; }
        // アンカー寸法との差を返します。
        [[nodiscard]] const DirectX::XMFLOAT2& SizeDelta() const noexcept
        { return m_sizeDelta; }
        // ビューポート寸法からUI矩形を計算します。
        // Resolve(viewportWidth: ビューポート幅, viewportHeight: ビューポート高さ)
        [[nodiscard]] UIRect Resolve(
            float viewportWidth,
            float viewportHeight) const noexcept;
        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        { return "UIRectTransform"; }

    private:
        // ビューポート基準の最小アンカー
        DirectX::XMFLOAT2 m_anchorMin;
        // ビューポート基準の最大アンカー
        DirectX::XMFLOAT2 m_anchorMax;
        // UI要素の基準点
        DirectX::XMFLOAT2 m_pivot;
        // アンカーからの位置差
        DirectX::XMFLOAT2 m_anchoredPosition;
        // アンカー寸法からの増減
        DirectX::XMFLOAT2 m_sizeDelta;
    };

    class TransformAnimatorComponent final : public Component
    {
    public:
        // クリップと再生条件を指定して生成します。
        // TransformAnimatorComponent(clipPath: クリップパス, speed: 再生倍率, loop: ループ設定, playOnStart: 自動再生)
        explicit TransformAnimatorComponent(
            std::filesystem::path clipPath = {},
            float speed = 1.0f,
            bool loop = true,
            bool playOnStart = true)
            : m_clipPath(std::move(clipPath)),
              m_speed(speed),
              m_loop(loop),
              m_playOnStart(playOnStart)
        {
        }

        // 再生速度倍率を設定します。
        // SetSpeed(value: 再生倍率)
        void SetSpeed(float value) noexcept { m_speed = value; }
        // 現在の再生速度倍率を返します。
        [[nodiscard]] float Speed() const noexcept { return m_speed; }
        // ループ再生を設定します。
        // SetLoop(value: ループ設定)
        void SetLoop(bool value) noexcept { m_loop = value; }
        // ループ再生設定を返します。
        [[nodiscard]] bool Loop() const noexcept { return m_loop; }
        // キーフレームがあれば再生を開始します。
        void Play() noexcept { m_playing = !m_keyframes.empty(); }
        // 再生位置を保って一時停止します。
        void Pause() noexcept { m_playing = false; }
        // 再生位置を先頭へ戻して停止します。
        void Stop() noexcept
        {
            m_playing = false;
            m_time = 0.0f;
        }
        // 再生位置を秒単位で設定します。
        // SetTime(value: 再生位置秒数)
        void SetTime(float value) noexcept;
        // 現在の再生位置を返します。
        [[nodiscard]] float Time() const noexcept { return m_time; }
        // クリップ長を秒単位で返します。
        [[nodiscard]] float Duration() const noexcept { return m_duration; }
        // 再生状態を返します。
        [[nodiscard]] bool IsPlaying() const noexcept { return m_playing; }

        // Windowsと同じ保存用コンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        { return "TransformAnimator"; }
    private:
        friend class Scene;
        struct Keyframe final
        {
            // クリップ開始からの時刻
            float time{};
            // キーフレーム位置
            DirectX::XMFLOAT3 position{};
            // キーフレーム回転
            DirectX::XMFLOAT3 rotation{};
            // キーフレーム拡大率
            DirectX::XMFLOAT3 scale{ 1.0f, 1.0f, 1.0f };
        };

        // クリップをポータブル形式で読み込みます。
        bool LoadPortableClip();
        // 経過時間分だけ再生位置を進めます。
        // AdvancePortableAnimation(deltaTime: 経過秒数)
        void AdvancePortableAnimation(float deltaTime);
        // 補間したTransformを反映します。
        void ApplyPortableSample();

        // アニメーションクリップのパス
        std::filesystem::path m_clipPath;
        // 再生速度倍率
        float m_speed{ 1.0f };
        // ループ設定
        bool m_loop{ true };
        // 開始時の自動再生設定
        bool m_playOnStart{ true };
        // 現在の再生状態
        bool m_playing{};
        // クリップ再生位置
        float m_time{};
        // クリップ全長
        float m_duration{};
        // 時刻順のキーフレーム
        std::vector<Keyframe> m_keyframes;
    };

    class TextRendererComponent final : public Component
    {
    public:
        // 文字列、書体、寸法、配置条件を指定します。
        // TextRendererComponent(text: 表示文字列, fontFamily: 書体名, fontSize: 文字サイズ, color: RGBA色, bounds: 描画範囲, wordWrap: 折返し設定, horizontal: 横揃え, vertical: 縦揃え)
        TextRendererComponent(
            std::string text = "日本語テキスト",
            std::string fontFamily = "Yu Gothic UI",
            float fontSize = 32.0f,
            DirectX::XMFLOAT4 color = {1, 1, 1, 1},
            DirectX::XMFLOAT2 bounds = {0, 0},
            bool wordWrap = false,
            TextHorizontalAlignment horizontal = TextHorizontalAlignment::Left,
            TextVerticalAlignment vertical = TextVerticalAlignment::Top);

        // 描画順を設定します。
        // SetSortOrder(value: 描画順)
        void SetSortOrder(int value) noexcept { m_sortOrder = value; }
        // 設定した描画順を返します。
        [[nodiscard]] int SortOrder() const noexcept { return m_sortOrder; }
        // 表示文字列を設定します。
        // SetText(value: 表示文字列)
        void SetText(std::string value) { m_text = std::move(value); }
        // 表示文字列を返します。
        [[nodiscard]] const std::string& Text() const noexcept { return m_text; }
        // 書体を設定します。
        void SetFontFamily(std::string value) { m_fontFamily = std::move(value); }
        // 書体を返します。
        [[nodiscard]] const std::string& FontFamily() const noexcept { return m_fontFamily; }
        // 表示色を設定します。
        // SetColor(value: RGBA色)
        void SetColor(DirectX::XMFLOAT4 value) noexcept { m_color = value; }
        // 表示色を返します。
        [[nodiscard]] const DirectX::XMFLOAT4& Color() const noexcept { return m_color; }
        // 文字サイズを設定します。
        // SetFontSize(value: 文字サイズ)
        void SetFontSize(float value) noexcept { m_fontSize = std::max(value, 1.0f); }
        // 文字サイズを返します。
        [[nodiscard]] float FontSize() const noexcept { return m_fontSize; }
        // フォントアセットを設定します。
        // SetFontAsset(value: フォントアセットパス)
        void SetFontAsset(std::filesystem::path value)
        {
            m_fontAsset = std::move(value);
        }
        // 折返し計算に使う幅と高さを設定します。
        // SetLayoutSize(value: 描画範囲寸法)
        void SetLayoutSize(DirectX::XMFLOAT2 value) noexcept
        {
            m_bounds = {
                std::clamp(value.x, 0.0f, 4096.0f),
                std::clamp(value.y, 0.0f, 4096.0f)
            };
        }
        // 描画範囲寸法を返します。
        [[nodiscard]] const DirectX::XMFLOAT2& LayoutSize() const noexcept
        { return m_bounds; }
        // 折返しを設定します。
        void SetWordWrap(bool value) noexcept { m_wordWrap = value; }
        // 折返し設定を返します。
        [[nodiscard]] bool WordWrap() const noexcept { return m_wordWrap; }
        // 横揃えを設定します。
        void SetHorizontalAlignment(TextHorizontalAlignment value) noexcept
        { m_horizontal = value; }
        // 横揃え設定を返します。
        [[nodiscard]] TextHorizontalAlignment HorizontalAlignment() const noexcept
        { return m_horizontal; }
        // 縦揃えを設定します。
        void SetVerticalAlignment(TextVerticalAlignment value) noexcept
        { m_vertical = value; }
        // 縦揃え設定を返します。
        [[nodiscard]] TextVerticalAlignment VerticalAlignment() const noexcept
        { return m_vertical; }

        // Windowsと同じ保存用コンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        { return "TextRenderer"; }
    private:
        friend class Scene;
        // 描画する文字列
        std::string m_text;
        // 使用するフォント書体
        std::string m_fontFamily;
        // フォントアセットのパス
        std::filesystem::path m_fontAsset;
        // 文字の高さ
        float m_fontSize{};
        // 文字列の描画色
        DirectX::XMFLOAT4 m_color{};
        // レイアウト範囲の幅と高さ
        DirectX::XMFLOAT2 m_bounds{};
        // 単語単位で折り返すか
        bool m_wordWrap{};
        // 横方向の揃え方
        TextHorizontalAlignment m_horizontal{};
        // 縦方向の揃え方
        TextVerticalAlignment m_vertical{};
        // UI内の描画順
        int m_sortOrder{};
    };

    class SpriteMaskComponent final : public Component
    {
    public:
        // マスク形状と寸法を指定します。
        // SpriteMaskComponent(shape: マスク形状, size: マスク寸法)
        explicit SpriteMaskComponent(
            SpriteMaskShape shape = SpriteMaskShape::Rectangle,
            DirectX::XMFLOAT2 size = { 128.0f, 128.0f }) noexcept
            : m_shape(shape), m_size(size)
        {
        }

        // マスク形状を設定します。
        // SetShape(value: マスク形状)
        void SetShape(SpriteMaskShape value) noexcept { m_shape = value; }
        // マスク寸法を設定します。
        // SetSize(value: マスク寸法)
        void SetSize(DirectX::XMFLOAT2 value) noexcept { m_size = value; }

        // Windowsと同じ保存用コンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        { return "SpriteMask"; }
    private:
        friend class Scene;
        // マスク形状
        SpriteMaskShape m_shape{};
        // マスク寸法
        DirectX::XMFLOAT2 m_size{};
    };

    class SpriteRendererComponent final : public Component
    {
    public:
        // 画像描画の寸法、色、テクスチャを指定します。
        // SpriteRendererComponent(size: 描画寸法, color: RGBA色, texture: 画像パス)
        SpriteRendererComponent(
            DirectX::XMFLOAT2 size,
            DirectX::XMFLOAT4 color,
            std::filesystem::path texture = {});

        // スプライト寸法を設定します。
        // SetSize(value: 幅と高さ)
        void SetSize(DirectX::XMFLOAT2 value) noexcept { m_size = value; }
        // スプライト色を設定します。
        // SetColor(value: RGBA色)
        void SetColor(DirectX::XMFLOAT4 value) noexcept { m_color = value; }
        // スプライト基準点を設定します。
        // SetPivot(value: UV基準点)
        void SetPivot(DirectX::XMFLOAT2 value) noexcept { m_pivot = value; }
        // UI内の描画順を設定します。
        // SetSortOrder(value: 描画順)
        void SetSortOrder(int value) noexcept { m_sortOrder = value; }
        // マスク内外の表示方法を設定します。
        // SetMaskInteraction(value: マスク表示方法)
        void SetMaskInteraction(SpriteMaskInteraction value) noexcept
        {
            m_maskInteraction = value;
        }
        // 画像から切り出すUV範囲を設定します。
        // SetSourceRect(value: UV切出し範囲)
        void SetSourceRect(DirectX::XMFLOAT4 value) noexcept
        {
            m_sourceRect = value;
        }
        // スプライト画像のパスを設定します。
        // SetTexturePath(value: 画像パス)
        void SetTexturePath(std::filesystem::path value)
        {
            m_texture = std::move(value);
        }

        // Windowsと同じ保存用コンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        { return "SpriteRenderer"; }
    private:
        friend class Scene;
        // スプライト幅と高さ
        DirectX::XMFLOAT2 m_size{};
        // スプライト描画色
        DirectX::XMFLOAT4 m_color{};
        // スプライト画像パス
        std::filesystem::path m_texture;
        // スプライト基準点
        DirectX::XMFLOAT2 m_pivot{};
        // 画像のUV切出し範囲
        DirectX::XMFLOAT4 m_sourceRect{ 0.0f, 0.0f, 1.0f, 1.0f };
        // マスク内外の表示方法
        SpriteMaskInteraction m_maskInteraction{};
        // UI内の描画順
        int m_sortOrder{};
    };

    struct SpriteAnimationClip final
    {
        // クリップ識別名
        std::string name;
        // シート上の開始番号
        int startFrame{};
        // クリップ内のフレーム数
        int frameCount{ 1 };
        // 1秒あたりの表示フレーム数
        float framesPerSecond{ 10.0f };
        // 再生終了後に先頭へ戻るか
        bool loop{ true };
    };

    class SpriteAnimatorComponent final : public Component
    {
    public:
        // スプライトシートの列数と行数を設定します。
        // SpriteAnimatorComponent(columns: 横分割数, rows: 縦分割数)
        SpriteAnimatorComponent(int columns = 1, int rows = 1) noexcept;
        // シート分割数を更新します。
        // SetSheetGrid(columns: 横分割数, rows: 縦分割数)
        void SetSheetGrid(int columns, int rows) noexcept;
        // シートの列数を返します。
        [[nodiscard]] int Columns() const noexcept { return m_columns; }
        // シートの行数を返します。
        [[nodiscard]] int Rows() const noexcept { return m_rows; }
        // クリップを再生一覧へ追加します。
        // AddClip(clip: 追加するクリップ)
        void AddClip(SpriteAnimationClip clip);
        // 名前が一致するクリップを削除します。
        // RemoveClip(name: 削除するクリップ名)
        void RemoveClip(std::string_view name);
        // 変更可能なクリップ一覧を返します。
        [[nodiscard]] std::vector<SpriteAnimationClip>& Clips() noexcept
        {
            return m_clips;
        }
        // 読み取り専用のクリップ一覧を返します。
        [[nodiscard]] const std::vector<SpriteAnimationClip>& Clips() const noexcept
        {
            return m_clips;
        }
        // 名前で指定したクリップを再生します。
        // Play(clipName: 再生するクリップ名)
        bool Play(std::string_view clipName);
        // クリップ再生を停止します。
        void Stop() noexcept { m_playing = false; }
        // クリップの再生状態を返します。
        [[nodiscard]] bool IsPlaying() const noexcept { return m_playing; }
        // 再生中のクリップ名を返します。
        [[nodiscard]] const std::string& ActiveClipName() const noexcept
        {
            return m_activeClip;
        }
        // 現在表示中のフレーム番号を返します。
        [[nodiscard]] int CurrentFrame() const noexcept
        {
            return m_currentFrame;
        }
        // 再生速度倍率を設定します。
        // SetSpeed(value: 再生倍率)
        void SetSpeed(float value) noexcept { m_speed = value; }
        // 現在の再生速度倍率を返します。
        [[nodiscard]] float Speed() const noexcept { return m_speed; }
        // 初期再生クリップ名を設定します。
        // SetDefaultClip(value: 既定クリップ名)
        void SetDefaultClip(std::string value)
        {
            m_defaultClip = std::move(value);
        }
        // 初期再生クリップ名を返します。
        [[nodiscard]] const std::string& DefaultClip() const noexcept
        {
            return m_defaultClip;
        }
        // シーン開始時の自動再生を設定します。
        // SetPlayOnStart(value: 自動再生設定)
        void SetPlayOnStart(bool value) noexcept { m_playOnStart = value; }
        // シーン開始時の自動再生設定を返します。
        [[nodiscard]] bool PlayOnStart() const noexcept { return m_playOnStart; }

        // Windowsと同じ保存用コンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        { return "SpriteAnimator"; }
    private:
        friend class Scene;
        // 名前が一致するクリップを検索します。
        // FindClip(name: 検索するクリップ名)
        [[nodiscard]] const SpriteAnimationClip* FindClip(
            std::string_view name) const noexcept;
        // 指定フレームの画像範囲を設定します。
        // ApplyFrame(frame: 適用するフレーム番号)
        void ApplyFrame(int frame);
        // 経過時間分だけ再生を進めます。
        // Advance(deltaTime: 経過秒数)
        void Advance(float deltaTime);

        // シートの列数
        int m_columns{ 1 };
        // シートの行数
        int m_rows{ 1 };
        // 登録されたアニメーションクリップ
        std::vector<SpriteAnimationClip> m_clips;
        // 開始時に再生するクリップ名
        std::string m_defaultClip;
        // 現在再生中のクリップ名
        std::string m_activeClip;
        // 再生速度倍率
        float m_speed{ 1.0f };
        // 現在のクリップ内再生時刻
        float m_time{};
        // 現在のシートフレーム番号
        int m_currentFrame{ -1 };
        // 再生中か
        bool m_playing{};
        // シーン開始時の自動再生設定
        bool m_playOnStart{ true };
        // 開始時処理を実行済みか
        bool m_started{};
    };

    class ParallaxLayerComponent final : public Component
    {
    public:
        // 移動率と参照オブジェクトを指定します。
        // ParallaxLayerComponent(factor: カメラ追従率, referenceId: 参照GameObject ID)
        ParallaxLayerComponent(
            DirectX::XMFLOAT2 factor = { 0.5f, 0.5f },
            GameObjectId referenceId = 0) noexcept
            : m_factor(factor), m_referenceSourceId(referenceId)
        {
        }

        // X・Y方向の視差係数を設定します。
        // SetFactor(value: 視差係数)
        void SetFactor(DirectX::XMFLOAT2 value) noexcept { m_factor = value; }
        // 現在の視差係数を返します。
        [[nodiscard]] const DirectX::XMFLOAT2& Factor() const noexcept
        {
            return m_factor;
        }
        // 参照元を変更し、初期化状態をリセットします。
        // SetReferenceId(value: 参照GameObject ID)
        void SetReferenceId(GameObjectId value) noexcept
        {
            m_referenceSourceId = value;
            m_reference = nullptr;
            m_initialized = false;
        }
        // 参照元GameObject IDを返します。
        [[nodiscard]] GameObjectId ReferenceId() const noexcept
        {
            return m_referenceSourceId;
        }

        // Windowsと同じ保存用コンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        { return "ParallaxLayer"; }
    private:
        friend class Scene;
        // カメラ移動から視差位置を更新します。
        // Advance(mainCamera: 追従するカメラ)
        void Advance(GameObject* mainCamera);

        // X・Y方向のカメラ追従率
        DirectX::XMFLOAT2 m_factor{ 0.5f, 0.5f };
        // 参照元GameObject ID
        GameObjectId m_referenceSourceId{};
        // 解決済みの参照GameObject
        GameObject* m_reference{};
        // 初期化時の参照位置
        DirectX::XMFLOAT2 m_referenceOrigin{};
        // 初期化時の自身の位置
        DirectX::XMFLOAT2 m_ownOrigin{};
        // 初期位置を取得済みか
        bool m_initialized{};
    };

    class NativeScriptComponent final : public Component
    {
    public:
        // 登録IDを使うネイティブスクリプトを設定します。
        // NativeScriptComponent(scriptId: 登録スクリプトID)
        explicit NativeScriptComponent(std::string scriptId)
            : m_scriptId(std::move(scriptId))
        {
        }

        // 生成済みスクリプトを返します。
        [[nodiscard]] Script* Instance() noexcept { return m_script.get(); }
        // 生成済みスクリプトを読み取り専用で返します。
        [[nodiscard]] const Script* Instance() const noexcept
        {
            return m_script.get();
        }
        // Component基底からスクリプト実体を取得します。
        [[nodiscard]] Script* ScriptInstance() const noexcept override
        {
            return m_script.get();
        }

    protected:
        // 登録済みスクリプトを生成します。
        void OnAttached() override;

        // Windowsと同じ保存用コンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        { return "NativeScript"; }
    private:
        // 生成する登録スクリプトID
        std::string m_scriptId;
        // GameObjectが所有するスクリプト実体
        std::unique_ptr<Script> m_script;
    };

    class Scene final
    {
    public:
        // 描画器、音声装置、入力装置を接続します。
        // Scene(renderer: 3D描画器, audio: Web音声装置, input: Web入力装置)
        Scene(
            Web::Renderer3D& renderer,
            Web::WebAudioRuntime& audio,
            Web::WebInput& input);
        // シーンが所有する実行資源を解放します。
        ~Scene();

        // 名前を指定してGameObjectを生成します。
        // CreateGameObject(name: GameObject名)
        [[nodiscard]] GameObject& CreateGameObject(std::string name);
        // 名前が一致するGameObjectを検索します。
        // FindGameObjectByName(name: 検索する名前)
        [[nodiscard]] GameObject* FindGameObjectByName(
            std::string_view name) noexcept;
        // タグが一致する最初のGameObjectを検索します。
        // FindGameObjectByTag(tag: 検索するタグ)
        [[nodiscard]] GameObject* FindGameObjectByTag(
            std::string_view tag) noexcept;
        // タグが一致するGameObjectを登録順に収集します。
        // FindGameObjectsByTag(tag: 検索するタグ)
        [[nodiscard]] std::vector<GameObject*> FindGameObjectsByTag(
            std::string_view tag) const;
        // 所有GameObjectを登録順で読み取り専用に返します。
        [[nodiscard]] const std::vector<std::unique_ptr<GameObject>>&
            GameObjects() const noexcept
        {
            return m_objects;
        }
        // Scene内で共有する名前付きイベントバスを返します。
        [[nodiscard]] EventBus& Events() noexcept { return m_events; }
        [[nodiscard]] const EventBus& Events() const noexcept { return m_events; }
        // GameObjectの破棄を予約します。
        // DestroyGameObject(gameObject: 破棄対象)
        bool DestroyGameObject(GameObject& gameObject);

        // シーン内から指定型のコンポーネントを検索します。
        template<typename T>
        [[nodiscard]] T* FindComponentOfType() noexcept
        {
            // シーン内オブジェクトを走査
            for (const auto& object : m_objects)
            {
                // 対象オブジェクトの型一致コンポーネント
                if (auto* component = object->GetComponent<T>())
                {
                    return component;
                }
            }
            return nullptr;
        }

        // 仮想パスからシーン文書を読み込みます。
        // Load(virtualPath: 仮想アセットパス)
        [[nodiscard]] bool Load(const std::filesystem::path& virtualPath);
        // PrefabアセットをSceneへ生成します(prefabPath: 仮想アセットパス, parent: 任意の同一Sceneの親)
        [[nodiscard]] GameObject& InstantiatePrefab(
            const std::filesystem::path& prefabPath,
            GameObject* parent = nullptr);
        // 所有スクリプトの開始処理を実行します。
        void StartScripts();
        // 固定時間刻みで物理とスクリプトを更新します。
        // FixedUpdate(deltaTime: 固定更新の経過秒数)
        void FixedUpdate(float deltaTime);
        // フレーム時間でシーン状態を更新します。
        // Update(deltaTime: フレーム経過秒数)
        void Update(float deltaTime);
        // シーン内の描画要素を描画します。
        void Render();
        // 物理状態の描画補間率を設定します。
        // SetPhysicsInterpolationAlpha(value: 0から1の補間率)
        void SetPhysicsInterpolationAlpha(float value) noexcept
        {
            m_interpolationAlpha = value;
        }
        // 現在の物理描画補間率を返します。
        [[nodiscard]] float PhysicsInterpolationAlpha() const noexcept
        {
            return m_interpolationAlpha;
        }
        // レイと最初に衝突した物体を検索します。
        // Raycast(ray: 判定レイ, maximumDistance: 最大距離, hit: 衝突結果, filter: 判定対象条件)
        [[nodiscard]] bool Raycast(
            const Ray& ray,
            float maximumDistance,
            PhysicsHit& hit,
            const PhysicsQueryFilter& filter = {}) const;
        // シーンの描画装置を返します。
        [[nodiscard]] GraphicsDevice& Graphics() noexcept { return m_graphics; }
        // シーンのWeb音声装置を返します。
        [[nodiscard]] Web::WebAudioRuntime& WebAudio() noexcept
        {
            return *m_audio;
        }
        // シーン間共有状態を変更可能な参照で返します。
        [[nodiscard]] SceneCollection& Scenes() noexcept { return m_scenes; }
        // シーン間共有状態を読み取り専用で返します。
        [[nodiscard]] const SceneCollection& Scenes() const noexcept
        {
            return m_scenes;
        }

    private:
        // 破棄予約済みGameObjectを所有一覧から削除します。
        void FlushDestroyedObjects();
        // 対象GameObjectのScriptへ終了通知を送り、例外をログに残します。
        void DestroyScripts(GameObject& gameObject) noexcept;
        // 予約された主Scene読み込みを適用します。
        void ProcessPendingSceneLoad();
        // SceneまたはPrefab JSONをトランザクションで復元します。
        bool LoadDocument(
            std::string_view json,
            GameObject* prefabParent,
            GameObject** prefabRoot,
            bool restoreEnvironment);
        // Awake・有効状態通知・必要なStartをScene内Scriptへ適用します。
        void PrepareScripts(
            bool startActiveScripts,
            bool flushDestroyedObjects = true);

        struct Impl;
        // 内部実装状態
        std::unique_ptr<Impl> m_impl;
        // 3D描画バックエンド
        Web::Renderer3D* m_renderer{};
        // Web音声バックエンド
        Web::WebAudioRuntime* m_audio{};
        // シーンが使う描画装置
        GraphicsDevice m_graphics;
        // シーン間共有状態
        SceneCollection m_scenes;
        // ScriptとUI componentが共有する名前付きイベント
        EventBus m_events;
        // シーンが所有するGameObject
        std::vector<std::unique_ptr<GameObject>> m_objects;
        // フレーム終端で破棄する識別子
        std::vector<GameObjectId> m_pendingDestroy;
        // 次に割り当てるGameObject ID
        GameObjectId m_nextId{ 1 };
        // 物理状態の描画補間率
        float m_interpolationAlpha{};
    };

    // 指定型のスクリプト実体を検索します。
    template<typename T>
    T* GameObject::GetScript() const noexcept
    {
        // 所有コンポーネントからスクリプトを検索
        for (const auto& component : m_components)
        {
            // ネイティブスクリプトコンポーネント
            auto* native = dynamic_cast<NativeScriptComponent*>(component.get());
            // 他のコンポーネントは検索対象外
            if (native == nullptr)
            {
                continue;
            }
            // 型が一致するスクリプト実体
            if (auto* script = dynamic_cast<T*>(native->Instance()))
            {
                return script;
            }
        }
        return nullptr;
    }

    template<typename T>
    T* Script::GetComponent() noexcept
    {
        return Owner().GetComponent<T>();
    }

    template<typename T>
    const T* Script::GetComponent() const noexcept
    {
        return static_cast<const GameObject&>(Owner())
            .GetComponent<T>();
    }

    template<typename T>
    T* Script::GetScript() const noexcept
    {
        return Owner().GetScript<T>();
    }

    template<typename T>
    T* Script::GetComponentInParent(
        const bool includeInactive) noexcept
    {
        return Owner().GetComponentInParent<T>(includeInactive);
    }

    template<typename T>
    T* Script::GetComponentInChildren(
        const bool includeInactive) noexcept
    {
        return Owner().GetComponentInChildren<T>(includeInactive);
    }

    template<typename T>
    std::vector<T*> Script::GetComponentsInChildren(
        const bool includeInactive)
    {
        return Owner().GetComponentsInChildren<T>(includeInactive);
    }

    template<typename T>
    std::vector<T*> Script::GetComponentsInParent(
        const bool includeInactive)
    {
        return Owner().GetComponentsInParent<T>(includeInactive);
    }

    template<typename T>
    T* Script::GetScriptInChildren(
        const bool includeInactive) const noexcept
    {
        return Owner().GetScriptInChildren<T>(includeInactive);
    }

    template<typename T, typename... Args>
    T& Script::AddComponent(Args&&... args)
    {
        return Owner().AddComponent<T>(std::forward<Args>(args)...);
    }

    inline Transform& Script::GetTransform() noexcept
    {
        return Owner().GetTransform();
    }

    inline const Transform& Script::GetTransform() const noexcept
    {
        return static_cast<const GameObject&>(Owner()).GetTransform();
    }
}

#define LAMAPON_PORTABLE_JOIN_DETAIL(left, right) left##right
#define LAMAPON_PORTABLE_JOIN(left, right) \
    LAMAPON_PORTABLE_JOIN_DETAIL(left, right)
#define LAMAPON_SCRIPT_NAMED(type, id, displayName)                         \
    namespace                                                              \
    {                                                                      \
        const bool LAMAPON_PORTABLE_JOIN(                                  \
            LamaPonPortableScriptRegistration_, __LINE__) =                \
            ::LamaPon::RegisterPortableScript(                             \
                id,                                                        \
                displayName,                                               \
                []() -> std::unique_ptr<::LamaPon::Script>                 \
                {                                                          \
                    return std::make_unique<type>();                        \
                });                                                        \
    }
#define LAMAPON_SCRIPT(type)                                                \
    LAMAPON_SCRIPT_NAMED(type, "Game." #type, #type)
#define LAMAPON_SCRIPT_WITH_SCHEMA(type, id, displayName, schema)           \
    LAMAPON_SCRIPT_NAMED(type, id, displayName)
