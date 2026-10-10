#pragma once
#include <array>
#include <string>
#include <string_view>
#include <unordered_set>

namespace LamaPon::Portable
{
    // OSイベントの登録と分離した、フレーム単位の共通入力状態。
    class InputState
    {
    public:
        // 消費済みのキー・ポインター・タッチの変化状態を消す。
        void EndFrame() noexcept;
        // フォーカス喪失時に保持入力と未消費の変化を解除する。
        void Reset() noexcept;
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
        // 左スティックの方向への初回入力。direction: 上0、下1、左2、右3。
        [[nodiscard]] bool WasGamepadNavigationPressed(int direction) const noexcept
        {
            return direction >= 0 && direction < 4 && m_navigationPressed[static_cast<std::size_t>(direction)];
        }
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

        // OSアダプターからキーボードの保持状態を受け取る(code: 共通キー名, down: 押下状態)。
        void KeyEvent(std::string_view code, bool down);
        // 論理画面上のポインター位置を受け取る(x/y: 座標)。
        void PointerEvent(float x, float y) noexcept;
        // ポインターボタンの変化を受け取る(button: ボタン番号, down: 押下状態)。
        void ButtonEvent(int button, bool down) noexcept;
        // フレームのホイール移動を累積する(amount: 上方向が正)。
        void WheelEvent(float amount) noexcept;
        // 接続中または切断後のゲームパッド状態を適用する(buttons: Web標準順の32ボタン, axes: 左右XY・左右trigger)。
        void GamepadSnapshot(const std::array<bool, 32>& buttons, const std::array<float, 6>& axes) noexcept;

    protected:
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
        std::array<bool, 4> m_navigationDown{}, m_navigationPressed{};
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
    };
}
