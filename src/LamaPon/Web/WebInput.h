#pragma once
#if defined(LAMAPON_NATIVE_RUNTIME)
#include "LamaPon/Native/NativeInput.h"
namespace LamaPon::Web { using WebInput = Native::NativeInput; }
#else

#include <array>
#include <string>
#include <string_view>
#include <unordered_set>

#include "LamaPon/Portable/PortableInputState.h"
#include <emscripten/html5.h>

namespace LamaPon::Web
{
    // ブラウザーのイベント駆動入力を、Windows版と同じフレーム単位のキー状態として公開します。
    class WebInput final : public Portable::InputState
    {
    public:
        // 未登録のWeb入力を用意する。
        WebInput() = default;
        // 所有するブラウザーイベント登録を解除する。
        ~WebInput();

        // 複製を禁止する。
        WebInput(const WebInput&) = delete;
        // 複製代入を禁止する。
        WebInput& operator=(const WebInput&) = delete;

        // windowを占有するため同時に初期化できるのは1つで、Canvas要素は破棄まで保持する。
        // 再呼出時に初期化済みなら登録先を変更せずtrueを返す。
        // Canvasとwindowのイベントを登録し、失敗時は成功分を解除する(target: 借用するCanvasのCSS指定)。
        [[nodiscard]] bool Initialize(const char* target = "#canvas");
        // Gamepadを取得し、フレーム用の変化状態を更新する。
        void BeginFrame() noexcept;
    private:
        // 成功したイベント登録をこの所有者とコールバックの組で解除する。
        void Shutdown() noexcept;
        // キー状態を更新し、処理済みイベントを占有する(eventType: イベント種別, code: DOMキーコード, userData: 入力の所有者)。
        static bool HandleKeyEvent(
            int eventType,
            const char* code,
            void* userData) noexcept;
        // タッチを移動・ペダル・ポインターへ変換する(eventType: イベント種別, event: タッチ状態, userData: 入力の所有者)。
        static bool HandleTouchEvent(
            int eventType,
            const EmscriptenTouchEvent* event,
            void* userData) noexcept;
        // マウス位置とボタンの状態を更新する(eventType: イベント種別, event: マウス状態, userData: 入力の所有者)。
        static bool HandleMouseEvent(
            int eventType,
            const EmscriptenMouseEvent* event,
            void* userData) noexcept;
        // wheel deltaYを反転して累積する(event: ホイール状態, userData: 入力の所有者)。
        static bool HandleWheelEvent(
            const EmscriptenWheelEvent* event,
            void* userData) noexcept;
        // 取得失敗時は保持状態を残し、切断時は保持ボタンの解放を通知する。
        // 最初に取得できた接続中Gamepadの軸とボタンを更新する。
        void SampleGamepads() noexcept;

        // イベントごとの登録成功状態
        std::array<bool, 12> m_registered{};
        // 解除時に照合する登録関数
        std::array<void*, 12> m_callbacks{};
        // 登録先CanvasのCSS指定
        std::string m_target;
        // 全イベントの登録成功状態
        bool m_initialized{};
    };
}

#endif
