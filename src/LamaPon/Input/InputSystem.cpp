#include "LamaPon/Input/InputSystem.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace
{
    using LamaPon::InputControl;
    using LamaPon::PointerButton;

    struct ControlDescriptor final
    {
        InputControl control;
        std::string_view name;
        std::string_view displayName;
        DirectX::Keyboard::Keys key{
            DirectX::Keyboard::Keys::None };
    };

    using Keys = DirectX::Keyboard::Keys;

    constexpr std::array ControlDescriptors{
        ControlDescriptor{ InputControl::KeyboardA, "KeyboardA", "キーボード / A", Keys::A },
        ControlDescriptor{ InputControl::KeyboardB, "KeyboardB", "キーボード / B", Keys::B },
        ControlDescriptor{ InputControl::KeyboardC, "KeyboardC", "キーボード / C", Keys::C },
        ControlDescriptor{ InputControl::KeyboardD, "KeyboardD", "キーボード / D", Keys::D },
        ControlDescriptor{ InputControl::KeyboardE, "KeyboardE", "キーボード / E", Keys::E },
        ControlDescriptor{ InputControl::KeyboardF, "KeyboardF", "キーボード / F", Keys::F },
        ControlDescriptor{ InputControl::KeyboardG, "KeyboardG", "キーボード / G", Keys::G },
        ControlDescriptor{ InputControl::KeyboardH, "KeyboardH", "キーボード / H", Keys::H },
        ControlDescriptor{ InputControl::KeyboardI, "KeyboardI", "キーボード / I", Keys::I },
        ControlDescriptor{ InputControl::KeyboardJ, "KeyboardJ", "キーボード / J", Keys::J },
        ControlDescriptor{ InputControl::KeyboardK, "KeyboardK", "キーボード / K", Keys::K },
        ControlDescriptor{ InputControl::KeyboardL, "KeyboardL", "キーボード / L", Keys::L },
        ControlDescriptor{ InputControl::KeyboardM, "KeyboardM", "キーボード / M", Keys::M },
        ControlDescriptor{ InputControl::KeyboardN, "KeyboardN", "キーボード / N", Keys::N },
        ControlDescriptor{ InputControl::KeyboardO, "KeyboardO", "キーボード / O", Keys::O },
        ControlDescriptor{ InputControl::KeyboardP, "KeyboardP", "キーボード / P", Keys::P },
        ControlDescriptor{ InputControl::KeyboardQ, "KeyboardQ", "キーボード / Q", Keys::Q },
        ControlDescriptor{ InputControl::KeyboardR, "KeyboardR", "キーボード / R", Keys::R },
        ControlDescriptor{ InputControl::KeyboardS, "KeyboardS", "キーボード / S", Keys::S },
        ControlDescriptor{ InputControl::KeyboardT, "KeyboardT", "キーボード / T", Keys::T },
        ControlDescriptor{ InputControl::KeyboardU, "KeyboardU", "キーボード / U", Keys::U },
        ControlDescriptor{ InputControl::KeyboardV, "KeyboardV", "キーボード / V", Keys::V },
        ControlDescriptor{ InputControl::KeyboardW, "KeyboardW", "キーボード / W", Keys::W },
        ControlDescriptor{ InputControl::KeyboardX, "KeyboardX", "キーボード / X", Keys::X },
        ControlDescriptor{ InputControl::KeyboardY, "KeyboardY", "キーボード / Y", Keys::Y },
        ControlDescriptor{ InputControl::KeyboardZ, "KeyboardZ", "キーボード / Z", Keys::Z },
        ControlDescriptor{ InputControl::KeyboardAlpha0, "KeyboardAlpha0", "キーボード / 0", Keys::D0 },
        ControlDescriptor{ InputControl::KeyboardAlpha1, "KeyboardAlpha1", "キーボード / 1", Keys::D1 },
        ControlDescriptor{ InputControl::KeyboardAlpha2, "KeyboardAlpha2", "キーボード / 2", Keys::D2 },
        ControlDescriptor{ InputControl::KeyboardAlpha3, "KeyboardAlpha3", "キーボード / 3", Keys::D3 },
        ControlDescriptor{ InputControl::KeyboardAlpha4, "KeyboardAlpha4", "キーボード / 4", Keys::D4 },
        ControlDescriptor{ InputControl::KeyboardAlpha5, "KeyboardAlpha5", "キーボード / 5", Keys::D5 },
        ControlDescriptor{ InputControl::KeyboardAlpha6, "KeyboardAlpha6", "キーボード / 6", Keys::D6 },
        ControlDescriptor{ InputControl::KeyboardAlpha7, "KeyboardAlpha7", "キーボード / 7", Keys::D7 },
        ControlDescriptor{ InputControl::KeyboardAlpha8, "KeyboardAlpha8", "キーボード / 8", Keys::D8 },
        ControlDescriptor{ InputControl::KeyboardAlpha9, "KeyboardAlpha9", "キーボード / 9", Keys::D9 },
        ControlDescriptor{ InputControl::KeyboardF1, "KeyboardF1", "キーボード / F1", Keys::F1 },
        ControlDescriptor{ InputControl::KeyboardF2, "KeyboardF2", "キーボード / F2", Keys::F2 },
        ControlDescriptor{ InputControl::KeyboardF3, "KeyboardF3", "キーボード / F3", Keys::F3 },
        ControlDescriptor{ InputControl::KeyboardF4, "KeyboardF4", "キーボード / F4", Keys::F4 },
        ControlDescriptor{ InputControl::KeyboardF5, "KeyboardF5", "キーボード / F5", Keys::F5 },
        ControlDescriptor{ InputControl::KeyboardF6, "KeyboardF6", "キーボード / F6", Keys::F6 },
        ControlDescriptor{ InputControl::KeyboardF7, "KeyboardF7", "キーボード / F7", Keys::F7 },
        ControlDescriptor{ InputControl::KeyboardF8, "KeyboardF8", "キーボード / F8", Keys::F8 },
        ControlDescriptor{ InputControl::KeyboardF9, "KeyboardF9", "キーボード / F9", Keys::F9 },
        ControlDescriptor{ InputControl::KeyboardF10, "KeyboardF10", "キーボード / F10", Keys::F10 },
        ControlDescriptor{ InputControl::KeyboardF11, "KeyboardF11", "キーボード / F11", Keys::F11 },
        ControlDescriptor{ InputControl::KeyboardF12, "KeyboardF12", "キーボード / F12", Keys::F12 },
        ControlDescriptor{ InputControl::KeyboardSpace, "KeyboardSpace", "キーボード / Space", Keys::Space },
        ControlDescriptor{ InputControl::KeyboardEnter, "KeyboardEnter", "キーボード / Enter", Keys::Enter },
        ControlDescriptor{ InputControl::KeyboardEscape, "KeyboardEscape", "キーボード / Escape", Keys::Escape },
        ControlDescriptor{ InputControl::KeyboardTab, "KeyboardTab", "キーボード / Tab", Keys::Tab },
        ControlDescriptor{ InputControl::KeyboardBackspace, "KeyboardBackspace", "キーボード / Backspace", Keys::Back },
        ControlDescriptor{ InputControl::KeyboardDelete, "KeyboardDelete", "キーボード / Delete", Keys::Delete },
        ControlDescriptor{ InputControl::KeyboardInsert, "KeyboardInsert", "キーボード / Insert", Keys::Insert },
        ControlDescriptor{ InputControl::KeyboardHome, "KeyboardHome", "キーボード / Home", Keys::Home },
        ControlDescriptor{ InputControl::KeyboardEnd, "KeyboardEnd", "キーボード / End", Keys::End },
        ControlDescriptor{ InputControl::KeyboardPageUp, "KeyboardPageUp", "キーボード / PageUp", Keys::PageUp },
        ControlDescriptor{ InputControl::KeyboardPageDown, "KeyboardPageDown", "キーボード / PageDown", Keys::PageDown },
        ControlDescriptor{ InputControl::KeyboardUp, "KeyboardUp", "キーボード / ↑", Keys::Up },
        ControlDescriptor{ InputControl::KeyboardDown, "KeyboardDown", "キーボード / ↓", Keys::Down },
        ControlDescriptor{ InputControl::KeyboardLeft, "KeyboardLeft", "キーボード / ←", Keys::Left },
        ControlDescriptor{ InputControl::KeyboardRight, "KeyboardRight", "キーボード / →", Keys::Right },
        ControlDescriptor{ InputControl::KeyboardLeftShift, "KeyboardLeftShift", "キーボード / 左Shift", Keys::LeftShift },
        ControlDescriptor{ InputControl::KeyboardRightShift, "KeyboardRightShift", "キーボード / 右Shift", Keys::RightShift },
        ControlDescriptor{ InputControl::KeyboardLeftControl, "KeyboardLeftControl", "キーボード / 左Ctrl", Keys::LeftControl },
        ControlDescriptor{ InputControl::KeyboardRightControl, "KeyboardRightControl", "キーボード / 右Ctrl", Keys::RightControl },
        ControlDescriptor{ InputControl::KeyboardLeftAlt, "KeyboardLeftAlt", "キーボード / 左Alt", Keys::LeftAlt },
        ControlDescriptor{ InputControl::KeyboardRightAlt, "KeyboardRightAlt", "キーボード / 右Alt", Keys::RightAlt },
        ControlDescriptor{ InputControl::MouseLeft, "MouseLeft", "マウス / 左ボタン" },
        ControlDescriptor{ InputControl::MouseRight, "MouseRight", "マウス / 右ボタン" },
        ControlDescriptor{ InputControl::MouseMiddle, "MouseMiddle", "マウス / 中ボタン" },
        ControlDescriptor{ InputControl::MouseX1, "MouseX1", "マウス / X1ボタン" },
        ControlDescriptor{ InputControl::MouseX2, "MouseX2", "マウス / X2ボタン" },
        ControlDescriptor{ InputControl::MouseWheelUp, "MouseWheelUp", "マウス / ホイール上" },
        ControlDescriptor{ InputControl::MouseWheelDown, "MouseWheelDown", "マウス / ホイール下" },
        ControlDescriptor{ InputControl::GamePadA, "GamePadA", "ゲームパッド / A" },
        ControlDescriptor{ InputControl::GamePadB, "GamePadB", "ゲームパッド / B" },
        ControlDescriptor{ InputControl::GamePadX, "GamePadX", "ゲームパッド / X" },
        ControlDescriptor{ InputControl::GamePadY, "GamePadY", "ゲームパッド / Y" },
        ControlDescriptor{ InputControl::GamePadDPadUp, "GamePadDPadUp", "ゲームパッド / 十字上" },
        ControlDescriptor{ InputControl::GamePadDPadDown, "GamePadDPadDown", "ゲームパッド / 十字下" },
        ControlDescriptor{ InputControl::GamePadDPadLeft, "GamePadDPadLeft", "ゲームパッド / 十字左" },
        ControlDescriptor{ InputControl::GamePadDPadRight, "GamePadDPadRight", "ゲームパッド / 十字右" },
        ControlDescriptor{ InputControl::GamePadLeftShoulder, "GamePadLeftShoulder", "ゲームパッド / LB" },
        ControlDescriptor{ InputControl::GamePadRightShoulder, "GamePadRightShoulder", "ゲームパッド / RB" },
        ControlDescriptor{ InputControl::GamePadBack, "GamePadBack", "ゲームパッド / Back" },
        ControlDescriptor{ InputControl::GamePadStart, "GamePadStart", "ゲームパッド / Start" },
        ControlDescriptor{ InputControl::GamePadLeftStickButton, "GamePadLeftStickButton", "ゲームパッド / 左Stick押込" },
        ControlDescriptor{ InputControl::GamePadRightStickButton, "GamePadRightStickButton", "ゲームパッド / 右Stick押込" },
        ControlDescriptor{ InputControl::GamePadLeftX, "GamePadLeftX", "ゲームパッド / 左Stick X" },
        ControlDescriptor{ InputControl::GamePadLeftY, "GamePadLeftY", "ゲームパッド / 左Stick Y" },
        ControlDescriptor{ InputControl::GamePadRightX, "GamePadRightX", "ゲームパッド / 右Stick X" },
        ControlDescriptor{ InputControl::GamePadRightY, "GamePadRightY", "ゲームパッド / 右Stick Y" },
        ControlDescriptor{ InputControl::GamePadLeftTrigger, "GamePadLeftTrigger", "ゲームパッド / LT" },
        ControlDescriptor{ InputControl::GamePadRightTrigger, "GamePadRightTrigger", "ゲームパッド / RT" }
    };

    static_assert(
        ControlDescriptors.size()
        == static_cast<std::size_t>(InputControl::Count));

    consteval bool ControlDescriptorsAreOrdered()
    {
        for (std::size_t index = 0;
            index < ControlDescriptors.size();
            ++index)
        {
            if (ControlDescriptors[index].control
                != static_cast<InputControl>(index))
            {
                return false;
            }
        }
        return true;
    }

    static_assert(
        ControlDescriptorsAreOrdered(),
        "ControlDescriptors must follow InputControl order.");

    DirectX::Keyboard::Keys KeyboardKey(
        const InputControl control)
    {
        const auto index =
            static_cast<std::size_t>(control);
        if (!LamaPon::IsKeyboardControl(control))
        {
            throw std::invalid_argument(
                "Input control is not a keyboard key.");
        }
        return ControlDescriptors[index].key;
    }

    // Keys列挙子の値はWin32仮想キーコードと一致します。
    const std::array<InputControl, 256>& VirtualKeyControls()
    {
        static const auto table = []
        {
            std::array<InputControl, 256> result{};
            result.fill(InputControl::Count);
            for (const auto& descriptor : ControlDescriptors)
            {
                if (LamaPon::IsKeyboardControl(
                    descriptor.control))
                {
                    result[static_cast<std::size_t>(
                        descriptor.key)] =
                        descriptor.control;
                }
            }
            return result;
        }();
        return table;
    }

    InputControl KeyboardControlFromMessage(
        const std::uint64_t wParam,
        const std::int64_t lParam) noexcept
    {
        auto virtualKey = static_cast<UINT>(wParam);
        const bool extended =
            (lParam & (std::int64_t{ 1 } << 24)) != 0;
        if (virtualKey == VK_SHIFT)
        {
            const auto scanCode = static_cast<UINT>(
                (lParam >> 16) & 0xFF);
            virtualKey = MapVirtualKeyW(
                scanCode,
                MAPVK_VSC_TO_VK_EX);
        }
        else if (virtualKey == VK_CONTROL)
        {
            virtualKey = extended
                ? VK_RCONTROL
                : VK_LCONTROL;
        }
        else if (virtualKey == VK_MENU)
        {
            virtualKey = extended ? VK_RMENU : VK_LMENU;
        }
        return virtualKey < 256
            ? VirtualKeyControls()[virtualKey]
            : InputControl::Count;
    }

    float MouseControlValue(
        const LamaPon::InputPointerState& pointer,
        const InputControl control) noexcept
    {
        const auto button = [&](const PointerButton index)
        {
            const auto& state = pointer.Button(index);
            return state.down || state.pressed
                ? 1.0f
                : 0.0f;
        };
        switch (control)
        {
        case InputControl::MouseLeft: return button(PointerButton::Left);
        case InputControl::MouseRight: return button(PointerButton::Right);
        case InputControl::MouseMiddle: return button(PointerButton::Middle);
        case InputControl::MouseX1: return button(PointerButton::Extra1);
        case InputControl::MouseX2: return button(PointerButton::Extra2);
        case InputControl::MouseWheelUp: return pointer.wheel > 0.0f ? 1.0f : 0.0f;
        case InputControl::MouseWheelDown: return pointer.wheel < 0.0f ? 1.0f : 0.0f;
        default: return 0.0f;
        }
    }

    float GamePadValue(
        const DirectX::GamePad::State& state,
        const InputControl control) noexcept
    {
        if (!state.IsConnected())
        {
            return 0.0f;
        }

        switch (control)
        {
        case InputControl::GamePadA: return state.buttons.a ? 1.0f : 0.0f;
        case InputControl::GamePadB: return state.buttons.b ? 1.0f : 0.0f;
        case InputControl::GamePadX: return state.buttons.x ? 1.0f : 0.0f;
        case InputControl::GamePadY: return state.buttons.y ? 1.0f : 0.0f;
        case InputControl::GamePadDPadUp: return state.dpad.up ? 1.0f : 0.0f;
        case InputControl::GamePadDPadDown: return state.dpad.down ? 1.0f : 0.0f;
        case InputControl::GamePadDPadLeft: return state.dpad.left ? 1.0f : 0.0f;
        case InputControl::GamePadDPadRight: return state.dpad.right ? 1.0f : 0.0f;
        case InputControl::GamePadLeftShoulder: return state.buttons.leftShoulder ? 1.0f : 0.0f;
        case InputControl::GamePadRightShoulder: return state.buttons.rightShoulder ? 1.0f : 0.0f;
        case InputControl::GamePadBack: return state.buttons.back ? 1.0f : 0.0f;
        case InputControl::GamePadStart: return state.buttons.start ? 1.0f : 0.0f;
        case InputControl::GamePadLeftStickButton: return state.buttons.leftStick ? 1.0f : 0.0f;
        case InputControl::GamePadRightStickButton: return state.buttons.rightStick ? 1.0f : 0.0f;
        case InputControl::GamePadLeftX: return state.thumbSticks.leftX;
        case InputControl::GamePadLeftY: return state.thumbSticks.leftY;
        case InputControl::GamePadRightX: return state.thumbSticks.rightX;
        case InputControl::GamePadRightY: return state.thumbSticks.rightY;
        case InputControl::GamePadLeftTrigger: return state.triggers.left;
        case InputControl::GamePadRightTrigger: return state.triggers.right;
        default: return 0.0f;
        }
    }

    constexpr std::array PointerButtonVirtualKeys{
        VK_LBUTTON,
        VK_RBUTTON,
        VK_MBUTTON,
        VK_XBUTTON1,
        VK_XBUTTON2
    };

    static_assert(
        PointerButtonVirtualKeys.size()
        == static_cast<std::size_t>(PointerButton::Count));

    // ウィンドウプロシージャとInputSystem::Updateはどちらも
    // メインスレッドで動くため、ロックなしのメンバー状態で十分です。
    LamaPon::InputSystem* g_activeInputSystem{};
}

namespace LamaPon
{
    void InputSnapshot::Set(
        const InputControl control,
        const float value)
    {
        values[control] = std::clamp(value, -1.0f, 1.0f);
    }

    float InputSnapshot::Get(
        const InputControl control) const noexcept
    {
        const auto found = values.find(control);
        return found != values.end()
            ? found->second
            : 0.0f;
    }

    std::vector<InputActionDefinition>
        DefaultInputActions()
    {
        return {
            {
                "MoveHorizontal",
                {
                    { InputControl::KeyboardA, -1.0f },
                    { InputControl::KeyboardD, 1.0f },
                    { InputControl::GamePadLeftX, 1.0f }
                }
            },
            {
                "MoveVertical",
                {
                    { InputControl::KeyboardS, -1.0f },
                    { InputControl::KeyboardW, 1.0f },
                    { InputControl::GamePadLeftY, 1.0f }
                }
            },
            {
                "LookHorizontal",
                { { InputControl::GamePadRightX, 1.0f } }
            },
            {
                "LookVertical",
                { { InputControl::GamePadRightY, 1.0f } }
            },
            {
                "Jump",
                {
                    { InputControl::KeyboardSpace, 1.0f },
                    { InputControl::GamePadA, 1.0f }
                }
            },
            {
                "Fire",
                {
                    { InputControl::MouseLeft, 1.0f },
                    { InputControl::GamePadRightTrigger, 1.0f }
                }
            },
            {
                "AltFire",
                {
                    { InputControl::MouseRight, 1.0f },
                    { InputControl::GamePadLeftTrigger, 1.0f }
                }
            },
            {
                "Submit",
                {
                    { InputControl::KeyboardEnter, 1.0f },
                    { InputControl::GamePadA, 1.0f }
                }
            },
            {
                "Cancel",
                {
                    { InputControl::KeyboardEscape, 1.0f },
                    { InputControl::GamePadB, 1.0f }
                }
            }
        };
    }

    void ValidateInputActions(
        const std::span<const InputActionDefinition> actions)
    {
        if (actions.empty() || actions.size() > 64)
        {
            throw std::invalid_argument(
                "Input actions must contain between 1 and 64 actions.");
        }

        std::unordered_set<std::string> names;
        for (const auto& action : actions)
        {
            if (action.name.empty()
                || action.name.size() > 64
                || action.name.find_first_not_of(" \t\r\n")
                    == std::string::npos)
            {
                throw std::invalid_argument(
                    "Input action names must contain 1 to 64 bytes.");
            }
            if (!names.insert(action.name).second)
            {
                throw std::invalid_argument(
                    "Input action names must be unique: "
                    + action.name);
            }
            if (action.bindings.empty()
                || action.bindings.size() > 16)
            {
                throw std::invalid_argument(
                    "Each input action requires 1 to 16 bindings: "
                    + action.name);
            }
            for (const auto& binding : action.bindings)
            {
                if (binding.control < InputControl{}
                    || binding.control >= InputControl::Count
                    || !std::isfinite(binding.scale)
                    || std::abs(binding.scale) < 0.001f
                    || std::abs(binding.scale) > 4.0f)
                {
                    throw std::invalid_argument(
                        "Input binding is invalid: "
                        + action.name);
                }
            }
        }
    }

    std::span<const InputControl>
        AllInputControls() noexcept
    {
        static const auto controls = []
        {
            std::array<
                InputControl,
                static_cast<std::size_t>(
                    InputControl::Count)> result{};
            for (std::size_t index = 0;
                index < result.size();
                ++index)
            {
                result[index] =
                    static_cast<InputControl>(index);
            }
            return result;
        }();
        return controls;
    }

    std::string_view InputControlName(
        const InputControl control) noexcept
    {
        const auto index =
            static_cast<std::size_t>(control);
        return index < ControlDescriptors.size()
            ? ControlDescriptors[index].name
            : "Unknown";
    }

    std::string_view InputControlDisplayName(
        const InputControl control) noexcept
    {
        const auto index =
            static_cast<std::size_t>(control);
        return index < ControlDescriptors.size()
            ? ControlDescriptors[index].displayName
            : "不明";
    }

    InputControl InputControlFromName(
        const std::string_view name)
    {
        for (const auto& descriptor : ControlDescriptors)
        {
            if (descriptor.name == name)
            {
                return descriptor.control;
            }
        }
        throw std::invalid_argument(
            "Unknown input control: "
            + std::string(name));
    }

    InputSystem::InputSystem(
        void* const nativeWindow)
        : m_keyboard(std::make_unique<DirectX::Keyboard>())
        , m_gamePad(std::make_unique<DirectX::GamePad>())
        , m_nativeWindow(nativeWindow)
        , m_actions(DefaultInputActions())
    {
        ValidateInputActions(m_actions);
        g_activeInputSystem = this;
    }

    InputSystem::~InputSystem()
    {
        if (g_activeInputSystem == this)
        {
            g_activeInputSystem = nullptr;
        }
    }

    void InputSystem::ProcessWindowMessage(
        const std::uint32_t message,
        const std::uint64_t wParam,
        const std::int64_t lParam) noexcept
    {
        if (g_activeInputSystem != nullptr)
        {
            g_activeInputSystem->HandleWindowMessage(
                message,
                wParam,
                lParam);
        }
    }

    void InputSystem::HandleWindowMessage(
        const std::uint32_t message,
        const std::uint64_t wParam,
        const std::int64_t lParam) noexcept
    {
        const auto pressKey = [&]
        {
            const auto control =
                KeyboardControlFromMessage(wParam, lParam);
            if (control == InputControl::Count)
            {
                return;
            }
            auto& state = m_keyEvents[
                static_cast<std::size_t>(control)];
            const bool wasDown =
                (lParam & (std::int64_t{ 1 } << 30)) != 0;
            state.down = true;
            if (!wasDown && state.pressedCount < 255)
            {
                ++state.pressedCount;
            }
        };
        const auto releaseKey = [&]
        {
            const auto control =
                KeyboardControlFromMessage(wParam, lParam);
            if (control == InputControl::Count)
            {
                return;
            }
            auto& state = m_keyEvents[
                static_cast<std::size_t>(control)];
            state.down = false;
            if (state.releasedCount < 255)
            {
                ++state.releasedCount;
            }
        };
        const auto pressButton = [&](
            const PointerButton button)
        {
            auto& state = m_mouseButtonEvents[
                static_cast<std::size_t>(button)];
            state.down = true;
            if (state.pressedCount < 255)
            {
                ++state.pressedCount;
            }
        };
        const auto releaseButton = [&](
            const PointerButton button)
        {
            auto& state = m_mouseButtonEvents[
                static_cast<std::size_t>(button)];
            state.down = false;
            if (state.releasedCount < 255)
            {
                ++state.releasedCount;
            }
        };
        const auto extraButton = [&]
        {
            return GET_XBUTTON_WPARAM(
                static_cast<WPARAM>(wParam)) == XBUTTON1
                ? PointerButton::Extra1
                : PointerButton::Extra2;
        };

        switch (message)
        {
        case WM_ACTIVATEAPP:
            if (wParam == 0)
            {
                ResetEventStates();
            }
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            pressKey();
            break;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            releaseKey();
            break;
        case WM_LBUTTONDOWN:
            pressButton(PointerButton::Left);
            break;
        case WM_LBUTTONUP:
            releaseButton(PointerButton::Left);
            break;
        case WM_RBUTTONDOWN:
            pressButton(PointerButton::Right);
            break;
        case WM_RBUTTONUP:
            releaseButton(PointerButton::Right);
            break;
        case WM_MBUTTONDOWN:
            pressButton(PointerButton::Middle);
            break;
        case WM_MBUTTONUP:
            releaseButton(PointerButton::Middle);
            break;
        case WM_XBUTTONDOWN:
            pressButton(extraButton());
            break;
        case WM_XBUTTONUP:
            releaseButton(extraButton());
            break;
        case WM_MOUSEWHEEL:
            m_wheelAccumulator += GET_WHEEL_DELTA_WPARAM(
                static_cast<WPARAM>(wParam));
            break;
        case WM_MOUSEHWHEEL:
            m_wheelHorizontalAccumulator +=
                GET_WHEEL_DELTA_WPARAM(
                    static_cast<WPARAM>(wParam));
            break;
        case WM_CHAR:
        case WM_IME_CHAR:
        {
            const auto character =
                static_cast<wchar_t>(wParam);
            // テキスト編集で使う制御文字だけを通します。
            const bool editingControl =
                character == L'\b'
                || character == L'\r'
                || character == L'\t';
            if ((character >= 0x20
                    && character != 0x7F)
                || editingControl)
            {
                if (m_textInputAccumulator.size() < 256)
                {
                    m_textInputAccumulator.push_back(
                        character);
                }
            }
            break;
        }
        default:
            break;
        }
    }

    void InputSystem::ResetEventStates() noexcept
    {
        m_keyEvents = {};
        m_mouseButtonEvents = {};
        m_wheelAccumulator = 0;
        m_wheelHorizontalAccumulator = 0;
        m_textInputAccumulator.clear();
        m_frameTextInput.clear();
        m_hasLastCursorPosition = false;
    }

    void InputSystem::ClearFrameEvents() noexcept
    {
        for (auto& state : m_keyEvents)
        {
            state.pressedCount = 0;
            state.releasedCount = 0;
        }
        for (auto& state : m_mouseButtonEvents)
        {
            state.pressedCount = 0;
            state.releasedCount = 0;
        }
        m_wheelAccumulator = 0;
        m_wheelHorizontalAccumulator = 0;
        // 蓄積分を今フレームのテキスト入力として公開します。
        m_frameTextInput.swap(m_textInputAccumulator);
        m_textInputAccumulator.clear();
    }

    void InputSystem::SetActions(
        std::vector<InputActionDefinition> actions)
    {
        ValidateInputActions(actions);
        m_actions = std::move(actions);
        m_values.clear();
        m_previousValues.clear();
    }

    void InputSystem::Update(
        const bool allowKeyboardActions)
    {
        m_keyboardState = m_keyboard->GetState();
        m_gamePadState = m_gamePad->GetState(
            0,
            DirectX::GamePad::DEAD_ZONE_INDEPENDENT_AXES);

        // ポインタ状態はマウスバインディングの入力源なので、
        // Actionスナップショット構築前に更新します。
        UpdatePointer();

        InputSnapshot snapshot;
        for (const auto control : AllInputControls())
        {
            float value{};
            if (IsKeyboardControl(control))
            {
                if (allowKeyboardActions)
                {
                    const auto& events = m_keyEvents[
                        static_cast<std::size_t>(control)];
                    if (m_keyboardState.IsKeyDown(
                            KeyboardKey(control))
                        || events.down
                        || events.pressedCount > 0)
                    {
                        value = 1.0f;
                    }
                }
            }
            else if (IsMouseControl(control))
            {
                value = MouseControlValue(
                    m_pointerState,
                    control);
            }
            else
            {
                value = GamePadValue(
                    m_gamePadState,
                    control);
            }
            if (value != 0.0f)
            {
                snapshot.Set(control, value);
            }
        }
        ApplySnapshot(snapshot);
        ClearFrameEvents();
    }

    void InputSystem::UpdateFromSnapshot(
        const InputSnapshot& snapshot)
    {
        UpdatePointer();
        ApplySnapshot(snapshot);
        ClearFrameEvents();
    }

    void InputSystem::UpdatePointer() noexcept
    {
        InputPointerState next;
        bool fromOverride = false;
        if (m_pointerOverride)
        {
            next = *m_pointerOverride;
            m_pointerOverride.reset();
            fromOverride = true;
            // ボタン別状態が導入される前の呼び出し元は、集約left
            // ボタンフラグ（down）だけを設定します。
            auto& left = next.buttons[
                static_cast<std::size_t>(
                    PointerButton::Left)];
            left.down = left.down || next.down;
            m_hasLastCursorPosition = false;
        }
        else if (m_nativeWindow != nullptr)
        {
            const auto window =
                static_cast<HWND>(m_nativeWindow);
            POINT cursor{};
            RECT client{};
            if (GetCursorPos(&cursor)
                && ScreenToClient(window, &cursor)
                && GetClientRect(window, &client))
            {
                next.position = {
                    static_cast<float>(cursor.x),
                    static_cast<float>(cursor.y)
                };
                next.valid =
                    cursor.x >= 0
                    && cursor.y >= 0
                    && cursor.x
                        < client.right
                    && cursor.y
                        < client.bottom;
                if (m_hasLastCursorPosition)
                {
                    next.delta = {
                        next.position.x
                            - m_lastCursorPosition.x,
                        next.position.y
                            - m_lastCursorPosition.y
                    };
                }
                m_lastCursorPosition = next.position;
                m_hasLastCursorPosition = true;
            }
            else
            {
                m_hasLastCursorPosition = false;
            }

            for (std::size_t index = 0;
                index < next.buttons.size();
                ++index)
            {
                // ウィンドウメッセージが主な入力源。非同期ポーリングは
                // クライアント領域外へのドラッグ継続を保証します。
                next.buttons[index].down =
                    m_mouseButtonEvents[index].down
                    || (GetAsyncKeyState(
                            PointerButtonVirtualKeys[index])
                        & 0x8000) != 0;
            }
            next.wheel =
                static_cast<float>(m_wheelAccumulator)
                / static_cast<float>(WHEEL_DELTA);
            next.wheelHorizontal =
                static_cast<float>(
                    m_wheelHorizontalAccumulator)
                / static_cast<float>(WHEEL_DELTA);
        }

        for (std::size_t index = 0;
            index < next.buttons.size();
            ++index)
        {
            auto& button = next.buttons[index];
            const auto& previous =
                m_pointerState.buttons[index];
            // 1フレーム内で押して離した場合も、押下と解放の両方の
            // 遷移を報告します。
            const bool tapped =
                !fromOverride
                && m_mouseButtonEvents[index].pressedCount > 0
                && !previous.down;
            button.pressed =
                next.valid
                && ((button.down && !previous.down)
                    || tapped);
            button.released =
                (previous.down && !button.down)
                || (tapped && !button.down);
        }

        const auto& left = next.buttons[
            static_cast<std::size_t>(PointerButton::Left)];
        next.down = left.down;
        next.pressed = left.pressed;
        next.released = left.released;
        m_pointerState = next;
    }

    float InputSystem::Value(
        const std::string_view action) const noexcept
    {
        const auto found =
            m_values.find(std::string(action));
        return found != m_values.end()
            ? found->second
            : 0.0f;
    }

    bool InputSystem::IsDown(
        const std::string_view action,
        const float threshold) const noexcept
    {
        return std::abs(Value(action))
            >= std::abs(threshold);
    }

    bool InputSystem::WasPressed(
        const std::string_view action,
        const float threshold) const noexcept
    {
        const float absoluteThreshold =
            std::abs(threshold);
        const float previous = [&]
        {
            const auto found = m_previousValues.find(
                std::string(action));
            return found != m_previousValues.end()
                ? found->second
                : 0.0f;
        }();
        return std::abs(Value(action)) >= absoluteThreshold
            && std::abs(previous) < absoluteThreshold;
    }

    bool InputSystem::WasReleased(
        const std::string_view action,
        const float threshold) const noexcept
    {
        const float absoluteThreshold =
            std::abs(threshold);
        const float previous = [&]
        {
            const auto found = m_previousValues.find(
                std::string(action));
            return found != m_previousValues.end()
                ? found->second
                : 0.0f;
        }();
        return std::abs(Value(action)) < absoluteThreshold
            && std::abs(previous) >= absoluteThreshold;
    }

    void InputSystem::ApplySnapshot(
        const InputSnapshot& snapshot)
    {
        m_previousValues = m_values;
        m_values.clear();
        for (const auto& action : m_actions)
        {
            m_values.emplace(
                action.name,
                ActionValue(action, snapshot));
        }
    }

    float InputSystem::ActionValue(
        const InputActionDefinition& action,
        const InputSnapshot& snapshot) noexcept
    {
        float value{};
        for (const auto& binding : action.bindings)
        {
            value += snapshot.Get(binding.control)
                * binding.scale;
        }
        return std::clamp(value, -1.0f, 1.0f);
    }
}
