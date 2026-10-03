#pragma once

#include <array>
#include <string>
#include <string_view>
#include <unordered_set>

#include <emscripten/html5.h>

namespace LamaPon::Web
{
    // ブラウザーのイベント駆動入力を、Windows版と同じフレーム単位のキー状態として公開します。
    class WebInput final
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
        // 消費済みのキー・ポインター・タッチの変化状態を消す。
        void EndFrame() noexcept;
        // キーの保持状態を返す(code: DOMのキーコード)。
        [[nodiscard]] bool IsDown(const char* code) const;
        // 今フレームの初回キー押下を返す(code: DOMのキーコード)。
        [[nodiscard]] bool WasPressed(const char* code) const;
        // 今フレームのキー解放イベントを返す(code: DOMのキーコード)。
        [[nodiscard]] bool WasReleased(const char* code) const;
        // 絶対値が大きいタッチ・左スティックの横軸を返す。
        [[nodiscard]] float HorizontalAxis() const noexcept;
        // 絶対値が大きいタッチ・左スティックの縦軸を返す。
        [[nodiscard]] float VerticalAxis() const noexcept;
        // タッチと右トリガーの大きいアクセル値を返す。
        [[nodiscard]] float AccelerateAxis() const noexcept;
        // タッチと左トリガーの大きいブレーキ値を返す。
        [[nodiscard]] float BrakeAxis() const noexcept;
        // タッチ領域の横軸を返す。
        [[nodiscard]] float TouchHorizontalAxis() const noexcept
        {
            return m_touchHorizontal;
        }
        // タッチ領域の縦軸を返す。
        [[nodiscard]] float TouchVerticalAxis() const noexcept
        {
            return m_touchVertical;
        }
        // タッチ領域のアクセル値を返す。
        [[nodiscard]] float TouchAccelerateAxis() const noexcept
        {
            return m_touchAccelerate;
        }
        // タッチ領域のブレーキ値を返す。
        [[nodiscard]] float TouchBrakeAxis() const noexcept
        {
            return m_touchBrake;
        }
        // 今フレームのGamepad押下を返す(button: ボタン番号0～31)。
        [[nodiscard]] bool WasGamepadPressed(int button) const noexcept;
        // 今フレームのタッチによる表示切替要求を返す。
        [[nodiscard]] bool WasTouchToggleViewPressed() const noexcept
        {
            return m_touchToggleViewPressed;
        }
        // 名前付き入力の値を返し、未知の名前は零とする(control: 入力名)。
        [[nodiscard]] float ControlValue(std::string_view control) const;
        // 名前付きボタンの押下を返し、未知の名前はfalseとする(control: 入力名)。
        [[nodiscard]] bool ControlWasPressed(std::string_view control) const;
        // 名前付きボタンの解放を返し、未知の名前はfalseとする(control: 入力名)。
        [[nodiscard]] bool ControlWasReleased(std::string_view control) const;
        // Canvas内のポインターX座標をCSS単位で返す。
        [[nodiscard]] float PointerX() const noexcept { return m_pointerX; }
        // Canvas内のポインターY座標をCSS単位で返す。
        [[nodiscard]] float PointerY() const noexcept { return m_pointerY; }
        // フレーム中のポインターX移動量を返す。
        [[nodiscard]] float PointerDeltaX() const noexcept
        {
            return m_pointerDeltaX;
        }
        // フレーム中のポインターY移動量を返す。
        [[nodiscard]] float PointerDeltaY() const noexcept
        {
            return m_pointerDeltaY;
        }
        // フレーム中の反転したwheel deltaY累積値を返す。
        [[nodiscard]] float PointerWheel() const noexcept
        {
            return m_pointerWheel;
        }
        // ポインター位置を一度でも取得したか返す。
        [[nodiscard]] bool PointerValid() const noexcept
        {
            return m_pointerValid;
        }
        // ポインターボタンの保持状態を返す(button: 左／右／中／戻る／進むの番号)。
        [[nodiscard]] bool PointerButtonDown(int button) const noexcept;
        // ポインターボタンの押下を返す(button: 左／右／中／戻る／進むの番号)。
        [[nodiscard]] bool PointerButtonPressed(int button) const noexcept;
        // ポインターボタンの解放を返す(button: 左／右／中／戻る／進むの番号)。
        [[nodiscard]] bool PointerButtonReleased(int button) const noexcept;

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
        // 取得失敗時は保持状態を残し、切断時も解放イベントは生成しない。
        // 最初に取得できた接続中Gamepadの軸とボタンを更新する。
        void SampleGamepads() noexcept;

        // イベントごとの登録成功状態
        std::array<bool, 10> m_registered{};
        // 解除時に照合する登録関数
        std::array<void*, 10> m_callbacks{};
        // 保持中のDOMキーコード
        std::unordered_set<std::string> m_down;
        // 今フレームの初回キー押下
        std::unordered_set<std::string> m_pressed;
        // 今フレームのキー解放イベント
        std::unordered_set<std::string> m_released;
        // Gamepadの保持ボタン
        std::array<bool, 32> m_gamepadDown{};
        // 今フレームのGamepad押下
        std::array<bool, 32> m_gamepadPressed{};
        // 今フレームのGamepad解放
        std::array<bool, 32> m_gamepadReleased{};
        // 左スティックの横軸
        float m_gamepadHorizontal{};
        // 上を正とする左スティック縦軸
        float m_gamepadVertical{};
        // 右スティックの横軸
        float m_gamepadRightHorizontal{};
        // 上を正とする右スティック縦軸
        float m_gamepadRightVertical{};
        // 右トリガーのアクセル値
        float m_gamepadAccelerate{};
        // 左トリガーのブレーキ値
        float m_gamepadBrake{};
        // タッチ移動領域の横軸
        float m_touchHorizontal{};
        // タッチ移動領域の縦軸
        float m_touchVertical{};
        // タッチのアクセル値
        float m_touchAccelerate{};
        // タッチのブレーキ値
        float m_touchBrake{};
        // 今フレームのタッチ表示切替
        bool m_touchToggleViewPressed{};
        // ポインターボタンの保持状態
        std::array<bool, 5> m_pointerButtonsDown{};
        // 今フレームのポインター押下
        std::array<bool, 5> m_pointerButtonsPressed{};
        // 今フレームのポインター解放
        std::array<bool, 5> m_pointerButtonsReleased{};
        // Canvas内のX座標（CSS単位）
        float m_pointerX{};
        // Canvas内のY座標（CSS単位）
        float m_pointerY{};
        // フレーム中のX移動量
        float m_pointerDeltaX{};
        // フレーム中のY移動量
        float m_pointerDeltaY{};
        // 反転したwheel deltaYの累積
        float m_pointerWheel{};
        // ポインター位置の取得済み状態
        bool m_pointerValid{};
        // 登録先CanvasのCSS指定
        std::string m_target;
        // 全イベントの登録成功状態
        bool m_initialized{};
    };
}
