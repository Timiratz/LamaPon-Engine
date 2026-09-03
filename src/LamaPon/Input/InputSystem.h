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
        KeyboardA,
        KeyboardB,
        KeyboardC,
        KeyboardD,
        KeyboardE,
        KeyboardF,
        KeyboardG,
        KeyboardH,
        KeyboardI,
        KeyboardJ,
        KeyboardK,
        KeyboardL,
        KeyboardM,
        KeyboardN,
        KeyboardO,
        KeyboardP,
        KeyboardQ,
        KeyboardR,
        KeyboardS,
        KeyboardT,
        KeyboardU,
        KeyboardV,
        KeyboardW,
        KeyboardX,
        KeyboardY,
        KeyboardZ,
        KeyboardAlpha0,
        KeyboardAlpha1,
        KeyboardAlpha2,
        KeyboardAlpha3,
        KeyboardAlpha4,
        KeyboardAlpha5,
        KeyboardAlpha6,
        KeyboardAlpha7,
        KeyboardAlpha8,
        KeyboardAlpha9,
        KeyboardF1,
        KeyboardF2,
        KeyboardF3,
        KeyboardF4,
        KeyboardF5,
        KeyboardF6,
        KeyboardF7,
        KeyboardF8,
        KeyboardF9,
        KeyboardF10,
        KeyboardF11,
        KeyboardF12,
        KeyboardSpace,
        KeyboardEnter,
        KeyboardEscape,
        KeyboardTab,
        KeyboardBackspace,
        KeyboardDelete,
        KeyboardInsert,
        KeyboardHome,
        KeyboardEnd,
        KeyboardPageUp,
        KeyboardPageDown,
        KeyboardUp,
        KeyboardDown,
        KeyboardLeft,
        KeyboardRight,
        KeyboardLeftShift,
        KeyboardRightShift,
        KeyboardLeftControl,
        KeyboardRightControl,
        KeyboardLeftAlt,
        KeyboardRightAlt,
        MouseLeft,
        MouseRight,
        MouseMiddle,
        MouseX1,
        MouseX2,
        MouseWheelUp,
        MouseWheelDown,
        GamePadA,
        GamePadB,
        GamePadX,
        GamePadY,
        GamePadDPadUp,
        GamePadDPadDown,
        GamePadDPadLeft,
        GamePadDPadRight,
        GamePadLeftShoulder,
        GamePadRightShoulder,
        GamePadBack,
        GamePadStart,
        GamePadLeftStickButton,
        GamePadRightStickButton,
        GamePadLeftX,
        GamePadLeftY,
        GamePadRightX,
        GamePadRightY,
        GamePadLeftTrigger,
        GamePadRightTrigger,
        Count
    };

    [[nodiscard]] constexpr bool IsKeyboardControl(
        const InputControl control) noexcept
    {
        return control >= InputControl::KeyboardA
            && control <= InputControl::KeyboardRightAlt;
    }

    [[nodiscard]] constexpr bool IsMouseControl(
        const InputControl control) noexcept
    {
        return control >= InputControl::MouseLeft
            && control <= InputControl::MouseWheelDown;
    }

    [[nodiscard]] constexpr bool IsGamePadControl(
        const InputControl control) noexcept
    {
        return control >= InputControl::GamePadA
            && control < InputControl::Count;
    }

    struct InputBinding final
    {
        InputControl control{ InputControl::KeyboardSpace };
        float scale{ 1.0f };
    };

    struct InputActionDefinition final
    {
        std::string name;
        std::vector<InputBinding> bindings;
    };

    struct InputSnapshot final
    {
        std::unordered_map<InputControl, float> values;

        void Set(InputControl control, float value);
        [[nodiscard]] float Get(
            InputControl control) const noexcept;
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
        bool down{};
        bool pressed{};
        bool released{};
    };

    struct InputPointerState final
    {
        DirectX::XMFLOAT2 position{};
        bool valid{};
        bool down{};
        bool pressed{};
        bool released{};
        DirectX::XMFLOAT2 delta{};
        float wheel{};
        float wheelHorizontal{};
        std::array<
            InputPointerButtonState,
            static_cast<std::size_t>(PointerButton::Count)>
            buttons{};

        [[nodiscard]] const InputPointerButtonState& Button(
            const PointerButton button) const noexcept
        {
            return buttons[static_cast<std::size_t>(button)];
        }
    };

    [[nodiscard]] std::vector<InputActionDefinition>
        DefaultInputActions();
    void ValidateInputActions(
        std::span<const InputActionDefinition> actions);
    [[nodiscard]] std::span<const InputControl>
        AllInputControls() noexcept;
    [[nodiscard]] std::string_view InputControlName(
        InputControl control) noexcept;
    [[nodiscard]] std::string_view InputControlDisplayName(
        InputControl control) noexcept;
    [[nodiscard]] InputControl InputControlFromName(
        std::string_view name);

    class InputSystem final
    {
    public:
        explicit InputSystem(void* nativeWindow = nullptr);
        ~InputSystem();

        InputSystem(const InputSystem&) = delete;
        InputSystem& operator=(const InputSystem&) = delete;

        // ウィンドウプロシージャが生のWin32メッセージをここへ転送
        // することで、1フレーム未満の押下も取りこぼしません。
        // InputSystemのインスタンスが無い状態で呼んでも安全です。
        static void ProcessWindowMessage(
            std::uint32_t message,
            std::uint64_t wParam,
            std::int64_t lParam) noexcept;

        void SetActions(
            std::vector<InputActionDefinition> actions);
        [[nodiscard]] const std::vector<InputActionDefinition>&
            Actions() const noexcept
        {
            return m_actions;
        }

        void Update(bool allowKeyboardActions = true);
        void UpdateFromSnapshot(const InputSnapshot& snapshot);
        void SetPointerOverride(
            InputPointerState state) noexcept
        {
            m_pointerOverride = state;
        }
        [[nodiscard]] const InputPointerState&
            Pointer() const noexcept
        {
            return m_pointerState;
        }
        // このフレームに入力された文字列（UTF-16、IME確定文字を含む）。
        // 制御文字はバックスペース(0x08)・Enter(0x0D)・Tab(0x09)のみ
        // 含まれます。
        [[nodiscard]] const std::wstring&
            TextInput() const noexcept
        {
            return m_frameTextInput;
        }

        [[nodiscard]] float Value(
            std::string_view action) const noexcept;
        [[nodiscard]] bool IsDown(
            std::string_view action,
            float threshold = 0.5f) const noexcept;
        [[nodiscard]] bool WasPressed(
            std::string_view action,
            float threshold = 0.5f) const noexcept;
        [[nodiscard]] bool WasReleased(
            std::string_view action,
            float threshold = 0.5f) const noexcept;

        [[nodiscard]] const DirectX::Keyboard::State&
            KeyboardState() const noexcept
        {
            return m_keyboardState;
        }
        [[nodiscard]] const DirectX::GamePad::State&
            GamePadState() const noexcept
        {
            return m_gamePadState;
        }
        [[nodiscard]] bool IsGamePadConnected() const noexcept
        {
            return m_gamePadState.IsConnected();
        }

    private:
        struct ControlEventState final
        {
            bool down{};
            std::uint8_t pressedCount{};
            std::uint8_t releasedCount{};
        };

        void HandleWindowMessage(
            std::uint32_t message,
            std::uint64_t wParam,
            std::int64_t lParam) noexcept;
        void ResetEventStates() noexcept;
        void ClearFrameEvents() noexcept;
        void ApplySnapshot(const InputSnapshot& snapshot);
        void UpdatePointer() noexcept;
        [[nodiscard]] static float ActionValue(
            const InputActionDefinition& action,
            const InputSnapshot& snapshot) noexcept;

        std::unique_ptr<DirectX::Keyboard> m_keyboard;
        std::unique_ptr<DirectX::GamePad> m_gamePad;
        void* m_nativeWindow{};
        DirectX::Keyboard::State m_keyboardState{};
        DirectX::GamePad::State m_gamePadState{};
        std::vector<InputActionDefinition> m_actions;
        std::unordered_map<std::string, float> m_values;
        std::unordered_map<std::string, float> m_previousValues;
        InputPointerState m_pointerState;
        std::optional<InputPointerState>
            m_pointerOverride;
        std::array<
            ControlEventState,
            static_cast<std::size_t>(InputControl::MouseLeft)>
            m_keyEvents{};
        std::array<
            ControlEventState,
            static_cast<std::size_t>(PointerButton::Count)>
            m_mouseButtonEvents{};
        std::int32_t m_wheelAccumulator{};
        std::int32_t m_wheelHorizontalAccumulator{};
        std::wstring m_textInputAccumulator;
        std::wstring m_frameTextInput;
        DirectX::XMFLOAT2 m_lastCursorPosition{};
        bool m_hasLastCursorPosition{};
    };
}
