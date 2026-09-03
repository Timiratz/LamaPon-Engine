#pragma once

#include "LamaPon/Scene/Component.h"

#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    // スプライトシート内の1アニメーション（歩き、待機など）。
    // コマ番号はシートの左上から行方向（右へ、次に下へ）に数えます。
    struct SpriteAnimationClip final
    {
        std::string name;
        int startFrame{};
        int frameCount{ 1 };
        float framesPerSecond{ 10.0f };
        bool loop{ true };
    };

    // 同じGameObjectのSpriteRendererをスプライトシートで
    // コマ送りアニメーションさせます（フリップブック）。
    // シートは columns × rows の均等グリッドとして扱います。
    class SpriteAnimatorComponent final : public Component
    {
    public:
        explicit SpriteAnimatorComponent(
            int columns = 1,
            int rows = 1) noexcept;

        void SetSheetGrid(int columns, int rows) noexcept;
        [[nodiscard]] int Columns() const noexcept
        {
            return m_columns;
        }
        [[nodiscard]] int Rows() const noexcept
        {
            return m_rows;
        }

        // 同名クリップは置き換えます。
        void AddClip(SpriteAnimationClip clip);
        void RemoveClip(std::string_view name);
        [[nodiscard]] std::vector<SpriteAnimationClip>&
            Clips() noexcept
        {
            return m_clips;
        }
        [[nodiscard]] const std::vector<
            SpriteAnimationClip>& Clips() const noexcept
        {
            return m_clips;
        }

        // クリップを最初のコマから再生します。
        // 見つからない場合はfalseを返します。
        bool Play(std::string_view clipName);
        void Stop() noexcept { m_playing = false; }
        [[nodiscard]] bool IsPlaying() const noexcept
        {
            return m_playing;
        }
        [[nodiscard]] const std::string&
            ActiveClipName() const noexcept
        {
            return m_activeClip;
        }
        // 現在表示中のシート内コマ番号（クリップ未再生なら-1）。
        [[nodiscard]] int CurrentFrame() const noexcept
        {
            return m_currentFrame;
        }

        void SetSpeed(const float speed) noexcept
        {
            m_speed = speed;
        }
        [[nodiscard]] float Speed() const noexcept
        {
            return m_speed;
        }
        // 再生開始時に自動再生するクリップ名（空なら先頭クリップ）。
        void SetDefaultClip(std::string name)
        {
            m_defaultClip = std::move(name);
        }
        [[nodiscard]] const std::string&
            DefaultClip() const noexcept
        {
            return m_defaultClip;
        }
        void SetPlayOnStart(const bool playOnStart) noexcept
        {
            m_playOnStart = playOnStart;
        }
        [[nodiscard]] bool PlayOnStart() const noexcept
        {
            return m_playOnStart;
        }

        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "SpriteAnimator";
        }

    protected:
        void OnUpdate(float deltaTime) override;

    private:
        [[nodiscard]] const SpriteAnimationClip*
            FindClip(std::string_view name) const noexcept;
        // 現在のコマをSpriteRendererのソース矩形へ反映します。
        void ApplyFrame(int sheetFrame);

        int m_columns{ 1 };
        int m_rows{ 1 };
        std::vector<SpriteAnimationClip> m_clips;
        std::string m_defaultClip;
        std::string m_activeClip;
        float m_speed{ 1.0f };
        float m_time{};
        int m_currentFrame{ -1 };
        bool m_playing{};
        bool m_playOnStart{ true };
        bool m_started{};
    };
}
