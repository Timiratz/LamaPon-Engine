#pragma once

#include <GamePad.h>
#include <Keyboard.h>
#include <DirectXMath.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace LamaPon
{
    enum class InputControl
    {
        // キーボードのA
        KeyboardA,
        // キーボードのB
        KeyboardB,
        // キーボードのC
        KeyboardC,
        // キーボードのD
        KeyboardD,
        // キーボードのE
        KeyboardE,
        // キーボードのF
        KeyboardF,
        // キーボードのG
        KeyboardG,
        // キーボードのH
        KeyboardH,
        // キーボードのI
        KeyboardI,
        // キーボードのJ
        KeyboardJ,
        // キーボードのK
        KeyboardK,
        // キーボードのL
        KeyboardL,
        // キーボードのM
        KeyboardM,
        // キーボードのN
        KeyboardN,
        // キーボードのO
        KeyboardO,
        // キーボードのP
        KeyboardP,
        // キーボードのQ
        KeyboardQ,
        // キーボードのR
        KeyboardR,
        // キーボードのS
        KeyboardS,
        // キーボードのT
        KeyboardT,
        // キーボードのU
        KeyboardU,
        // キーボードのV
        KeyboardV,
        // キーボードのW
        KeyboardW,
        // キーボードのX
        KeyboardX,
        // キーボードのY
        KeyboardY,
        // キーボードのZ
        KeyboardZ,
        // キーボードの0
        KeyboardAlpha0,
        // キーボードの1
        KeyboardAlpha1,
        // キーボードの2
        KeyboardAlpha2,
        // キーボードの3
        KeyboardAlpha3,
        // キーボードの4
        KeyboardAlpha4,
        // キーボードの5
        KeyboardAlpha5,
        // キーボードの6
        KeyboardAlpha6,
        // キーボードの7
        KeyboardAlpha7,
        // キーボードの8
        KeyboardAlpha8,
        // キーボードの9
        KeyboardAlpha9,
        // キーボードのF1
        KeyboardF1,
        // キーボードのF2
        KeyboardF2,
        // キーボードのF3
        KeyboardF3,
        // キーボードのF4
        KeyboardF4,
        // キーボードのF5
        KeyboardF5,
        // キーボードのF6
        KeyboardF6,
        // キーボードのF7
        KeyboardF7,
        // キーボードのF8
        KeyboardF8,
        // キーボードのF9
        KeyboardF9,
        // キーボードのF10
        KeyboardF10,
        // キーボードのF11
        KeyboardF11,
        // キーボードのF12
        KeyboardF12,
        // キーボードのSpace
        KeyboardSpace,
        // キーボードのEnter
        KeyboardEnter,
        // キーボードのEscape
        KeyboardEscape,
        // キーボードのTab
        KeyboardTab,
        // キーボードのBackspace
        KeyboardBackspace,
        // キーボードのDelete
        KeyboardDelete,
        // キーボードのInsert
        KeyboardInsert,
        // キーボードのHome
        KeyboardHome,
        // キーボードのEnd
        KeyboardEnd,
        // キーボードのPageUp
        KeyboardPageUp,
        // キーボードのPageDown
        KeyboardPageDown,
        // キーボードの↑
        KeyboardUp,
        // キーボードの↓
        KeyboardDown,
        // キーボードの←
        KeyboardLeft,
        // キーボードの→
        KeyboardRight,
        // キーボードの左Shift
        KeyboardLeftShift,
        // キーボードの右Shift
        KeyboardRightShift,
        // キーボードの左Ctrl
        KeyboardLeftControl,
        // キーボードの右Ctrl
        KeyboardRightControl,
        // キーボードの左Alt
        KeyboardLeftAlt,
        // キーボードの右Alt
        KeyboardRightAlt,
        // マウスの左ボタン
        MouseLeft,
        // マウスの右ボタン
        MouseRight,
        // マウスの中ボタン
        MouseMiddle,
        // マウスのX1ボタン
        MouseX1,
        // マウスのX2ボタン
        MouseX2,
        // マウスのホイール上
        MouseWheelUp,
        // マウスのホイール下
        MouseWheelDown,
        // ゲームパッドのA
        GamePadA,
        // ゲームパッドのB
        GamePadB,
        // ゲームパッドのX
        GamePadX,
        // ゲームパッドのY
        GamePadY,
        // ゲームパッドの十字上
        GamePadDPadUp,
        // ゲームパッドの十字下
        GamePadDPadDown,
        // ゲームパッドの十字左
        GamePadDPadLeft,
        // ゲームパッドの十字右
        GamePadDPadRight,
        // ゲームパッドのLB
        GamePadLeftShoulder,
        // ゲームパッドのRB
        GamePadRightShoulder,
        // ゲームパッドのBack
        GamePadBack,
        // ゲームパッドのStart
        GamePadStart,
        // ゲームパッドの左Stick押込
        GamePadLeftStickButton,
        // ゲームパッドの右Stick押込
        GamePadRightStickButton,
        // ゲームパッドの左Stick X
        GamePadLeftX,
        // ゲームパッドの左Stick Y
        GamePadLeftY,
        // ゲームパッドの右Stick X
        GamePadRightX,
        // ゲームパッドの右Stick Y
        GamePadRightY,
        // ゲームパッドのLT
        GamePadLeftTrigger,
        // ゲームパッドのRT
        GamePadRightTrigger,
        // 有効な入力種別の個数
        Count
    };

    // キーボード入力の範囲内か調べます(control: 入力種別)。
    [[nodiscard]] constexpr bool IsKeyboardControl(
        const InputControl control) noexcept
    {
        return control >= InputControl::KeyboardA
            && control <= InputControl::KeyboardRightAlt;
    }

    // マウス入力の範囲内か調べます(control: 入力種別)。
    [[nodiscard]] constexpr bool IsMouseControl(
        const InputControl control) noexcept
    {
        return control >= InputControl::MouseLeft
            && control <= InputControl::MouseWheelDown;
    }

    // ゲームパッド入力の範囲内か調べます(control: 入力種別)。
    [[nodiscard]] constexpr bool IsGamePadControl(
        const InputControl control) noexcept
    {
        return control >= InputControl::GamePadA
            && control < InputControl::Count;
    }

    struct InputBinding final
    {
        // 割り当てる入力種別
        InputControl control{ InputControl::KeyboardSpace };
        // 入力値への加算倍率
        float scale{ 1.0f };
    };

    struct InputActionDefinition final
    {
        // 入力アクションの名前
        std::string name;
        // 加算する操作の割り当て
        std::vector<InputBinding> bindings;
    };

    struct InputSnapshot final
    {
        // 入力種別ごとの値
        std::unordered_map<InputControl, float> values;

        // 入力値を-1〜1へ制限して設定します(control: 入力種別, value: 入力値)。
        void Set(InputControl control, float value);
        // 入力値を取得し、未登録なら0を返します(control: 入力種別)。
        [[nodiscard]] float Get(
            InputControl control) const noexcept;
    };

    enum class PointerButton : std::uint8_t
    {
        // マウスの左ボタン
        Left,
        // マウスの右ボタン
        Right,
        // マウスの中ボタン
        Middle,
        // マウスのX1ボタン
        Extra1,
        // マウスのX2ボタン
        Extra2,
        // 有効な入力種別の個数
        Count
    };

    struct InputPointerButtonState final
    {
        // 現在の押下有無
        bool down{};
        // 今フレームの押下遷移
        bool pressed{};
        // 今フレームの解放遷移
        bool released{};
    };

    // positionとdeltaはクライアント領域の画素単位で、wheelはWHEEL_DELTAを1とする量です。
    // down・pressed・releasedは左ボタンの集約状態です。
    struct InputPointerState final
    {
        // 現在のポインター位置
        DirectX::XMFLOAT2 position{};
        // クライアント領域内の位置か
        bool valid{};
        // 左ボタンの押下有無
        bool down{};
        // 左ボタンの押下遷移
        bool pressed{};
        // 左ボタンの解放遷移
        bool released{};
        // 前回位置からの移動量
        DirectX::XMFLOAT2 delta{};
        // 縦ホイールの移動量
        float wheel{};
        // 横ホイールの移動量
        float wheelHorizontal{};
        // ボタン別の現在フレーム状態
        std::array<
            InputPointerButtonState,
            static_cast<std::size_t>(PointerButton::Count)>
            buttons{};

        // 指定ボタンの状態を参照します(button: Count未満のボタン種別)。
        [[nodiscard]] const InputPointerButtonState& Button(
            const PointerButton button) const noexcept
        {
            return buttons[static_cast<std::size_t>(button)];
        }
    };

    // 移動・視点・ジャンプなど標準の入力割り当てを返します。
    [[nodiscard]] std::vector<InputActionDefinition>
        DefaultInputActions();
    // 入力割り当ての個数・名前・操作値を検証します(actions: 検証する割り当て)。
    // アクションは1〜64件で名前は一意とし、不正な値はinvalid_argumentです。
    void ValidateInputActions(
        std::span<const InputActionDefinition> actions);
    // Countを除く全入力種別の静的な配列を参照します。
    [[nodiscard]] std::span<const InputControl>
        AllInputControls() noexcept;
    // 入力種別の保存名を返します(control: 入力種別)。
    // 範囲外ならUnknownです。
    [[nodiscard]] std::string_view InputControlName(
        InputControl control) noexcept;
    // 入力種別の表示名を返します(control: 入力種別)。
    // 範囲外なら不明です。
    [[nodiscard]] std::string_view InputControlDisplayName(
        InputControl control) noexcept;
    // 保存名から入力種別を復元します(name: 保存名)。
    // 大文字小文字を区別し、未定義の名前はinvalid_argumentです。
    [[nodiscard]] InputControl InputControlFromName(
        std::string_view name);

    // 入力の更新とWin32メッセージ転送は同じ主スレッドで行います。
    // Win32メッセージの転送先は最後に構築した生存インスタンスです。
    class InputSystem final
    {
    public:
        // デバイスと標準入力を初期化します(nativeWindow: 非所有のウィンドウハンドル)。
        explicit InputSystem(void* nativeWindow = nullptr);
        // メッセージ転送先を解除して入力デバイスを解放します。
        ~InputSystem();

        // 入力デバイスの共有を防ぐためコピーを禁止します。
        InputSystem(const InputSystem&) = delete;
        // 入力デバイスの共有を防ぐためコピー代入を禁止します。
        InputSystem& operator=(const InputSystem&) = delete;

        // Win32の入力メッセージを蓄積します(message: メッセージID, wParam: 第1パラメーター, lParam: 第2パラメーター)。
        // 短い押下を保持するためウィンドウ処理から転送し、転送先がない場合は何もしません。
        static void ProcessWindowMessage(
            std::uint32_t message,
            std::uint64_t wParam,
            std::int64_t lParam) noexcept;

        // 検証した入力割り当てを設定します(actions: 新しい割り当て)。
        // 現在値と前フレーム値は消去されます。
        void SetActions(
            std::vector<InputActionDefinition> actions);
        // 現在の入力割り当てを参照します。
        [[nodiscard]] const std::vector<InputActionDefinition>&
            Actions() const noexcept
        {
            return m_actions;
        }

        // 実デバイスから入力と遷移を更新します(allowKeyboardActions: キーボード割り当ての許可)。
        // ゲームパッドは接続番号0を使い、読み取ったフレームイベントを消費します。
        void Update(bool allowKeyboardActions = true);
        // 指定入力からアクションを更新します(snapshot: 今フレームの入力値)。
        // ポインターは通常どおり更新し、フレームイベントも消費します。
        void UpdateFromSnapshot(const InputSnapshot& snapshot);
        // 次の更新だけポインター状態を置き換えます(state: 次のポインター状態)。
        void SetPointerOverride(
            InputPointerState state) noexcept
        {
            m_pointerOverride = state;
        }
        // 最後に更新したポインター状態を参照します。
        [[nodiscard]] const InputPointerState&
            Pointer() const noexcept
        {
            return m_pointerState;
        }
        // 今フレームのUTF-16文字入力を参照します。
        // IME確定文字を含み、制御文字はBackspace・Enter・Tabだけを受け付けます。
        [[nodiscard]] const std::wstring&
            TextInput() const noexcept
        {
            return m_frameTextInput;
        }

        // アクションの値を取得し、未定義なら0を返します(action: アクション名)。
        [[nodiscard]] float Value(
            std::string_view action) const noexcept;
        // アクションの絶対値が閾値以上か調べます(action: アクション名, threshold: 押下閾値)。
        [[nodiscard]] bool IsDown(
            std::string_view action,
            float threshold = 0.5f) const noexcept;
        // 閾値未満から以上への遷移を調べます(action: アクション名, threshold: 押下閾値)。
        [[nodiscard]] bool WasPressed(
            std::string_view action,
            float threshold = 0.5f) const noexcept;
        // 閾値以上から未満への遷移を調べます(action: アクション名, threshold: 押下閾値)。
        [[nodiscard]] bool WasReleased(
            std::string_view action,
            float threshold = 0.5f) const noexcept;

        // 最後に読み取ったキーボード状態を参照します。
        [[nodiscard]] const DirectX::Keyboard::State&
            KeyboardState() const noexcept
        {
            return m_keyboardState;
        }
        // 最後に読み取ったゲームパッド状態を参照します。
        [[nodiscard]] const DirectX::GamePad::State&
            GamePadState() const noexcept
        {
            return m_gamePadState;
        }
        // 最後の更新時点でゲームパッドが接続済みか返します。
        [[nodiscard]] bool IsGamePadConnected() const noexcept
        {
            return m_gamePadState.IsConnected();
        }

    private:
        struct ControlEventState final
        {
            // メッセージ上の押下有無
            bool down{};
            // 更新前の押下回数
            std::uint8_t pressedCount{};
            // 更新前の解放回数
            std::uint8_t releasedCount{};
        };

        // Win32入力を内部イベントへ変換します(message: メッセージID, wParam: 第1パラメーター, lParam: 第2パラメーター)。
        void HandleWindowMessage(
            std::uint32_t message,
            std::uint64_t wParam,
            std::int64_t lParam) noexcept;
        // 非アクティブ時に蓄積入力とポインター履歴を消去します。
        void ResetEventStates() noexcept;
        // 押下・解放・ホイールを消費し、蓄積文字を公開します。
        void ClearFrameEvents() noexcept;
        // 前フレーム値を保存して全アクションを更新します(snapshot: 今フレームの入力値)。
        void ApplySnapshot(const InputSnapshot& snapshot);
        // 上書き指定または実ウィンドウからポインターを更新します。
        void UpdatePointer() noexcept;
        // 入力割り当てを合算して-1〜1へ制限します(action: 入力割り当て, snapshot: 入力値)。
        [[nodiscard]] static float ActionValue(
            const InputActionDefinition& action,
            const InputSnapshot& snapshot) noexcept;

        // 所有するキーボード入力源
        std::unique_ptr<DirectX::Keyboard> m_keyboard;
        // 所有するゲームパッド入力源
        std::unique_ptr<DirectX::GamePad> m_gamePad;
        // 非所有のウィンドウハンドル
        void* m_nativeWindow{};
        // 前回取得したキーボード状態
        DirectX::Keyboard::State m_keyboardState{};
        // 前回取得したゲームパッド状態
        DirectX::GamePad::State m_gamePadState{};
        // 登録済み入力アクション
        std::vector<InputActionDefinition> m_actions;
        // 今フレームのアクション値
        std::unordered_map<std::string, float> m_values;
        // 前フレームのアクション値
        std::unordered_map<std::string, float> m_previousValues;
        // 最後に更新したポインター状態
        InputPointerState m_pointerState;
        // 次回だけ使うポインター上書き
        std::optional<InputPointerState>
            m_pointerOverride;
        // キー別の蓄積イベント
        std::array<
            ControlEventState,
            static_cast<std::size_t>(InputControl::MouseLeft)>
            m_keyEvents{};
        // マウスボタン別の蓄積イベント
        std::array<
            ControlEventState,
            static_cast<std::size_t>(PointerButton::Count)>
            m_mouseButtonEvents{};
        // 蓄積した縦ホイール移動
        std::int32_t m_wheelAccumulator{};
        // 蓄積した横ホイール移動
        std::int32_t m_wheelHorizontalAccumulator{};
        // 次の更新に渡す入力文字
        std::wstring m_textInputAccumulator;
        // 今フレームの入力文字
        std::wstring m_frameTextInput;
        // 前回のクライアント座標
        DirectX::XMFLOAT2 m_lastCursorPosition{};
        // 前回座標の有効性
        bool m_hasLastCursorPosition{};
    };
}
