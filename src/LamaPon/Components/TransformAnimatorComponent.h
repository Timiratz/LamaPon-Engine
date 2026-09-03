#pragma once

#include "LamaPon/Animation/AnimationClip.h"
#include "LamaPon/Animation/AnimatorController.h"
#include "LamaPon/Scene/Component.h"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>

namespace LamaPon
{
    class AssetManager;

    class TransformAnimatorComponent final : public Component
    {
    public:
        explicit TransformAnimatorComponent(
            std::filesystem::path clipPath = {},
            float speed = 1.0f,
            bool loop = true,
            bool playOnStart = true,
            std::filesystem::path controllerPath = {});

        void SetClipPath(
            std::filesystem::path path);
        [[nodiscard]] const std::filesystem::path&
            ClipPath() const noexcept
        {
            return m_clipPath;
        }
        void SetSpeed(float speed);
        void SetControllerPath(
            std::filesystem::path path);
        [[nodiscard]] const std::filesystem::path&
            ControllerPath() const noexcept
        {
            return m_controllerPath;
        }
        [[nodiscard]] float Speed() const noexcept
        {
            return m_speed;
        }
        void SetLoop(bool loop) noexcept
        {
            m_loop = loop;
        }
        [[nodiscard]] bool Loop() const noexcept
        {
            return m_loop;
        }
        void SetPlayOnStart(bool enabled) noexcept
        {
            m_playOnStart = enabled;
        }
        [[nodiscard]] bool PlayOnStart() const noexcept
        {
            return m_playOnStart;
        }

        void Play() noexcept;
        void Pause() noexcept;
        void Stop() noexcept;
        void ReloadClip();
        void ReloadController();
        void SetTrigger(std::string trigger);
        void SetTime(float time) noexcept;
        [[nodiscard]] bool IsPlaying() const noexcept
        {
            return m_playing;
        }
        [[nodiscard]] float Time() const noexcept
        {
            return m_time;
        }
        [[nodiscard]] float Duration() const noexcept
        {
            return m_clip != nullptr
                ? m_clip->Duration()
                : 0.0f;
        }
        [[nodiscard]] std::size_t KeyframeCount() const noexcept
        {
            return m_clip != nullptr
                ? m_clip->Keyframes().size()
                : 0;
        }
        [[nodiscard]] const std::string&
            CurrentState() const noexcept
        {
            return m_currentState;
        }
        [[nodiscard]] bool IsTransitioning() const noexcept
        {
            return m_nextClip != nullptr;
        }

        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "TransformAnimator";
        }

    protected:
        void OnInitialize(
            GraphicsDevice& graphics) override;
        void OnUpdate(float deltaTime) override;

    private:
        void LoadClip();
        void LoadController();
        void EnterState(
            const AnimatorState& state);
        void StartTransition(
            const AnimatorTransition& transition);
        void UpdateController(float deltaTime);
        void ApplyCurrentSample() noexcept;

        std::filesystem::path m_clipPath;
        std::filesystem::path m_controllerPath;
        float m_speed{ 1.0f };
        bool m_loop{ true };
        bool m_playOnStart{ true };
        bool m_playing{};
        float m_time{};
        AssetManager* m_assets{};
        std::shared_ptr<const AnimationClip> m_clip;
        std::shared_ptr<const AnimatorController>
            m_controller;
        std::shared_ptr<const AnimationClip> m_nextClip;
        std::string m_currentState;
        std::string m_nextState;
        float m_nextTime{};
        float m_transitionTime{};
        float m_transitionDuration{};
        std::unordered_set<std::string>
            m_activeTriggers;
    };
}
