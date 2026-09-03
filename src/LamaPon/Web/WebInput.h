#pragma once

#include <array>
#include <string>
#include <string_view>
#include <unordered_set>

#include <emscripten/html5.h>

namespace LamaPon::Web
{
    // Browser InputはEvent駆動ですが、Game向けAPIではWindows Runtimeと
    // 同じFrame内で安定したKey State Queryとして公開します。
    class WebInput final
    {
    public:
        WebInput() = default;
        ~WebInput();

        WebInput(const WebInput&) = delete;
        WebInput& operator=(const WebInput&) = delete;

        [[nodiscard]] bool Initialize(const char* target = "#canvas");
        void BeginFrame() noexcept;
        void EndFrame() noexcept;
        [[nodiscard]] bool IsDown(const char* code) const;
        [[nodiscard]] bool WasPressed(const char* code) const;
        [[nodiscard]] bool WasReleased(const char* code) const;
        [[nodiscard]] float HorizontalAxis() const noexcept;
        [[nodiscard]] float VerticalAxis() const noexcept;
        [[nodiscard]] float AccelerateAxis() const noexcept;
        [[nodiscard]] float BrakeAxis() const noexcept;
        [[nodiscard]] float TouchHorizontalAxis() const noexcept
        {
            return m_touchHorizontal;
        }
        [[nodiscard]] float TouchVerticalAxis() const noexcept
        {
            return m_touchVertical;
        }
        [[nodiscard]] float TouchAccelerateAxis() const noexcept
        {
            return m_touchAccelerate;
        }
        [[nodiscard]] float TouchBrakeAxis() const noexcept
        {
            return m_touchBrake;
        }
        [[nodiscard]] bool WasGamepadPressed(int button) const noexcept;
        [[nodiscard]] bool WasTouchToggleViewPressed() const noexcept
        {
            return m_touchToggleViewPressed;
        }
        [[nodiscard]] float ControlValue(std::string_view control) const;
        [[nodiscard]] bool ControlWasPressed(std::string_view control) const;
        [[nodiscard]] bool ControlWasReleased(std::string_view control) const;
        [[nodiscard]] float PointerX() const noexcept { return m_pointerX; }
        [[nodiscard]] float PointerY() const noexcept { return m_pointerY; }
        [[nodiscard]] float PointerDeltaX() const noexcept
        {
            return m_pointerDeltaX;
        }
        [[nodiscard]] float PointerDeltaY() const noexcept
        {
            return m_pointerDeltaY;
        }
        [[nodiscard]] float PointerWheel() const noexcept
        {
            return m_pointerWheel;
        }
        [[nodiscard]] bool PointerValid() const noexcept
        {
            return m_pointerValid;
        }
        [[nodiscard]] bool PointerButtonDown(int button) const noexcept;
        [[nodiscard]] bool PointerButtonPressed(int button) const noexcept;
        [[nodiscard]] bool PointerButtonReleased(int button) const noexcept;

    private:
        struct EventState;
        static bool HandleKeyEvent(
            int eventType,
            const char* code,
            void* userData) noexcept;
        static bool HandleTouchEvent(
            int eventType,
            const EmscriptenTouchEvent* event,
            void* userData) noexcept;
        static bool HandleMouseEvent(
            int eventType,
            const EmscriptenMouseEvent* event,
            void* userData) noexcept;
        static bool HandleWheelEvent(
            const EmscriptenWheelEvent* event,
            void* userData) noexcept;
        void SampleGamepads() noexcept;

        EventState* m_state{};
        std::unordered_set<std::string> m_down;
        std::unordered_set<std::string> m_pressed;
        std::unordered_set<std::string> m_released;
        std::array<bool, 32> m_gamepadDown{};
        std::array<bool, 32> m_gamepadPressed{};
        std::array<bool, 32> m_gamepadReleased{};
        float m_gamepadHorizontal{};
        float m_gamepadVertical{};
        float m_gamepadRightHorizontal{};
        float m_gamepadRightVertical{};
        float m_gamepadAccelerate{};
        float m_gamepadBrake{};
        float m_touchHorizontal{};
        float m_touchVertical{};
        float m_touchAccelerate{};
        float m_touchBrake{};
        bool m_touchToggleViewPressed{};
        std::array<bool, 5> m_pointerButtonsDown{};
        std::array<bool, 5> m_pointerButtonsPressed{};
        std::array<bool, 5> m_pointerButtonsReleased{};
        float m_pointerX{};
        float m_pointerY{};
        float m_pointerDeltaX{};
        float m_pointerDeltaY{};
        float m_pointerWheel{};
        bool m_pointerValid{};
        std::string m_target;
        bool m_initialized{};
    };
}
