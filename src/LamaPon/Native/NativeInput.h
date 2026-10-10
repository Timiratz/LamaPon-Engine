#pragma once

#include "LamaPon/Portable/PortableInputState.h"
#include <SDL3/SDL.h>
#include <map>

namespace LamaPon::Native
{
    // SDLのイベントを既存ゲームScriptの入力名へ変換する。
    class NativeInput final : public Portable::InputState
    {
    public:
        NativeInput() = default;
        ~NativeInput();
        NativeInput(const NativeInput&) = delete;
        NativeInput& operator=(const NativeInput&) = delete;
        bool Initialize(SDL_Window* window);
        // 0は最初の接続機器を自動選択。指定機器の切断後は別機器へ勝手に切り替えない。
        bool SelectGamepad(SDL_JoystickID id);
        void BeginFrame() noexcept;
        void ProcessEvent(const SDL_Event& event);
        void Reset() noexcept;
    private:
        void SampleGamepads() noexcept;
        void RefreshTouchAxes() noexcept;
        void MousePosition(float x, float y) noexcept;
        SDL_Window* m_window{};
        SDL_Gamepad* m_gamepad{};
        SDL_JoystickID m_preferredGamepad{};
        // Finger IDs are unique within a touch device, not across devices.
        using TouchKey = std::pair<SDL_TouchID, SDL_FingerID>;
        std::map<TouchKey, std::pair<float, float>> m_touches;
        TouchKey m_pointerFinger{};
        bool m_hasPointerFinger{};
    };
}
