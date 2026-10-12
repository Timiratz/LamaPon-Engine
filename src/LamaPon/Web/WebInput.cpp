#include "LamaPon/Web/WebInput.h"

#include <emscripten/html5.h>
#include <emscripten.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace LamaPon::Web
{
    namespace
    {
        // windowイベントを占有する入力の所有者
        WebInput* activeInput{};

        EM_JS(bool, BrowserOwnsKeyboard, (), {
            const element = document.activeElement;
            return Boolean(element && (element.isContentEditable
                || ['INPUT', 'TEXTAREA', 'SELECT', 'BUTTON'].includes(element.tagName)));
        });
        EM_JS(bool, UsesSharedGameStage, (const char* target), {
            const canvas = document.querySelector(UTF8ToString(target));
            const stage = canvas && canvas.parentElement;
            return Boolean(stage && stage.id === 'stage' && stage.querySelector('#software-canvas'));
        });

        // CSSの拡縮とwindow側で受けた解放イベントをCanvas座標へ戻す。
        EM_JS(double, CanvasCoordinate, (const char* target, double client, int axis), {
            const canvas = document.querySelector(UTF8ToString(target));
            if (!canvas) return 0;
            const rect = canvas.getBoundingClientRect();
            const size = axis ? rect.height : rect.width;
            const logical = axis ? canvas.clientHeight : canvas.clientWidth;
            return (client - (axis ? rect.top : rect.left)) * logical / Math.max(size, 1);
        });
    }

    WebInput::~WebInput()
    {
        Shutdown();
    }

    void WebInput::Shutdown() noexcept
    {
        // 登録順に対応するイベント種別
        constexpr std::array eventTypes{
            EMSCRIPTEN_EVENT_KEYDOWN, EMSCRIPTEN_EVENT_KEYUP,
            EMSCRIPTEN_EVENT_TOUCHSTART, EMSCRIPTEN_EVENT_TOUCHMOVE,
            EMSCRIPTEN_EVENT_TOUCHEND, EMSCRIPTEN_EVENT_TOUCHCANCEL,
            EMSCRIPTEN_EVENT_MOUSEMOVE, EMSCRIPTEN_EVENT_MOUSEDOWN,
            EMSCRIPTEN_EVENT_MOUSEUP, EMSCRIPTEN_EVENT_WHEEL,
            EMSCRIPTEN_EVENT_BLUR, EMSCRIPTEN_EVENT_VISIBILITYCHANGE };

        // イベント・入力配列の要素番号
        for (std::size_t index = 0; index < m_registered.size(); ++index)
        {
            if (m_registered[index])
            {
                emscripten_html5_remove_event_listener(
                    index == 11 ? EMSCRIPTEN_EVENT_TARGET_DOCUMENT
                        : (index < 2 || index == 8 || index == 10)
                            ? EMSCRIPTEN_EVENT_TARGET_WINDOW : m_target.c_str(),
                    // 登録順に対応するイベント種別
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
        // The default shell switches between sibling WebGL, Canvas2D and SVG surfaces.
        // Their common stage retains its geometry and listeners during that switch.
        if (UsesSharedGameStage(target)) m_target = "#stage";
        target = m_target.c_str();
        activeInput = this;
        // キーイベントを転送する(type: イベント種別, event: キー状態, owner: 入力の所有者)。
        const auto key = +[](int type, const EmscriptenKeyboardEvent* event,
                            void* owner) -> EM_BOOL
        {
            if (BrowserOwnsKeyboard())
            {
                static_cast<WebInput*>(owner)->Reset();
                return EM_FALSE;
            }
            return HandleKeyEvent(type, event ? event->code : "", owner);
        };
        // タッチイベントを転送する(type: イベント種別, event: タッチ状態, owner: 入力の所有者)。
        // 現在のタッチ点の状態
        const auto touch = +[](int type, const EmscriptenTouchEvent* event,
                              void* owner) -> EM_BOOL
        {
            return HandleTouchEvent(type, event, owner);
        };
        // マウスイベントを転送する(type: イベント種別, event: マウス状態, owner: 入力の所有者)。
        const auto mouse = +[](int type, const EmscriptenMouseEvent* event,
                              void* owner) -> EM_BOOL
        {
            return HandleMouseEvent(type, event, owner);
        };
        // ホイールイベントを転送する(event: ホイール状態, owner: 入力の所有者)。
        const auto wheel = +[](int, const EmscriptenWheelEvent* event,
                              void* owner) -> EM_BOOL
        {
            return HandleWheelEvent(event, owner);
        };
        const auto blur = +[](int, const EmscriptenFocusEvent*, void* owner) -> EM_BOOL
        {
            static_cast<WebInput*>(owner)->Reset();
            return EM_FALSE;
        };
        const auto visibility = +[](int, const EmscriptenVisibilityChangeEvent* event,
                                    void* owner) -> EM_BOOL
        {
            if (event && event->hidden) static_cast<WebInput*>(owner)->Reset();
            return EM_FALSE;
        };

        m_callbacks = {
            reinterpret_cast<void*>(key), reinterpret_cast<void*>(key),
            reinterpret_cast<void*>(touch), reinterpret_cast<void*>(touch),
            reinterpret_cast<void*>(touch), reinterpret_cast<void*>(touch),
            reinterpret_cast<void*>(mouse), reinterpret_cast<void*>(mouse),
            reinterpret_cast<void*>(mouse), reinterpret_cast<void*>(wheel),
            reinterpret_cast<void*>(blur), reinterpret_cast<void*>(visibility) };

        // キーイベントのwindow登録先
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
            emscripten_set_mouseup_callback(window, this, EM_TRUE, mouse) == EMSCRIPTEN_RESULT_SUCCESS,
            emscripten_set_wheel_callback(target, this, EM_TRUE, wheel) == EMSCRIPTEN_RESULT_SUCCESS,
            emscripten_set_blur_callback(window, this, EM_TRUE, blur) == EMSCRIPTEN_RESULT_SUCCESS,
            emscripten_set_visibilitychange_callback(this, EM_TRUE, visibility) == EMSCRIPTEN_RESULT_SUCCESS };
        // 全登録の成功を確認する(registered: イベントの登録成功状態)。
        m_initialized = std::all_of(
            m_registered.begin(), m_registered.end(),
            [](bool registered) { return registered; });
        if (!m_initialized)
        {

            Shutdown();
        }
        return m_initialized;
    }

    void WebInput::BeginFrame() noexcept
    {
        // 押下・解放はフレーム処理後のEndFrameで消すため、ここでは消去しない。
        SampleGamepads();
    }

    bool WebInput::HandleKeyEvent(
        int eventType,
        const char* code,
        void* userData) noexcept
    {
        // イベントを受ける入力の所有者
        auto* input = static_cast<WebInput*>(userData);
        if (input == nullptr || code == nullptr || *code == '\0')
        {
            return false;
        }
        if (eventType == EMSCRIPTEN_EVENT_KEYDOWN)
        {
            // iterator: 登録位置、inserted: 初回の押下か
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
        // イベントを受ける入力の所有者
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
            // 取消をクリック用の解放に変換しない。
            input->m_pointerButtonsPressed[0] = false;
            input->m_pointerButtonsReleased[0] = false;
            input->m_pointerButtonsDown[0] = false;
            input->m_pointerValid = false;
            input->m_touchToggleViewPressed = false;
            return true;
        }
        // 終了・取消以外のタッチがあるか
        bool activeTouch{};
        // CanvasのCSS幅
        double width = 1.0;
        // CanvasのCSS高さ
        double height = 1.0;
        emscripten_get_element_css_size(
            input->m_target.c_str(), &width, &height);
        width = std::max(width, 1.0);
        height = std::max(height, 1.0);
        // イベント・入力配列の要素番号
        for (int index = 0; index < event->numTouches; ++index)
        {
            // 現在のタッチ点の状態
            const auto& touch = event->touches[index];
            if ((eventType == EMSCRIPTEN_EVENT_TOUCHEND) && touch.isChanged)
            {
                if (index == 0)
                {
                    input->m_pointerX = static_cast<float>(CanvasCoordinate(
                        input->m_target.c_str(), touch.clientX, 0));
                    input->m_pointerY = static_cast<float>(CanvasCoordinate(
                        input->m_target.c_str(), touch.clientY, 1));
                }
                continue;
            }
            activeTouch = true;
            // Canvas幅で正規化したX座標
            const float normalizedX = std::clamp(
                static_cast<float>(touch.targetX / width), 0.0f, 1.0f);
            // Canvas高さで正規化したY座標
            const float normalizedY = std::clamp(
                static_cast<float>(touch.targetY / height), 0.0f, 1.0f);
            if (!input->m_pointerValid || index == 0)
            {
                const float x = static_cast<float>(CanvasCoordinate(
                    input->m_target.c_str(), touch.clientX, 0));
                const float y = static_cast<float>(CanvasCoordinate(
                    input->m_target.c_str(), touch.clientY, 1));
                if (input->m_pointerValid)
                {
                    input->m_pointerDeltaX += x - input->m_pointerX;
                    input->m_pointerDeltaY += y - input->m_pointerY;
                }
                input->m_pointerX = x;
                input->m_pointerY = y;
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
                // 移動領域の横入力-1～1
                const float horizontal = std::clamp(
                    (normalizedX / 0.55f - 0.5f) * 2.0f, -1.0f, 1.0f);
                // 移動領域の縦入力-1～1
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
        // イベントを受ける入力の所有者
        auto* input = static_cast<WebInput*>(userData);
        if (input == nullptr || event == nullptr)
        {
            return false;
        }
        const float x = static_cast<float>(CanvasCoordinate(
            input->m_target.c_str(), event->clientX, 0));
        const float y = static_cast<float>(CanvasCoordinate(
            input->m_target.c_str(), event->clientY, 1));
        if (input->m_pointerValid)
        {
            input->m_pointerDeltaX += x - input->m_pointerX;
            input->m_pointerDeltaY += y - input->m_pointerY;
        }
        input->m_pointerX = x;
        input->m_pointerY = y;
        input->m_pointerValid = true;
        // DOM番号を左・右・中・戻る・進むの順へ変換する(button: DOMボタン番号)。
        const auto mapButton = [](unsigned short button)
        {
            return button == 0 ? 0 : button == 2 ? 1
                : button == 1 ? 2 : button == 3 ? 3
                : button == 4 ? 4 : -1;
        };
        // 変換・走査するボタン番号
        const int button = mapButton(event->button);
        if (button >= 0)
        {
            // イベント・入力配列の要素番号
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
        // イベントを受ける入力の所有者
        auto* input = static_cast<WebInput*>(userData);
        if (input == nullptr || event == nullptr)
        {
            return false;
        }
        input->m_pointerWheel += static_cast<float>(-event->deltaY);
        return true;
    }

    void WebInput::SampleGamepads() noexcept
    {
        std::array<bool, 32> buttons{};
        std::array<float, 6> axes{};
        if (emscripten_sample_gamepad_data() == EMSCRIPTEN_RESULT_SUCCESS)
        {
            const int count = emscripten_get_num_gamepads();
            EmscriptenGamepadEvent state{};
            for (int gamepad = 0; gamepad < count; ++gamepad)
            {
                if (emscripten_get_gamepad_status(gamepad, &state) != EMSCRIPTEN_RESULT_SUCCESS
                    || !state.connected) continue;
                for (int index = 0; index < 4 && index < state.numAxes; ++index)
                {
                    float value = static_cast<float>(state.axis[index]);
                    if (index == 1 || index == 3) value = -value;
                    axes[static_cast<std::size_t>(index)] = std::abs(value) < 0.12f ? 0.0f : value;
                }
                if (state.numButtons > 7)
                {
                    axes[4] = static_cast<float>(state.analogButton[6]);
                    axes[5] = static_cast<float>(state.analogButton[7]);
                }
                for (std::size_t button = 0; button < buttons.size()
                    && button < static_cast<std::size_t>(std::max(state.numButtons, 0)); ++button)
                    buttons[button] = state.digitalButton[button] != 0;
                break;
            }
        }
        // SDLと同じ方向エッジ・有限値検査・切断時の解除を使う。
        GamepadSnapshot(buttons, axes);
    }
}
