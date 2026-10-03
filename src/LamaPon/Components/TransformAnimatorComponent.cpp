#include "LamaPon/Components/TransformAnimatorComponent.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/Transform.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace
{
    // 正の長さを持つクリップの時刻を進め、末尾で周回または停止する(time: 現在の再生秒数, delta: 進める有限な秒数, duration: クリップ長の正の秒数, loop: 末尾で周回する指定)。
    float AdvanceAnimationTime(
        const float time,
        const float delta,
        const float duration,
        const bool loop) noexcept
    {
        // 進行後の再生秒数
        const float advanced = time + delta;
        if (advanced < duration)
        {
            return advanced;
        }
        return loop
            ? std::fmod(advanced, duration)
            : duration;
    }

    // 二つの値を指定割合で線形補間する(from: 補間元の値, to: 補間先の値, amount: 補間の割合)。
    float Lerp(
        const float from,
        const float to,
        const float amount) noexcept
    {
        return from + (to - from) * amount;
    }

    // 三次元の各成分を線形補間する(from: 補間元の値, to: 補間先の値, amount: 補間の割合)。
    DirectX::XMFLOAT3 LerpFloat3(
        const DirectX::XMFLOAT3& from,
        const DirectX::XMFLOAT3& to,
        const float amount) noexcept
    {
        return {
            Lerp(from.x, to.x, amount),
            Lerp(from.y, to.y, amount),
            Lerp(from.z, to.z, amount)
        };
    }

    // ラジアン角の差を最短の一周内に収めて補間する(from: 補間元のラジアン角, to: 補間先のラジアン角, amount: 補間の割合)。
    float LerpAngle(
        const float from,
        const float to,
        const float amount) noexcept
    {
        // ラジアンでの一周
        constexpr float TwoPi =
            std::numbers::pi_v<float> * 2.0f;
        return from
            + std::remainder(to - from, TwoPi)
                * amount;
    }

    // オイラー角の各軸を最短の一周内で補間する(from: 補間元のラジアン角, to: 補間先のラジアン角, amount: 補間の割合)。
    DirectX::XMFLOAT3 LerpRotation(
        const DirectX::XMFLOAT3& from,
        const DirectX::XMFLOAT3& to,
        const float amount) noexcept
    {
        return {
            LerpAngle(from.x, to.x, amount),
            LerpAngle(from.y, to.y, amount),
            LerpAngle(from.z, to.z, amount)
        };
    }
}

namespace LamaPon
{
    TransformAnimatorComponent::
        TransformAnimatorComponent(
            std::filesystem::path clipPath,
            const float speed,
            const bool loop,
            const bool playOnStart,
            std::filesystem::path controllerPath)
        : m_clipPath(std::move(clipPath))
        , m_controllerPath(
            std::move(controllerPath))
        , m_loop(loop)
        , m_playOnStart(playOnStart)
    {
        SetSpeed(speed);
    }

    void TransformAnimatorComponent::SetClipPath(
        std::filesystem::path path)
    {
        if (path == m_clipPath
            && m_controllerPath.empty())
        {
            return;
        }
        // 切り替え前に読む新クリップ
        std::shared_ptr<const AnimationClip> newClip;
        if (m_assets != nullptr && !path.empty())
        {
            newClip =
                m_assets->LoadAnimationClip(path);
        }
        m_clipPath = std::move(path);
        m_clip = std::move(newClip);
        m_controllerPath.clear();
        m_controller.reset();
        m_currentState.clear();
        m_nextClip.reset();
        m_activeTriggers.clear();
        m_time = 0.0f;
        ApplyCurrentSample();
    }

    void TransformAnimatorComponent::SetControllerPath(
        std::filesystem::path path)
    {
        if (path == m_controllerPath)
        {
            return;
        }
        if (m_assets != nullptr && !path.empty())
        {
            // 切り替え前に読む制御器
            const auto controller =
                m_assets->LoadAnimatorController(path);
            // 制御器の入口状態
            const auto* entry =
                controller->FindState(
                    controller->EntryState());
            if (entry == nullptr)
            {
                throw std::runtime_error(
                    "Animator entry state is missing.");
            }
            // 切り替え前に読む入口クリップ
            const auto clip =
                m_assets->LoadAnimationClip(
                    entry->clipPath);
            m_controllerPath = std::move(path);
            m_controller = controller;
            m_clip = clip;
            m_currentState = entry->name;
        }
        else
        {
            m_controllerPath = std::move(path);
            m_controller.reset();
            m_currentState.clear();
            if (m_assets != nullptr
                && !m_clipPath.empty())
            {
                m_clip =
                    m_assets->LoadAnimationClip(
                        m_clipPath);
            }
        }
        m_nextClip.reset();
        m_activeTriggers.clear();
        m_time = 0.0f;
        ApplyCurrentSample();
    }

    void TransformAnimatorComponent::SetSpeed(
        const float speed)
    {
        if (!std::isfinite(speed)
            || speed <= 0.0f
            || speed > 100.0f)
        {
            throw std::invalid_argument(
                "Animation speed must be greater than 0 and at most 100.");
        }
        m_speed = speed;
    }

    void TransformAnimatorComponent::Play() noexcept
    {
        if (m_clip != nullptr)
        {
            if (m_time >= m_clip->Duration())
            {
                m_time = 0.0f;
            }
            m_playing = true;
        }
    }

    void TransformAnimatorComponent::Pause() noexcept
    {
        m_playing = false;
    }

    void TransformAnimatorComponent::Stop() noexcept
    {
        m_playing = false;
        m_time = 0.0f;
        m_nextClip.reset();
        m_nextState.clear();
        m_activeTriggers.clear();
        ApplyCurrentSample();
    }

    void TransformAnimatorComponent::ReloadClip()
    {
        if (!m_controllerPath.empty())
        {
            ReloadController();
            return;
        }
        m_clip.reset();
        m_time = 0.0f;
        if (m_assets != nullptr
            && !m_clipPath.empty())
        {
            m_clip =
                m_assets->ReloadAnimationClip(
                    m_clipPath);
        }
        ApplyCurrentSample();
    }

    void TransformAnimatorComponent::ReloadController()
    {
        m_controller.reset();
        m_clip.reset();
        m_nextClip.reset();
        m_time = 0.0f;
        if (m_assets != nullptr
            && !m_controllerPath.empty())
        {
            m_controller =
                m_assets->ReloadAnimatorController(
                    m_controllerPath);
            // 制御器の入口状態
            const auto* entry =
                m_controller->FindState(
                    m_controller->EntryState());
            if (entry == nullptr)
            {
                throw std::runtime_error(
                    "Animator entry state is missing.");
            }
            EnterState(*entry);
        }
        ApplyCurrentSample();
    }

    void TransformAnimatorComponent::SetTrigger(
        std::string trigger)
    {
        if (trigger.empty()
            || trigger.size() > 96)
        {
            throw std::invalid_argument(
                "Animator trigger must contain between 1 and 96 characters.");
        }
        m_activeTriggers.insert(
            std::move(trigger));
    }

    void TransformAnimatorComponent::SetTime(
        const float time) noexcept
    {
        if (m_clip == nullptr)
        {
            m_time = 0.0f;
            return;
        }
        m_time = std::clamp(
            std::isfinite(time) ? time : 0.0f,
            0.0f,
            m_clip->Duration());
        ApplyCurrentSample();
    }

    void TransformAnimatorComponent::OnInitialize(
        GraphicsDevice& graphics)
    {
        m_assets = &graphics.Assets();
        if (!m_controllerPath.empty())
        {
            LoadController();
        }
        else
        {
            LoadClip();
        }
        m_playing =
            m_playOnStart && m_clip != nullptr;
        ApplyCurrentSample();
    }

    void TransformAnimatorComponent::OnUpdate(
        const float deltaTime)
    {
        if (!m_playing
            || m_clip == nullptr
            || deltaTime <= 0.0f)
        {
            return;
        }
        if (m_controller != nullptr)
        {
            UpdateController(deltaTime);
            return;
        }

        m_time = AdvanceAnimationTime(
            m_time,
            deltaTime * m_speed,
            m_clip->Duration(),
            m_loop);
        if (!m_loop
            && m_time >= m_clip->Duration())
        {
            m_playing = false;
        }
        ApplyCurrentSample();
    }

    void TransformAnimatorComponent::LoadClip()
    {
        m_clip.reset();
        m_time = 0.0f;
        if (m_assets != nullptr
            && !m_clipPath.empty())
        {
            m_clip =
                m_assets->LoadAnimationClip(
                    m_clipPath);
        }
    }

    void TransformAnimatorComponent::LoadController()
    {
        m_controller.reset();
        m_clip.reset();
        m_nextClip.reset();
        m_currentState.clear();
        m_time = 0.0f;
        if (m_assets == nullptr
            || m_controllerPath.empty())
        {
            return;
        }
        m_controller =
            m_assets->LoadAnimatorController(
                m_controllerPath);
        // 制御器の入口状態
        const auto* entry =
            m_controller->FindState(
                m_controller->EntryState());
        if (entry == nullptr)
        {
            throw std::runtime_error(
                "Animator entry state is missing.");
        }
        EnterState(*entry);
    }

    void TransformAnimatorComponent::EnterState(
        const AnimatorState& state)
    {
        if (m_assets == nullptr)
        {
            return;
        }
        m_clip =
            m_assets->LoadAnimationClip(
                state.clipPath);
        m_currentState = state.name;
        m_time = 0.0f;
        m_nextClip.reset();
        m_nextState.clear();
        m_nextTime = 0.0f;
        m_transitionTime = 0.0f;
        m_transitionDuration = 0.0f;
    }

    void TransformAnimatorComponent::StartTransition(
        const AnimatorTransition& transition)
    {
        if (m_assets == nullptr
            || m_controller == nullptr)
        {
            return;
        }
        // 遷移先の登録状態
        const auto* targetState =
            m_controller->FindState(transition.to);
        if (targetState == nullptr)
        {
            return;
        }
        // 遷移先の共有クリップ
        auto targetClip =
            m_assets->LoadAnimationClip(
                targetState->clipPath);
        if (!transition.trigger.empty())
        {
            m_activeTriggers.erase(
                transition.trigger);
        }
        if (transition.duration <= 0.0f)
        {
            m_clip = std::move(targetClip);
            m_currentState = targetState->name;
            m_time = 0.0f;
            ApplyCurrentSample();
            return;
        }
        m_nextClip = std::move(targetClip);
        m_nextState = targetState->name;
        m_nextTime = 0.0f;
        m_transitionTime = 0.0f;
        m_transitionDuration =
            transition.duration;
    }

    void TransformAnimatorComponent::UpdateController(
        const float deltaTime)
    {
        // 現在の登録状態
        const auto* current =
            m_controller->FindState(
                m_currentState);
        if (current == nullptr)
        {
            return;
        }
        m_time = AdvanceAnimationTime(
            m_time,
            deltaTime * m_speed
                * current->speed,
            m_clip->Duration(),
            current->loop);

        if (m_nextClip == nullptr)
        {
            // クリップ長に対する再生割合
            const float normalizedTime =
                m_clip->Duration() > 0.0f
                    ? m_time / m_clip->Duration()
                    : 1.0f;
            // 現在状態から選ぶ遷移
            if (const auto* transition =
                    m_controller->FindTransition(
                        m_currentState,
                        normalizedTime,
                        m_activeTriggers))
            {
                StartTransition(*transition);
            }
        }

        if (m_nextClip == nullptr)
        {
            ApplyCurrentSample();
            return;
        }

        // ブレンド先の登録状態
        const auto* next =
            m_controller->FindState(
                m_nextState);
        if (next == nullptr)
        {
            m_nextClip.reset();
            ApplyCurrentSample();
            return;
        }
        m_nextTime = AdvanceAnimationTime(
            m_nextTime,
            deltaTime * m_speed * next->speed,
            m_nextClip->Duration(),
            next->loop);
        m_transitionTime += deltaTime;
        // 実経過時間によるブレンド割合
        const float amount = std::clamp(
            m_transitionTime
                / m_transitionDuration,
            0.0f,
            1.0f);
        // 現在クリップの変換サンプル
        const auto from =
            m_clip->Sample(m_time);
        // 遷移先クリップの変換サンプル
        const auto to =
            m_nextClip->Sample(m_nextTime);
        // 適用するローカルTransform
        auto& transform = GetTransform();
        transform.position = LerpFloat3(
            from.position,
            to.position,
            amount);
        // クリップのオイラー角をクォータニオンへ変換し、姿勢を最短経路で補間する。
        {
            using namespace DirectX;
            transform.SetRotationVector(
                XMQuaternionSlerp(
                    XMQuaternionRotationRollPitchYaw(
                        from.rotation.x,
                        from.rotation.y,
                        from.rotation.z),
                    XMQuaternionRotationRollPitchYaw(
                        to.rotation.x,
                        to.rotation.y,
                        to.rotation.z),
                    amount));
        }
        transform.scale = LerpFloat3(
            from.scale,
            to.scale,
            amount);

        if (amount >= 1.0f)
        {
            m_clip = std::move(m_nextClip);
            m_currentState =
                std::move(m_nextState);
            m_time = m_nextTime;
            m_nextTime = 0.0f;
            m_transitionTime = 0.0f;
            m_transitionDuration = 0.0f;
        }
    }

    void TransformAnimatorComponent::
        ApplyCurrentSample() noexcept
    {
        if (m_clip == nullptr)
        {
            return;
        }
        // 現在クリップの変換サンプル
        const auto sample =
            m_clip->Sample(m_time);
        // 適用するローカルTransform
        auto& transform = GetTransform();
        transform.position = sample.position;
        // 適用する回転クォータニオン
        const auto rotation =
            m_clip->SampleRotationQuaternion(m_time);
        transform.SetRotationVector(
            DirectX::XMLoadFloat4(&rotation));
        transform.scale = sample.scale;
    }
}
