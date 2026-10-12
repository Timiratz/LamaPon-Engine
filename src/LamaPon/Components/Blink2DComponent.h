#pragma once

#include "LamaPon/Scene/Component.h"

#include <cstdint>
#include <random>

namespace LamaPon
{
    // 目のスプライトシートと瞬きの間隔を表します。
    // コマ番号はSprite Animatorと同じく左上から右へ、次の行へと数えます。
    struct Blink2DSettings final
    {
        // シートの列数(1〜256)
        int columns{ 3 };
        // シートの行数(1〜256)
        int rows{ 1 };
        // 目を開いたコマ番号
        int openFrame{ 0 };
        // 閉じ始めのコマ番号で、ここから閉じきるまで連続して並べます
        int closingStartFrame{ 1 };
        // 閉じ始めから閉じきるまでのコマ数(1〜64、最後が閉じた目)
        int closingFrameCount{ 2 };
        // 半目などの途中のコマを表示する秒数
        float frameSeconds{ 0.04f };
        // 閉じた目を表示する秒数
        float closedSeconds{ 0.06f };
        // 次の瞬きまでの最短秒数
        float intervalMinSeconds{ 2.0f };
        // 次の瞬きまでの最長秒数
        float intervalMaxSeconds{ 6.0f };
        // 瞬きの直後にもう一度瞬く確率(0〜1)
        float doubleBlinkChance{ 0.15f };
        // ランダムな間隔で自動的に瞬くか
        bool autoBlink{ true };
        // 子孫のSprite Rendererも同じコマにするか
        bool includeChildren{ true };
    };

    // 自身と子孫のSprite Rendererを「開→半目→閉→半目→開」のコマで切り替えて瞬きさせます。
    // 左右の目をまとめた親に付けると両目が同時に瞬き、別のBlink2Dを持つ子孫はその部品に任せます。
    // 対象のSprite Rendererの画像領域を毎フレーム書き換えるため、Sprite Animatorと同じ対象には使いません。
    class Blink2DComponent final : public Component
    {
    public:
        // 瞬きを作ります(settings: 瞬き設定で範囲外の値は補正)。
        explicit Blink2DComponent(
            const Blink2DSettings& settings = {}) noexcept;

        // 範囲外の値を補正して置き換え、初期化済みなら現在のコマを反映します(settings: 新しい瞬き設定)。
        void SetSettings(const Blink2DSettings& settings);
        // 補正済みの瞬き設定を返します。
        [[nodiscard]] const Blink2DSettings&
            Settings() const noexcept
        {
            return m_settings;
        }
        // 非有限値を既定値へ戻し、各値を許容範囲へ収めた設定を返します(settings: 補正する設定)。
        [[nodiscard]] static Blink2DSettings Sanitize(
            Blink2DSettings settings) noexcept;

        // 目が開いていれば、次の更新から瞬きを始めます。
        void Blink() noexcept;
        // 目を閉じたままにするかを設定し、解除すると閉じた目の表示秒数の後に開きます(holdClosed: 閉じたままにする指定)。
        void SetHoldClosed(const bool holdClosed) noexcept
        {
            m_holdClosed = holdClosed;
        }
        // 目を閉じたままにする指定を返します。
        [[nodiscard]] bool HoldClosed() const noexcept
        {
            return m_holdClosed;
        }
        // 間隔の乱数を固定し、次の瞬きまでの時間を引き直します(seed: 乱数の種)。
        void SetRandomSeed(std::uint32_t seed) noexcept;

        // 閉じ始めてから開ききるまでの間か返します。
        [[nodiscard]] bool IsBlinking() const noexcept
        {
            return m_phase != Phase::Open;
        }
        // 現在表示するシートのコマ番号を返します。
        [[nodiscard]] int CurrentFrame() const noexcept;
        // 目が開いている間の、次の瞬きまでの残り秒数を返します。
        [[nodiscard]] float SecondsUntilNextBlink() const noexcept
        {
            return m_secondsUntilBlink;
        }

        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "Blink2D";
        }

    protected:
        // 乱数の種を決めて最初の間隔を引き、開いた目のコマを反映します(graphics: 使用しない描画装置)。
        void OnInitialize(GraphicsDevice& graphics) override;
        // 経過秒数だけ瞬きを進めて対象のコマを更新します(deltaTime: 経過秒数)。
        void OnUpdate(float deltaTime) override;
        // 無効化時は開いた目へ戻します(active: 新しい稼働状態)。
        void OnActiveStateChanged(bool active) override;

    private:
        // 瞬きの段階
        enum class Phase : std::uint8_t
        {
            // 開いた目で次の瞬きを待つ
            Open,
            // 途中のコマで閉じている
            Closing,
            // 閉じた目を表示している
            Closed,
            // 途中のコマを逆順にして開いている
            Opening
        };

        // 経過秒数だけ段階を進めます(deltaTime: 0以上の経過秒数)。
        void Advance(float deltaTime) noexcept;
        // 次の瞬きまでの秒数を引きます(allowDouble: 二度瞬きを許す指定)。
        void ScheduleNextBlink(bool allowDouble) noexcept;
        // 最小値と最大値の間の乱数を返します(minimum: 最小値, maximum: 最大値)。
        [[nodiscard]] float RandomRange(
            float minimum,
            float maximum) noexcept;
        // 自身と対象の子孫のSprite Rendererへコマを反映します(frame: シートのコマ番号)。
        void ApplyFrame(int frame);

        // 補正済みの瞬き設定
        Blink2DSettings m_settings;
        // 間隔を引く乱数
        std::minstd_rand m_random;
        // 現在の瞬きの段階
        Phase m_phase{ Phase::Open };
        // 現在の段階の経過秒数
        float m_phaseSeconds{};
        // 次の瞬きまでの残り秒数
        float m_secondsUntilBlink{};
        // 直前の間隔が二度瞬きだったか
        bool m_lastWasDouble{};
        // 目を閉じたままにする指定
        bool m_holdClosed{};
        // 乱数の種を明示したか
        bool m_seeded{};
        // 初期化を済ませたか
        bool m_ready{};
    };
}
