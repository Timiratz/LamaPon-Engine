#include "LamaPon/Components/Blink2DComponent.h"

#include "LamaPon/Components/SpriteRendererComponent.h"
#include "LamaPon/Scene/GameObject.h"

#include <algorithm>
#include <cmath>

namespace LamaPon
{
    namespace
    {
        // 1フレームで進める最大秒数
        constexpr float MaximumFrameSeconds = 1.0f;
        // 1フレームで切り替える段階の最大数
        constexpr int MaximumPhaseSteps = 256;
        // 二度瞬きで次に瞬くまでの最短秒数
        constexpr float DoubleBlinkMinimumSeconds = 0.1f;
        // 二度瞬きで次に瞬くまでの最長秒数
        constexpr float DoubleBlinkMaximumSeconds = 0.25f;
        // シート番号の上限
        constexpr int MaximumFrame = 65535;

        // 有限値ならそのまま、非有限なら代替値を返します(value: 確認する値, fallback: 代替値)。
        [[nodiscard]] float FiniteOr(
            const float value,
            const float fallback) noexcept
        {
            return std::isfinite(value) ? value : fallback;
        }

        // 物体と対象の子孫のSprite Rendererへ画像領域を設定します(object: 設定する物体, sourceRect: 正規化した画像領域, owner: 設定元の瞬き, includeChildren: 子孫も設定する指定)。
        void ApplySourceRect(
            GameObject& object,
            const DirectX::XMFLOAT4& sourceRect,
            const Blink2DComponent& owner,
            const bool includeChildren)
        {
            // 物体のスプライト描画
            if (auto* sprite =
                    object.GetComponent<SpriteRendererComponent>())
            {
                sprite->SetSourceRect(sourceRect);
            }
            if (!includeChildren)
            {
                return;
            }
            // 設定を伝える子
            for (GameObject* child : object.Children())
            {
                // 子が持つ別の瞬き
                const auto* childBlink =
                    child->GetComponent<Blink2DComponent>();
                if (childBlink != nullptr && childBlink != &owner)
                {
                    continue;
                }
                ApplySourceRect(
                    *child,
                    sourceRect,
                    owner,
                    true);
            }
        }
    }

    Blink2DComponent::Blink2DComponent(
        const Blink2DSettings& settings) noexcept
        : m_settings(Sanitize(settings))
    {
    }

    void Blink2DComponent::SetSettings(
        const Blink2DSettings& settings)
    {
        m_settings = Sanitize(settings);
        m_secondsUntilBlink = std::min(
            m_secondsUntilBlink,
            m_settings.intervalMaxSeconds);
        if (m_ready)
        {
            ApplyFrame(CurrentFrame());
        }
    }

    Blink2DSettings Blink2DComponent::Sanitize(
        Blink2DSettings settings) noexcept
    {
        // 非有限値の置き換えに使う既定設定
        const Blink2DSettings defaults{};
        settings.columns = std::clamp(settings.columns, 1, 256);
        settings.rows = std::clamp(settings.rows, 1, 256);
        settings.openFrame = std::clamp(
            settings.openFrame,
            0,
            MaximumFrame);
        settings.closingStartFrame = std::clamp(
            settings.closingStartFrame,
            0,
            MaximumFrame);
        settings.closingFrameCount = std::clamp(
            settings.closingFrameCount,
            1,
            64);
        settings.frameSeconds = std::clamp(
            FiniteOr(settings.frameSeconds, defaults.frameSeconds),
            0.001f,
            10.0f);
        settings.closedSeconds = std::clamp(
            FiniteOr(settings.closedSeconds, defaults.closedSeconds),
            0.0f,
            10.0f);
        settings.intervalMinSeconds = std::clamp(
            FiniteOr(
                settings.intervalMinSeconds,
                defaults.intervalMinSeconds),
            0.05f,
            3600.0f);
        settings.intervalMaxSeconds = std::clamp(
            FiniteOr(
                settings.intervalMaxSeconds,
                defaults.intervalMaxSeconds),
            settings.intervalMinSeconds,
            3600.0f);
        settings.doubleBlinkChance = std::clamp(
            FiniteOr(
                settings.doubleBlinkChance,
                defaults.doubleBlinkChance),
            0.0f,
            1.0f);
        return settings;
    }

    void Blink2DComponent::Blink() noexcept
    {
        if (m_phase == Phase::Open)
        {
            m_phase = Phase::Closing;
            m_phaseSeconds = 0.0f;
        }
    }

    void Blink2DComponent::SetRandomSeed(
        const std::uint32_t seed) noexcept
    {
        m_random.seed(seed);
        m_seeded = true;
        ScheduleNextBlink(false);
    }

    int Blink2DComponent::CurrentFrame() const noexcept
    {
        // 閉じきる前の途中コマ数
        const int transitional =
            m_settings.closingFrameCount - 1;
        // 経過秒数から求めた途中コマの位置
        const int step = transitional > 0
            ? std::min(
                static_cast<int>(
                    m_phaseSeconds / m_settings.frameSeconds),
                transitional - 1)
            : 0;
        switch (m_phase)
        {
        case Phase::Closing:
            return m_settings.closingStartFrame + step;
        case Phase::Closed:
            return m_settings.closingStartFrame + transitional;
        case Phase::Opening:
            return m_settings.closingStartFrame
                + std::max(transitional - 1 - step, 0);
        case Phase::Open:
        default:
            return m_settings.openFrame;
        }
    }

    void Blink2DComponent::OnInitialize(GraphicsDevice&)
    {
        if (!m_seeded)
        {
            // 物体ごとに瞬きの時刻をずらす乱数の種
            const auto seed = static_cast<std::uint32_t>(
                (Owner().Id() * 2654435761ull) ^ 0x5bd1e995ull);
            m_random.seed(seed);
        }
        ScheduleNextBlink(false);
        m_ready = true;
        ApplyFrame(CurrentFrame());
    }

    void Blink2DComponent::OnUpdate(const float deltaTime)
    {
        Advance(
            std::isfinite(deltaTime)
                ? std::clamp(
                    deltaTime,
                    0.0f,
                    MaximumFrameSeconds)
                : 0.0f);
        ApplyFrame(CurrentFrame());
    }

    void Blink2DComponent::OnActiveStateChanged(
        const bool active)
    {
        if (active || !m_ready)
        {
            return;
        }
        m_phase = Phase::Open;
        m_phaseSeconds = 0.0f;
        ApplyFrame(m_settings.openFrame);
    }

    void Blink2DComponent::Advance(float deltaTime) noexcept
    {
        // 片道の途中コマを表示する合計秒数
        const float transitionSeconds =
            static_cast<float>(
                m_settings.closingFrameCount - 1)
            * m_settings.frameSeconds;
        // 段階を切り替えた回数
        for (int stepCount = 0;
            stepCount < MaximumPhaseSteps;
            ++stepCount)
        {
            switch (m_phase)
            {
            case Phase::Open:
                if (!m_holdClosed)
                {
                    if (!m_settings.autoBlink
                        || m_secondsUntilBlink > deltaTime)
                    {
                        if (m_settings.autoBlink)
                        {
                            m_secondsUntilBlink -= deltaTime;
                        }
                        return;
                    }
                    deltaTime -= m_secondsUntilBlink;
                }
                m_secondsUntilBlink = 0.0f;
                m_phase = Phase::Closing;
                m_phaseSeconds = 0.0f;
                break;
            case Phase::Closing:
                if (m_phaseSeconds + deltaTime < transitionSeconds)
                {
                    m_phaseSeconds += deltaTime;
                    return;
                }
                deltaTime -= transitionSeconds - m_phaseSeconds;
                m_phase = Phase::Closed;
                m_phaseSeconds = 0.0f;
                break;
            case Phase::Closed:
                if (m_holdClosed)
                {
                    return;
                }
                if (m_phaseSeconds + deltaTime
                    < m_settings.closedSeconds)
                {
                    m_phaseSeconds += deltaTime;
                    return;
                }
                deltaTime -= std::max(
                    m_settings.closedSeconds - m_phaseSeconds,
                    0.0f);
                m_phase = Phase::Opening;
                m_phaseSeconds = 0.0f;
                break;
            case Phase::Opening:
                if (m_phaseSeconds + deltaTime < transitionSeconds)
                {
                    m_phaseSeconds += deltaTime;
                    return;
                }
                deltaTime -= transitionSeconds - m_phaseSeconds;
                m_phase = Phase::Open;
                m_phaseSeconds = 0.0f;
                ScheduleNextBlink(true);
                break;
            }
            deltaTime = std::max(deltaTime, 0.0f);
        }
    }

    void Blink2DComponent::ScheduleNextBlink(
        const bool allowDouble) noexcept
    {
        if (allowDouble
            && !m_lastWasDouble
            && m_settings.doubleBlinkChance > 0.0f
            && RandomRange(0.0f, 1.0f)
                < m_settings.doubleBlinkChance)
        {
            m_secondsUntilBlink = RandomRange(
                DoubleBlinkMinimumSeconds,
                DoubleBlinkMaximumSeconds);
            m_lastWasDouble = true;
            return;
        }
        m_secondsUntilBlink = RandomRange(
            m_settings.intervalMinSeconds,
            m_settings.intervalMaxSeconds);
        m_lastWasDouble = false;
    }

    float Blink2DComponent::RandomRange(
        const float minimum,
        const float maximum) noexcept
    {
        // 0〜1へ正規化した乱数
        const double unit =
            static_cast<double>(
                m_random() - std::minstd_rand::min())
            / static_cast<double>(
                std::minstd_rand::max()
                - std::minstd_rand::min());
        return maximum <= minimum
            ? minimum
            : minimum
                + static_cast<float>(unit)
                    * (maximum - minimum);
    }

    void Blink2DComponent::ApplyFrame(const int frame)
    {
        // シート全体のコマ数
        const int totalFrames =
            m_settings.columns * m_settings.rows;
        // シート周回後の表示コマ番号
        const int sheetFrame = frame % totalFrames;
        // 正規化したコマの幅
        const float cellWidth =
            1.0f / static_cast<float>(m_settings.columns);
        // 正規化したコマの高さ
        const float cellHeight =
            1.0f / static_cast<float>(m_settings.rows);
        ApplySourceRect(
            Owner(),
            {
                static_cast<float>(
                    sheetFrame % m_settings.columns)
                    * cellWidth,
                static_cast<float>(
                    sheetFrame / m_settings.columns)
                    * cellHeight,
                cellWidth,
                cellHeight
            },
            *this,
            m_settings.includeChildren);
    }
}
