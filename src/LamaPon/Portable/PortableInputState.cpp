#include "LamaPon/Portable/PortableInputState.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace LamaPon::Portable
{
    namespace
    {
        std::string KeyboardCodeForControl(const std::string_view control)
        {
            constexpr std::string_view prefix = "Keyboard";
            if (!control.starts_with(prefix)) return {};
            const auto key = control.substr(prefix.size());
            if (key.size() == 1)
            {
                if (key.front() >= 'A' && key.front() <= 'Z') return "Key" + std::string(key);
                if (key.front() >= '0' && key.front() <= '9') return "Digit" + std::string(key);
            }
            if (key.starts_with("Alpha") && key.size() == 6
                && key.back() >= '0' && key.back() <= '9')
                return "Digit" + std::string(1, key.back());
            if (key.size() >= 2 && key.front() == 'F'
                && key.size() <= 3 && key[1] >= '1' && key[1] <= '9'
                && (key.size() == 2 || (key[1] == '1' && key[2] >= '0' && key[2] <= '2')))
                return std::string(key);

            static constexpr std::pair<std::string_view, std::string_view> namedKeys[]{
                { "Space", "Space" }, { "Enter", "Enter" }, { "Escape", "Escape" },
                { "Tab", "Tab" }, { "Backspace", "Backspace" }, { "Delete", "Delete" },
                { "Insert", "Insert" }, { "Home", "Home" }, { "End", "End" },
                { "PageUp", "PageUp" }, { "PageDown", "PageDown" },
                { "Up", "ArrowUp" }, { "Down", "ArrowDown" },
                { "Left", "ArrowLeft" }, { "Right", "ArrowRight" },
                { "LeftShift", "ShiftLeft" }, { "RightShift", "ShiftRight" },
                { "LeftControl", "ControlLeft" }, { "RightControl", "ControlRight" },
                { "LeftAlt", "AltLeft" }, { "RightAlt", "AltRight" },
            };
            for (const auto& [name, code] : namedKeys)
                if (key == name) return std::string(code);
            return {};
        }
    }

    void InputState::KeyEvent(const std::string_view code, const bool down)
    {
        if (code.empty()) return;
        const std::string key(code);
        if (down)
        {
            if (m_down.insert(key).second) m_pressed.insert(key);
        }
        else if (m_down.erase(key) != 0) m_released.insert(key);
    }

    void InputState::PointerEvent(const float x, const float y) noexcept
    {
        if (!std::isfinite(x) || !std::isfinite(y)) return;
        if (m_pointerValid)
        {
            m_pointerDeltaX += x - m_pointerX;
            m_pointerDeltaY += y - m_pointerY;
        }
        m_pointerX = x;
        m_pointerY = y;
        m_pointerValid = true;
    }

    void InputState::ButtonEvent(const int button, const bool down) noexcept
    {
        if (button < 0 || button >= static_cast<int>(m_pointerButtonsDown.size())) return;
        const auto index = static_cast<std::size_t>(button);
        if (down && !m_pointerButtonsDown[index]) m_pointerButtonsPressed[index] = true;
        if (!down && m_pointerButtonsDown[index]) m_pointerButtonsReleased[index] = true;
        m_pointerButtonsDown[index] = down;
    }

    void InputState::WheelEvent(const float amount) noexcept
    {
        if (std::isfinite(amount)) m_pointerWheel += amount;
    }

    void InputState::GamepadSnapshot(const std::array<bool, 32>& buttons,
        const std::array<float, 6>& axes) noexcept
    {
        for (std::size_t index = 0; index < buttons.size(); ++index)
        {
            m_gamepadPressed[index] = m_gamepadPressed[index] || (buttons[index] && !m_gamepadDown[index]);
            m_gamepadReleased[index] = m_gamepadReleased[index] || (!buttons[index] && m_gamepadDown[index]);
        }
        m_gamepadDown = buttons;
        const auto axis = [&](const std::size_t index, const float minimum)
        { return std::isfinite(axes[index]) ? std::clamp(axes[index], minimum, 1.0f) : 0.0f; };
        m_gamepadHorizontal = axis(0, -1);
        m_gamepadVertical = axis(1, -1);
        const std::array<float, 4> directions{
            m_gamepadVertical, -m_gamepadVertical, -m_gamepadHorizontal, m_gamepadHorizontal};
        for (std::size_t index = 0; index < directions.size(); ++index)
        {
            // 押下と解除に異なる閾値を使い、境界付近の揺れによる連続移動を防ぐ。
            const bool down = directions[index] >= (m_navigationDown[index] ? 0.35f : 0.55f);
            m_navigationPressed[index] = m_navigationPressed[index] || (down && !m_navigationDown[index]);
            m_navigationDown[index] = down;
        }
        m_gamepadRightHorizontal = axis(2, -1);
        m_gamepadRightVertical = axis(3, -1);
        m_gamepadBrake = axis(4, 0);
        m_gamepadAccelerate = axis(5, 0);
    }

    void InputState::Reset() noexcept
    {
        m_down.clear();
        EndFrame();
        m_gamepadDown.fill(false);
        m_gamepadPressed.fill(false);
        m_gamepadReleased.fill(false);
        m_gamepadHorizontal = m_gamepadVertical = 0.0f;
        m_navigationDown.fill(false);
        m_gamepadRightHorizontal = m_gamepadRightVertical = 0.0f;
        m_gamepadAccelerate = m_gamepadBrake = 0.0f;
        m_touchHorizontal = m_touchVertical = 0.0f;
        m_touchAccelerate = m_touchBrake = 0.0f;
        m_pointerButtonsDown.fill(false);
        m_pointerValid = false;
    }

    void InputState::EndFrame() noexcept
    {
        m_navigationPressed.fill(false);
        m_gamepadPressed.fill(false);
        m_gamepadReleased.fill(false);
        m_pressed.clear();
        m_released.clear();
        m_pointerButtonsPressed.fill(false);
        m_pointerButtonsReleased.fill(false);
        m_pointerDeltaX = 0.0f;
        m_pointerDeltaY = 0.0f;
        m_pointerWheel = 0.0f;
        m_touchToggleViewPressed = false;
    }

    bool InputState::IsDown(const char* code) const
    {
        return code != nullptr && m_down.contains(code);
    }

    bool InputState::WasPressed(const char* code) const
    {
        return code != nullptr && m_pressed.contains(code);
    }

    bool InputState::WasReleased(const char* code) const
    {
        return code != nullptr && m_released.contains(code);
    }

    float InputState::HorizontalAxis() const noexcept
    {
        return std::abs(m_touchHorizontal) > std::abs(m_gamepadHorizontal)
            ? m_touchHorizontal : m_gamepadHorizontal;
    }

    float InputState::VerticalAxis() const noexcept
    {
        return std::abs(m_touchVertical) > std::abs(m_gamepadVertical)
            ? m_touchVertical : m_gamepadVertical;
    }

    float InputState::AccelerateAxis() const noexcept
    {
        return std::max(m_touchAccelerate, m_gamepadAccelerate);
    }

    float InputState::BrakeAxis() const noexcept
    {
        return std::max(m_touchBrake, m_gamepadBrake);
    }

    bool InputState::WasGamepadPressed(int button) const noexcept
    {
        return button >= 0
            && static_cast<std::size_t>(button) < m_gamepadPressed.size()
            && m_gamepadPressed[static_cast<std::size_t>(button)];
    }

    float InputState::ControlValue(const std::string_view control) const
    {
        const std::string key = KeyboardCodeForControl(control);
        if (!key.empty())
        {
            return IsDown(key.c_str()) ? 1.0f : 0.0f;
        }
        if (control == "MouseLeft") return PointerButtonDown(0) ? 1.0f : 0.0f;
        if (control == "MouseRight") return PointerButtonDown(1) ? 1.0f : 0.0f;
        if (control == "MouseMiddle") return PointerButtonDown(2) ? 1.0f : 0.0f;
        if (control == "MouseX") return m_pointerDeltaX;
        if (control == "MouseY") return m_pointerDeltaY;
        if (control == "MouseWheel") return m_pointerWheel;
        if (control == "GamePadLeftX") return m_gamepadHorizontal;
        if (control == "GamePadLeftY") return m_gamepadVertical;
        if (control == "GamePadRightX") return m_gamepadRightHorizontal;
        if (control == "GamePadRightY") return m_gamepadRightVertical;
        if (control == "GamePadLeftTrigger") return m_gamepadBrake;
        if (control == "GamePadRightTrigger") return m_gamepadAccelerate;
        // 入力名とGamepad番号の対応表
        static const std::pair<std::string_view, std::size_t> buttons[] = {
            { "GamePadA", 0 }, { "GamePadB", 1 },
            { "GamePadX", 2 }, { "GamePadY", 3 },
            { "GamePadLeftShoulder", 4 }, { "GamePadRightShoulder", 5 },
            { "GamePadBack", 8 }, { "GamePadStart", 9 },
            { "GamePadLeftStick", 10 }, { "GamePadRightStick", 11 },
            { "GamePadDPadUp", 12 }, { "GamePadDPadDown", 13 },
            { "GamePadDPadLeft", 14 }, { "GamePadDPadRight", 15 },
        };
        // name: 入力名、button: Gamepad番号
        for (const auto& [name, button] : buttons)
        {
            if (control == name)
            {
                return m_gamepadDown[button] ? 1.0f : 0.0f;
            }
        }
        return 0.0f;
    }

    bool InputState::ControlWasPressed(const std::string_view control) const
    {
        const std::string key = KeyboardCodeForControl(control);
        if (!key.empty()) return WasPressed(key.c_str());
        if (control == "MouseLeft") return PointerButtonPressed(0);
        if (control == "MouseRight") return PointerButtonPressed(1);
        if (control == "MouseMiddle") return PointerButtonPressed(2);
        if (control == "GamePadLeftTrigger") return m_gamepadPressed[6];
        if (control == "GamePadRightTrigger") return m_gamepadPressed[7];
        // Gamepadボタン番号順の入力名
        static const std::string_view buttonNames[] = {
            "GamePadA", "GamePadB", "GamePadX", "GamePadY",
            "GamePadLeftShoulder", "GamePadRightShoulder", "", "",
            "GamePadBack", "GamePadStart", "GamePadLeftStick",
            "GamePadRightStick", "GamePadDPadUp", "GamePadDPadDown",
            "GamePadDPadLeft", "GamePadDPadRight",
        };
        // イベント・入力配列の要素番号
        for (std::size_t index{}; index < std::size(buttonNames); ++index)
        {
            if (control == buttonNames[index]) return m_gamepadPressed[index];
        }
        return false;
    }

    bool InputState::ControlWasReleased(const std::string_view control) const
    {
        const std::string key = KeyboardCodeForControl(control);
        if (!key.empty()) return WasReleased(key.c_str());
        if (control == "MouseLeft") return PointerButtonReleased(0);
        if (control == "MouseRight") return PointerButtonReleased(1);
        if (control == "MouseMiddle") return PointerButtonReleased(2);
        if (control == "GamePadLeftTrigger") return m_gamepadReleased[6];
        if (control == "GamePadRightTrigger") return m_gamepadReleased[7];
        // Gamepadボタン番号順の入力名
        static const std::string_view buttonNames[] = {
            "GamePadA", "GamePadB", "GamePadX", "GamePadY",
            "GamePadLeftShoulder", "GamePadRightShoulder", "", "",
            "GamePadBack", "GamePadStart", "GamePadLeftStick",
            "GamePadRightStick", "GamePadDPadUp", "GamePadDPadDown",
            "GamePadDPadLeft", "GamePadDPadRight",
        };
        // イベント・入力配列の要素番号
        for (std::size_t index{}; index < std::size(buttonNames); ++index)
        {
            if (control == buttonNames[index]) return m_gamepadReleased[index];
        }
        return false;
    }

    bool InputState::PointerButtonDown(int button) const noexcept
    {
        return button >= 0
            && static_cast<std::size_t>(button) < m_pointerButtonsDown.size()
            && m_pointerButtonsDown[static_cast<std::size_t>(button)];
    }

    bool InputState::PointerButtonPressed(int button) const noexcept
    {
        return button >= 0
            && static_cast<std::size_t>(button) < m_pointerButtonsPressed.size()
            && m_pointerButtonsPressed[static_cast<std::size_t>(button)];
    }

    bool InputState::PointerButtonReleased(int button) const noexcept
    {
        return button >= 0
            && static_cast<std::size_t>(button) < m_pointerButtonsReleased.size()
            && m_pointerButtonsReleased[static_cast<std::size_t>(button)];
    }

}
