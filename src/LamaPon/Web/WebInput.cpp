#include "LamaPon/Web/WebInput.h"

#include <emscripten/html5.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace LamaPon::Web
{
    namespace
    {
        // キーボードの登録先はwindowで共通なので、同時に所有できる
        // 入力インスタンスは1つです。別インスタンスの登録を奪いません。
        WebInput* activeInput{};
    }

    WebInput::~WebInput()
    {
        Shutdown();
    }

    void WebInput::Shutdown() noexcept
    {
        constexpr std::array eventTypes{
            EMSCRIPTEN_EVENT_KEYDOWN, EMSCRIPTEN_EVENT_KEYUP,
            EMSCRIPTEN_EVENT_TOUCHSTART, EMSCRIPTEN_EVENT_TOUCHMOVE,
            EMSCRIPTEN_EVENT_TOUCHEND, EMSCRIPTEN_EVENT_TOUCHCANCEL,
            EMSCRIPTEN_EVENT_MOUSEMOVE, EMSCRIPTEN_EVENT_MOUSEDOWN,
            EMSCRIPTEN_EVENT_MOUSEUP, EMSCRIPTEN_EVENT_WHEEL };
        // 成功した登録を、所有者とコールバックの組で解除します。
        // 同じDOM要素を使う別機能のイベントまでは解除しません。
        for (std::size_t index = 0; index < m_registered.size(); ++index)
        {
            if (m_registered[index])
            {
                emscripten_html5_remove_event_listener(
                    index < 2 ? EMSCRIPTEN_EVENT_TARGET_WINDOW : m_target.c_str(),
                    this, eventTypes[index], m_callbacks[index]);
            }
        }
        m_registered.fill(false);
        m_callbacks.fill(nullptr);
        m_initialized = false;
        if (activeInput == this)
        {
            activeInput = nullptr;
        }
    }

    bool WebInput::Initialize(const char* target)
    {
        if (m_initialized)
        {
            return true;
        }
        if (target == nullptr || *target == '\0'
            || (activeInput != nullptr && activeInput != this))
        {
            return false;
        }
        m_target = target;
        activeInput = this;
        const auto key = +[](int type, const EmscriptenKeyboardEvent* event,
                            void* owner) -> EM_BOOL
        {
            return HandleKeyEvent(type, event ? event->code : "", owner);
        };
        const auto touch = +[](int type, const EmscriptenTouchEvent* event,
                              void* owner) -> EM_BOOL
        {
            return HandleTouchEvent(type, event, owner);
        };
        const auto mouse = +[](int type, const EmscriptenMouseEvent* event,
                              void* owner) -> EM_BOOL
        {
            return HandleMouseEvent(type, event, owner);
        };
        const auto wheel = +[](int, const EmscriptenWheelEvent* event,
                              void* owner) -> EM_BOOL
        {
            return HandleWheelEvent(event, owner);
        };
        // 解除APIはvoid*でコールバックを識別するため、登録時に控えます。
        m_callbacks = {
            reinterpret_cast<void*>(key), reinterpret_cast<void*>(key),
            reinterpret_cast<void*>(touch), reinterpret_cast<void*>(touch),
            reinterpret_cast<void*>(touch), reinterpret_cast<void*>(touch),
            reinterpret_cast<void*>(mouse), reinterpret_cast<void*>(mouse),
            reinterpret_cast<void*>(mouse), reinterpret_cast<void*>(wheel) };
        // Canvasが非表示でもキーボード操作は継続できます。
        const char* window = EMSCRIPTEN_EVENT_TARGET_WINDOW;
        m_registered = {
            emscripten_set_keydown_callback(window, this, EM_TRUE, key) == EMSCRIPTEN_RESULT_SUCCESS,
            emscripten_set_keyup_callback(window, this, EM_TRUE, key) == EMSCRIPTEN_RESULT_SUCCESS,
            emscripten_set_touchstart_callback(target, this, EM_TRUE, touch) == EMSCRIPTEN_RESULT_SUCCESS,
            emscripten_set_touchmove_callback(target, this, EM_TRUE, touch) == EMSCRIPTEN_RESULT_SUCCESS,
            emscripten_set_touchend_callback(target, this, EM_TRUE, touch) == EMSCRIPTEN_RESULT_SUCCESS,
            emscripten_set_touchcancel_callback(target, this, EM_TRUE, touch) == EMSCRIPTEN_RESULT_SUCCESS,
            emscripten_set_mousemove_callback(target, this, EM_TRUE, mouse) == EMSCRIPTEN_RESULT_SUCCESS,
            emscripten_set_mousedown_callback(target, this, EM_TRUE, mouse) == EMSCRIPTEN_RESULT_SUCCESS,
            emscripten_set_mouseup_callback(target, this, EM_TRUE, mouse) == EMSCRIPTEN_RESULT_SUCCESS,
            emscripten_set_wheel_callback(target, this, EM_TRUE, wheel) == EMSCRIPTEN_RESULT_SUCCESS };
        m_initialized = std::all_of(
            m_registered.begin(), m_registered.end(),
            [](bool registered) { return registered; });
        if (!m_initialized)
        {
            // 初期化途中の成功分も戻し、同じインスタンスで再試行できます。
            Shutdown();
        }
        return m_initialized;
    }

    void WebInput::BeginFrame() noexcept
    {
        // ブラウザーイベントは描画フレームの直前に届く場合があります。
        // 押下状態はフレーム側で消費した後に消去します。
        SampleGamepads();
    }

    void WebInput::EndFrame() noexcept
    {
        m_pressed.clear();
        m_released.clear();
        m_pointerButtonsPressed.fill(false);
        m_pointerButtonsReleased.fill(false);
        m_pointerDeltaX = 0.0f;
        m_pointerDeltaY = 0.0f;
        m_pointerWheel = 0.0f;
        m_touchToggleViewPressed = false;
    }

    bool WebInput::IsDown(const char* code) const
    {
        return code != nullptr && m_down.contains(code);
    }

    bool WebInput::WasPressed(const char* code) const
    {
        return code != nullptr && m_pressed.contains(code);
    }

    bool WebInput::WasReleased(const char* code) const
    {
        return code != nullptr && m_released.contains(code);
    }

    float WebInput::HorizontalAxis() const noexcept
    {
        return std::abs(m_touchHorizontal) > std::abs(m_gamepadHorizontal)
            ? m_touchHorizontal : m_gamepadHorizontal;
    }

    float WebInput::VerticalAxis() const noexcept
    {
        return std::abs(m_touchVertical) > std::abs(m_gamepadVertical)
            ? m_touchVertical : m_gamepadVertical;
    }

    float WebInput::AccelerateAxis() const noexcept
    {
        return std::max(m_touchAccelerate, m_gamepadAccelerate);
    }

    float WebInput::BrakeAxis() const noexcept
    {
        return std::max(m_touchBrake, m_gamepadBrake);
    }

    bool WebInput::WasGamepadPressed(int button) const noexcept
    {
        return button >= 0
            && static_cast<std::size_t>(button) < m_gamepadPressed.size()
            && m_gamepadPressed[static_cast<std::size_t>(button)];
    }

    float WebInput::ControlValue(const std::string_view control) const
    {
        const auto keyboardCode = [](const std::string_view value)
        {
            if (value.size() == 9 && value.starts_with("Keyboard"))
            {
                const char symbol = value.back();
                if (symbol >= 'A' && symbol <= 'Z')
                {
                    return std::string("Key") + symbol;
                }
                if (symbol >= '0' && symbol <= '9')
                {
                    return std::string("Digit") + symbol;
                }
            }
            static const std::pair<std::string_view, std::string_view> keys[] = {
                { "KeyboardLeft", "ArrowLeft" },
                { "KeyboardRight", "ArrowRight" },
                { "KeyboardUp", "ArrowUp" },
                { "KeyboardDown", "ArrowDown" },
                { "KeyboardSpace", "Space" },
                { "KeyboardEnter", "Enter" },
                { "KeyboardEscape", "Escape" },
                { "KeyboardTab", "Tab" },
                { "KeyboardLeftShift", "ShiftLeft" },
                { "KeyboardRightShift", "ShiftRight" },
                { "KeyboardLeftControl", "ControlLeft" },
                { "KeyboardRightControl", "ControlRight" },
            };
            for (const auto& [name, code] : keys)
            {
                if (value == name)
                {
                    return std::string(code);
                }
            }
            return std::string{};
        };
        const std::string key = keyboardCode(control);
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
        static const std::pair<std::string_view, std::size_t> buttons[] = {
            { "GamePadA", 0 }, { "GamePadB", 1 },
            { "GamePadX", 2 }, { "GamePadY", 3 },
            { "GamePadLeftShoulder", 4 }, { "GamePadRightShoulder", 5 },
            { "GamePadBack", 8 }, { "GamePadStart", 9 },
            { "GamePadLeftStick", 10 }, { "GamePadRightStick", 11 },
            { "GamePadDPadUp", 12 }, { "GamePadDPadDown", 13 },
            { "GamePadDPadLeft", 14 }, { "GamePadDPadRight", 15 },
        };
        for (const auto& [name, button] : buttons)
        {
            if (control == name)
            {
                return m_gamepadDown[button] ? 1.0f : 0.0f;
            }
        }
        return 0.0f;
    }

    bool WebInput::ControlWasPressed(const std::string_view control) const
    {
        if (control.starts_with("Keyboard"))
        {
            if (control.size() == 9)
            {
                const char symbol = control.back();
                const std::string code = (symbol >= '0' && symbol <= '9'
                    ? std::string("Digit") : std::string("Key")) + symbol;
                return WasPressed(code.c_str());
            }
            static const std::pair<std::string_view, const char*> keys[] = {
                { "KeyboardLeft", "ArrowLeft" },
                { "KeyboardRight", "ArrowRight" },
                { "KeyboardUp", "ArrowUp" }, { "KeyboardDown", "ArrowDown" },
                { "KeyboardSpace", "Space" }, { "KeyboardEnter", "Enter" },
                { "KeyboardEscape", "Escape" }, { "KeyboardTab", "Tab" },
                { "KeyboardLeftShift", "ShiftLeft" },
                { "KeyboardRightShift", "ShiftRight" },
                { "KeyboardLeftControl", "ControlLeft" },
                { "KeyboardRightControl", "ControlRight" },
            };
            for (const auto& [name, code] : keys)
            {
                if (control == name) return WasPressed(code);
            }
        }
        if (control == "MouseLeft") return PointerButtonPressed(0);
        if (control == "MouseRight") return PointerButtonPressed(1);
        if (control == "MouseMiddle") return PointerButtonPressed(2);
        if (control == "GamePadLeftTrigger") return m_gamepadPressed[6];
        if (control == "GamePadRightTrigger") return m_gamepadPressed[7];
        static const std::string_view buttonNames[] = {
            "GamePadA", "GamePadB", "GamePadX", "GamePadY",
            "GamePadLeftShoulder", "GamePadRightShoulder", "", "",
            "GamePadBack", "GamePadStart", "GamePadLeftStick",
            "GamePadRightStick", "GamePadDPadUp", "GamePadDPadDown",
            "GamePadDPadLeft", "GamePadDPadRight",
        };
        for (std::size_t index{}; index < std::size(buttonNames); ++index)
        {
            if (control == buttonNames[index]) return m_gamepadPressed[index];
        }
        return false;
    }

    bool WebInput::ControlWasReleased(const std::string_view control) const
    {
        if (control.starts_with("Keyboard"))
        {
            if (control.size() == 9)
            {
                const char symbol = control.back();
                const std::string code = (symbol >= '0' && symbol <= '9'
                    ? std::string("Digit") : std::string("Key")) + symbol;
                return WasReleased(code.c_str());
            }
            static const std::pair<std::string_view, const char*> keys[] = {
                { "KeyboardLeft", "ArrowLeft" },
                { "KeyboardRight", "ArrowRight" },
                { "KeyboardUp", "ArrowUp" }, { "KeyboardDown", "ArrowDown" },
                { "KeyboardSpace", "Space" }, { "KeyboardEnter", "Enter" },
                { "KeyboardEscape", "Escape" }, { "KeyboardTab", "Tab" },
                { "KeyboardLeftShift", "ShiftLeft" },
                { "KeyboardRightShift", "ShiftRight" },
                { "KeyboardLeftControl", "ControlLeft" },
                { "KeyboardRightControl", "ControlRight" },
            };
            for (const auto& [name, code] : keys)
            {
                if (control == name) return WasReleased(code);
            }
        }
        if (control == "MouseLeft") return PointerButtonReleased(0);
        if (control == "MouseRight") return PointerButtonReleased(1);
        if (control == "MouseMiddle") return PointerButtonReleased(2);
        if (control == "GamePadLeftTrigger") return m_gamepadReleased[6];
        if (control == "GamePadRightTrigger") return m_gamepadReleased[7];
        static const std::string_view buttonNames[] = {
            "GamePadA", "GamePadB", "GamePadX", "GamePadY",
            "GamePadLeftShoulder", "GamePadRightShoulder", "", "",
            "GamePadBack", "GamePadStart", "GamePadLeftStick",
            "GamePadRightStick", "GamePadDPadUp", "GamePadDPadDown",
            "GamePadDPadLeft", "GamePadDPadRight",
        };
        for (std::size_t index{}; index < std::size(buttonNames); ++index)
        {
            if (control == buttonNames[index]) return m_gamepadReleased[index];
        }
        return false;
    }

    bool WebInput::HandleKeyEvent(
        int eventType,
        const char* code,
        void* userData) noexcept
    {
        auto* input = static_cast<WebInput*>(userData);
        if (input == nullptr || code == nullptr || *code == '\0')
        {
            return false;
        }
        if (eventType == EMSCRIPTEN_EVENT_KEYDOWN)
        {
            const auto [iterator, inserted] = input->m_down.emplace(code);
            (void)iterator;
            if (inserted)
            {
                input->m_pressed.emplace(code);
            }
            return true;
        }
        if (eventType == EMSCRIPTEN_EVENT_KEYUP)
        {
            input->m_down.erase(code);
            input->m_released.emplace(code);
            return true;
        }
        return false;
    }

    bool WebInput::HandleTouchEvent(
        int eventType,
        const EmscriptenTouchEvent* event,
        void* userData) noexcept
    {
        auto* input = static_cast<WebInput*>(userData);
        if (input == nullptr || event == nullptr)
        {
            return false;
        }
        input->m_touchHorizontal = 0.0f;
        input->m_touchVertical = 0.0f;
        input->m_touchAccelerate = 0.0f;
        input->m_touchBrake = 0.0f;
        if (eventType == EMSCRIPTEN_EVENT_TOUCHCANCEL)
        {
            if (input->m_pointerButtonsDown[0])
            {
                input->m_pointerButtonsReleased[0] = true;
            }
            input->m_pointerButtonsDown[0] = false;
            return true;
        }
        bool activeTouch{};
        double width = 1.0;
        double height = 1.0;
        emscripten_get_element_css_size(
            input->m_target.c_str(), &width, &height);
        width = std::max(width, 1.0);
        height = std::max(height, 1.0);
        for (int index = 0; index < event->numTouches; ++index)
        {
            const auto& touch = event->touches[index];
            if ((eventType == EMSCRIPTEN_EVENT_TOUCHEND) && touch.isChanged)
            {
                continue;
            }
            activeTouch = true;
            const float normalizedX = std::clamp(
                static_cast<float>(touch.targetX / width), 0.0f, 1.0f);
            const float normalizedY = std::clamp(
                static_cast<float>(touch.targetY / height), 0.0f, 1.0f);
            if (!input->m_pointerValid || index == 0)
            {
                input->m_pointerDeltaX += static_cast<float>(
                    touch.targetX) - input->m_pointerX;
                input->m_pointerDeltaY += static_cast<float>(
                    touch.targetY) - input->m_pointerY;
                input->m_pointerX = static_cast<float>(touch.targetX);
                input->m_pointerY = static_cast<float>(touch.targetY);
                input->m_pointerValid = true;
            }
            if (eventType == EMSCRIPTEN_EVENT_TOUCHSTART
                && touch.isChanged
                && normalizedX > 0.82f && normalizedY < 0.22f)
            {
                input->m_touchToggleViewPressed = true;
            }
            else if (normalizedX < 0.55f)
            {
                const float horizontal = std::clamp(
                    (normalizedX / 0.55f - 0.5f) * 2.0f, -1.0f, 1.0f);
                const float vertical = std::clamp(
                    (0.5f - normalizedY) * 2.0f, -1.0f, 1.0f);
                if (std::abs(horizontal) > 0.12f)
                {
                    input->m_touchHorizontal = horizontal;
                }
                if (std::abs(vertical) > 0.12f)
                {
                    input->m_touchVertical = vertical;
                }
            }
            else if (normalizedY < 0.58f)
            {
                input->m_touchAccelerate = 1.0f;
            }
            else
            {
                input->m_touchBrake = 1.0f;
            }
        }
        if (activeTouch && !input->m_pointerButtonsDown[0])
        {
            input->m_pointerButtonsPressed[0] = true;
        }
        if (!activeTouch && input->m_pointerButtonsDown[0])
        {
            input->m_pointerButtonsReleased[0] = true;
        }
        input->m_pointerButtonsDown[0] = activeTouch;
        return true;
    }

    bool WebInput::HandleMouseEvent(
        int eventType,
        const EmscriptenMouseEvent* event,
        void* userData) noexcept
    {
        auto* input = static_cast<WebInput*>(userData);
        if (input == nullptr || event == nullptr)
        {
            return false;
        }
        input->m_pointerX = static_cast<float>(event->targetX);
        input->m_pointerY = static_cast<float>(event->targetY);
        input->m_pointerDeltaX += static_cast<float>(event->movementX);
        input->m_pointerDeltaY += static_cast<float>(event->movementY);
        input->m_pointerValid = true;
        const auto mapButton = [](unsigned short button)
        {
            return button == 0 ? 0 : button == 2 ? 1
                : button == 1 ? 2 : button == 3 ? 3
                : button == 4 ? 4 : -1;
        };
        const int button = mapButton(event->button);
        if (button >= 0)
        {
            const std::size_t index = static_cast<std::size_t>(button);
            if (eventType == EMSCRIPTEN_EVENT_MOUSEDOWN)
            {
                if (!input->m_pointerButtonsDown[index])
                {
                    input->m_pointerButtonsPressed[index] = true;
                }
                input->m_pointerButtonsDown[index] = true;
            }
            else if (eventType == EMSCRIPTEN_EVENT_MOUSEUP)
            {
                input->m_pointerButtonsDown[index] = false;
                input->m_pointerButtonsReleased[index] = true;
            }
        }
        return true;
    }

    bool WebInput::HandleWheelEvent(
        const EmscriptenWheelEvent* event,
        void* userData) noexcept
    {
        auto* input = static_cast<WebInput*>(userData);
        if (input == nullptr || event == nullptr)
        {
            return false;
        }
        input->m_pointerWheel += static_cast<float>(-event->deltaY);
        return true;
    }

    bool WebInput::PointerButtonDown(int button) const noexcept
    {
        return button >= 0
            && static_cast<std::size_t>(button) < m_pointerButtonsDown.size()
            && m_pointerButtonsDown[static_cast<std::size_t>(button)];
    }

    bool WebInput::PointerButtonPressed(int button) const noexcept
    {
        return button >= 0
            && static_cast<std::size_t>(button) < m_pointerButtonsPressed.size()
            && m_pointerButtonsPressed[static_cast<std::size_t>(button)];
    }

    bool WebInput::PointerButtonReleased(int button) const noexcept
    {
        return button >= 0
            && static_cast<std::size_t>(button) < m_pointerButtonsReleased.size()
            && m_pointerButtonsReleased[static_cast<std::size_t>(button)];
    }

    void WebInput::SampleGamepads() noexcept
    {
        m_gamepadPressed.fill(false);
        m_gamepadReleased.fill(false);
        m_gamepadHorizontal = 0.0f;
        m_gamepadVertical = 0.0f;
        m_gamepadRightHorizontal = 0.0f;
        m_gamepadRightVertical = 0.0f;
        m_gamepadAccelerate = 0.0f;
        m_gamepadBrake = 0.0f;
        if (emscripten_sample_gamepad_data() != EMSCRIPTEN_RESULT_SUCCESS)
        {
            return;
        }
        const int count = emscripten_get_num_gamepads();
        EmscriptenGamepadEvent state{};
        bool sampled{};
        for (int gamepad = 0; gamepad < count; ++gamepad)
        {
            if (emscripten_get_gamepad_status(gamepad, &state)
                    != EMSCRIPTEN_RESULT_SUCCESS
                || !state.connected)
            {
                continue;
            }
            if (state.numAxes > 0)
            {
                m_gamepadHorizontal = std::clamp(
                    static_cast<float>(state.axis[0]), -1.0f, 1.0f);
                if (std::abs(m_gamepadHorizontal) < 0.12f)
                {
                    m_gamepadHorizontal = 0.0f;
                }
            }
            if (state.numAxes > 1)
            {
                m_gamepadVertical = std::clamp(
                    -static_cast<float>(state.axis[1]), -1.0f, 1.0f);
                if (std::abs(m_gamepadVertical) < 0.12f)
                {
                    m_gamepadVertical = 0.0f;
                }
            }
            if (state.numAxes > 2)
            {
                m_gamepadRightHorizontal = std::clamp(
                    static_cast<float>(state.axis[2]), -1.0f, 1.0f);
                if (std::abs(m_gamepadRightHorizontal) < 0.12f)
                {
                    m_gamepadRightHorizontal = 0.0f;
                }
            }
            if (state.numAxes > 3)
            {
                m_gamepadRightVertical = std::clamp(
                    -static_cast<float>(state.axis[3]), -1.0f, 1.0f);
                if (std::abs(m_gamepadRightVertical) < 0.12f)
                {
                    m_gamepadRightVertical = 0.0f;
                }
            }
            if (state.numButtons > 7)
            {
                m_gamepadBrake = std::clamp(
                    static_cast<float>(state.analogButton[6]), 0.0f, 1.0f);
                m_gamepadAccelerate = std::clamp(
                    static_cast<float>(state.analogButton[7]), 0.0f, 1.0f);
            }
            for (std::size_t button{};
                 button < m_gamepadDown.size()
                    && button < static_cast<std::size_t>(state.numButtons);
                 ++button)
            {
                const bool down = state.digitalButton[button] != 0;
                m_gamepadPressed[button] = down && !m_gamepadDown[button];
                m_gamepadReleased[button] = !down && m_gamepadDown[button];
                m_gamepadDown[button] = down;
            }
            sampled = true;
            break;
        }
        if (!sampled)
        {
            m_gamepadDown.fill(false);
        }
    }
}
