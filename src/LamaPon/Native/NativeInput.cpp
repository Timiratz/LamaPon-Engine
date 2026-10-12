#include "LamaPon/Native/NativeInput.h"
#include <algorithm>
#include <cmath>

namespace LamaPon::Native
{
    namespace
    {
        std::string KeyCode(const SDL_Scancode code)
        {
            if (code >= SDL_SCANCODE_A && code <= SDL_SCANCODE_Z)
                return std::string("Key") + static_cast<char>('A' + code - SDL_SCANCODE_A);
            if (code >= SDL_SCANCODE_1 && code <= SDL_SCANCODE_9)
                return std::string("Digit") + static_cast<char>('1' + code - SDL_SCANCODE_1);
            if (code >= SDL_SCANCODE_F1 && code <= SDL_SCANCODE_F12)
                return "F" + std::to_string(1 + code - SDL_SCANCODE_F1);
            switch (code)
            {
            case SDL_SCANCODE_0: return "Digit0";
            case SDL_SCANCODE_SPACE: return "Space";
            case SDL_SCANCODE_RETURN: return "Enter";
            case SDL_SCANCODE_ESCAPE: return "Escape";
            case SDL_SCANCODE_AC_BACK: return "Escape";
            case SDL_SCANCODE_TAB: return "Tab";
            case SDL_SCANCODE_BACKSPACE: return "Backspace";
            case SDL_SCANCODE_DELETE: return "Delete";
            case SDL_SCANCODE_INSERT: return "Insert";
            case SDL_SCANCODE_HOME: return "Home";
            case SDL_SCANCODE_END: return "End";
            case SDL_SCANCODE_PAGEUP: return "PageUp";
            case SDL_SCANCODE_PAGEDOWN: return "PageDown";
            case SDL_SCANCODE_LEFT: return "ArrowLeft";
            case SDL_SCANCODE_RIGHT: return "ArrowRight";
            case SDL_SCANCODE_UP: return "ArrowUp";
            case SDL_SCANCODE_DOWN: return "ArrowDown";
            case SDL_SCANCODE_LSHIFT: return "ShiftLeft";
            case SDL_SCANCODE_RSHIFT: return "ShiftRight";
            case SDL_SCANCODE_LCTRL: return "ControlLeft";
            case SDL_SCANCODE_RCTRL: return "ControlRight";
            case SDL_SCANCODE_LALT: return "AltLeft";
            case SDL_SCANCODE_RALT: return "AltRight";
            default: return {};
            }
        }
        int PointerButton(const Uint8 button)
        {
            switch (button)
            {
            case SDL_BUTTON_LEFT: return 0;
            case SDL_BUTTON_RIGHT: return 1;
            case SDL_BUTTON_MIDDLE: return 2;
            case SDL_BUTTON_X1: return 3;
            case SDL_BUTTON_X2: return 4;
            default: return -1;
            }
        }
    }

    NativeInput::~NativeInput()
    {
        if (m_gamepad) SDL_CloseGamepad(m_gamepad);
    }

    bool NativeInput::Initialize(SDL_Window* window)
    {
        m_window = window;
        return window != nullptr;
    }

    bool NativeInput::SelectGamepad(const SDL_JoystickID id)
    {
        SDL_Gamepad* selected = id ? SDL_OpenGamepad(id) : nullptr;
        if (id && !selected) return false;
        if (m_gamepad) SDL_CloseGamepad(m_gamepad);
        m_gamepad = selected;
        m_preferredGamepad = id;
        GamepadSnapshot({}, {});
        return true;
    }

    void NativeInput::Reset() noexcept
    {
        Portable::InputState::Reset();
        m_touches.clear();
        m_hasPointerFinger = false;
        SDL_CaptureMouse(false);
    }

    void NativeInput::BeginFrame() noexcept { SampleGamepads(); }

    void NativeInput::MousePosition(const float x, const float y) noexcept
    {
        int width = 1, height = 1, pixelsWide = 1, pixelsHigh = 1;
        SDL_GetWindowSize(m_window, &width, &height);
        SDL_GetWindowSizeInPixels(m_window, &pixelsWide, &pixelsHigh);
        PointerEvent(x * static_cast<float>(pixelsWide) / std::max(width, 1),
            y * static_cast<float>(pixelsHigh) / std::max(height, 1));
    }

    void NativeInput::ProcessEvent(const SDL_Event& event)
    {
        switch (event.type)
        {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            KeyEvent(KeyCode(event.key.scancode), event.type == SDL_EVENT_KEY_DOWN);
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (event.motion.which != SDL_TOUCH_MOUSEID) MousePosition(event.motion.x, event.motion.y);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.which != SDL_TOUCH_MOUSEID)
            {
                MousePosition(event.button.x, event.button.y);
                ButtonEvent(PointerButton(event.button.button), event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                SDL_CaptureMouse(std::any_of(m_pointerButtonsDown.begin(), m_pointerButtonsDown.end(),
                    [](const bool down) { return down; }));
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            WheelEvent(event.wheel.y * (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f));
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
        case SDL_EVENT_DID_ENTER_BACKGROUND:
            Reset();
            break;
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED:
        {
            int width = 1, height = 1;
            SDL_GetWindowSizeInPixels(m_window, &width, &height);
            const TouchKey id{event.tfinger.touchID, event.tfinger.fingerID};
            if (event.type == SDL_EVENT_FINGER_DOWN && event.tfinger.x > 0.82f && event.tfinger.y < 0.22f)
                m_touchToggleViewPressed = true;
            if (!m_hasPointerFinger && event.type == SDL_EVENT_FINGER_DOWN)
            {
                m_pointerFinger = id;
                m_hasPointerFinger = true;
                PointerEvent(event.tfinger.x * width, event.tfinger.y * height);
                ButtonEvent(0, true);
            }
            if (m_hasPointerFinger && m_pointerFinger == id)
            {
                PointerEvent(event.tfinger.x * width, event.tfinger.y * height);
                if (event.type == SDL_EVENT_FINGER_UP) { ButtonEvent(0, false); m_hasPointerFinger = false; }
                if (event.type == SDL_EVENT_FINGER_CANCELED)
                {
                    m_pointerButtonsDown[0] = false;
                    m_pointerButtonsPressed[0] = m_pointerButtonsReleased[0] = false;
                    m_pointerValid = false;
                    m_hasPointerFinger = false;
                }
            }
            if (event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_CANCELED) m_touches.erase(id);
            else m_touches[id] = {event.tfinger.x, event.tfinger.y};
            RefreshTouchAxes();
            break;
        }
        default: break;
        }
    }

    void NativeInput::RefreshTouchAxes() noexcept
    {
        m_touchHorizontal = m_touchVertical = m_touchAccelerate = m_touchBrake = 0;
        for (const auto& [id, point] : m_touches)
        {
            static_cast<void>(id);
            const auto [x, y] = point;
            if (x > 0.82f && y < 0.22f) continue;
            if (x < 0.55f)
            {
                const float horizontal = std::clamp((x / 0.55f - 0.5f) * 2.0f, -1.0f, 1.0f);
                const float vertical = std::clamp((0.5f - y) * 2.0f, -1.0f, 1.0f);
                if (std::abs(horizontal) > 0.12f) m_touchHorizontal = horizontal;
                if (std::abs(vertical) > 0.12f) m_touchVertical = vertical;
            }
            else if (y < 0.58f) m_touchAccelerate = 1;
            else m_touchBrake = 1;
        }
    }

    void NativeInput::SampleGamepads() noexcept
    {
        if (m_gamepad && !SDL_GamepadConnected(m_gamepad))
        {
            SDL_CloseGamepad(m_gamepad);
            m_gamepad = nullptr;
            GamepadSnapshot({}, {});
        }
        if (!m_gamepad)
        {
            if (m_preferredGamepad)
            {
                if (SDL_IsGamepad(m_preferredGamepad)) m_gamepad = SDL_OpenGamepad(m_preferredGamepad);
            }
            else
            {
                int count{};
                SDL_JoystickID* ids = SDL_GetGamepads(&count);
                for (int index = 0; index < count && !m_gamepad; ++index) m_gamepad = SDL_OpenGamepad(ids[index]);
                SDL_free(ids);
            }
        }
        if (!m_gamepad) { GamepadSnapshot({}, {}); return; }
        std::array<bool, 32> buttons{};
        constexpr std::array<SDL_GamepadButton, 16> mapping{
            SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
            SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
            SDL_GAMEPAD_BUTTON_INVALID, SDL_GAMEPAD_BUTTON_INVALID, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START,
            SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_DPAD_UP,
            SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT};
        for (std::size_t index = 0; index < mapping.size(); ++index)
            if (mapping[index] != SDL_GAMEPAD_BUTTON_INVALID) buttons[index] = SDL_GetGamepadButton(m_gamepad, mapping[index]);
        std::array<float, 6> axes{};
        constexpr std::array<SDL_GamepadAxis, 6> axisMapping{
            SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX,
            SDL_GAMEPAD_AXIS_RIGHTY, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER};
        for (std::size_t index = 0; index < axes.size(); ++index)
        {
            float value = static_cast<float>(SDL_GetGamepadAxis(m_gamepad, axisMapping[index])) / 32767.0f;
            if (index < 4 && std::abs(value) < 0.18f) value = 0;
            axes[index] = (index == 1 || index == 3) ? -value : value;
        }
        buttons[6] = axes[4] > 0.5f;
        buttons[7] = axes[5] > 0.5f;
        GamepadSnapshot(buttons, axes);
    }
}
