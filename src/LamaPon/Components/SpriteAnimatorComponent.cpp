#include "LamaPon/Components/SpriteAnimatorComponent.h"

#include "LamaPon/Components/SpriteRendererComponent.h"
#include "LamaPon/Scene/GameObject.h"

#include <algorithm>
#include <cmath>

namespace LamaPon
{
    SpriteAnimatorComponent::SpriteAnimatorComponent(
        const int columns,
        const int rows) noexcept
        : m_columns(std::max(columns, 1))
        , m_rows(std::max(rows, 1))
    {
    }

    void SpriteAnimatorComponent::SetSheetGrid(
        const int columns,
        const int rows) noexcept
    {
        m_columns = std::max(columns, 1);
        m_rows = std::max(rows, 1);
    }

    void SpriteAnimatorComponent::AddClip(
        SpriteAnimationClip clip)
    {
        clip.startFrame = std::max(clip.startFrame, 0);
        clip.frameCount = std::max(clip.frameCount, 1);
        clip.framesPerSecond =
            std::max(clip.framesPerSecond, 0.01f);
        // 同名か確認する登録クリップ
        for (auto& existing : m_clips)
        {
            if (existing.name == clip.name)
            {
                existing = std::move(clip);
                return;
            }
        }
        m_clips.push_back(std::move(clip));
    }

    void SpriteAnimatorComponent::RemoveClip(
        const std::string_view name)
    {
        // 指定名と一致するクリップを除去する(clip: 判定する登録クリップ)。
        std::erase_if(
            m_clips,
            [name](const SpriteAnimationClip& clip)
            {
                return clip.name == name;
            });
        if (m_activeClip == name)
        {
            m_activeClip.clear();
            m_playing = false;
        }
    }

    bool SpriteAnimatorComponent::Play(
        const std::string_view clipName)
    {
        // 対象の登録クリップ
        const auto* clip = FindClip(clipName);
        if (clip == nullptr)
        {
            return false;
        }
        m_activeClip = clip->name;
        m_time = 0.0f;
        m_playing = true;
        ApplyFrame(clip->startFrame);
        return true;
    }

    void SpriteAnimatorComponent::OnUpdate(
        const float deltaTime)
    {
        // 自動再生が有効なら初回更新より前のPlayやStopもここで上書きする。
        if (!m_started)
        {
            m_started = true;
            if (m_playOnStart)
            {
                if (!m_defaultClip.empty())
                {
                    static_cast<void>(
                        Play(m_defaultClip));
                }
                else if (!m_clips.empty())
                {
                    static_cast<void>(
                        Play(m_clips.front().name));
                }
            }
        }

        if (!m_playing)
        {
            return;
        }
        // 対象の登録クリップ
        const auto* clip = FindClip(m_activeClip);
        if (clip == nullptr)
        {
            m_playing = false;
            return;
        }

        m_time += deltaTime * m_speed;
        // 経過時間による進行コマ数
        const int advanced = static_cast<int>(
            std::floor(
                m_time * clip->framesPerSecond));
        // クリップ内の表示コマ番号
        int index = advanced;
        if (clip->loop)
        {
            index = advanced % clip->frameCount;
            if (index < 0)
            {
                index += clip->frameCount;
            }
        }
        else if (advanced >= clip->frameCount)
        {

            index = clip->frameCount - 1;
            m_playing = false;
        }
        else if (index < 0)
        {
            index = 0;
        }
        ApplyFrame(clip->startFrame + index);
    }

    const SpriteAnimationClip*
        SpriteAnimatorComponent::FindClip(
            const std::string_view name) const noexcept
    {
        // 対象の登録クリップ
        for (const auto& clip : m_clips)
        {
            if (clip.name == name)
            {
                return &clip;
            }
        }
        return nullptr;
    }

    void SpriteAnimatorComponent::ApplyFrame(
        const int sheetFrame)
    {
        m_currentFrame = sheetFrame;
        // 同じ所有者のスプライト描画
        auto* sprite = Owner().GetComponent<
            SpriteRendererComponent>();
        if (sprite == nullptr)
        {
            return;
        }
        // シート全体のコマ数
        const int totalFrames = m_columns * m_rows;
        // シート周回後の表示コマ番号
        const int frame = totalFrames > 0
            ? sheetFrame % totalFrames
            : 0;
        // 正規化したコマの幅
        const float cellWidth =
            1.0f / static_cast<float>(m_columns);
        // 正規化したコマの高さ
        const float cellHeight =
            1.0f / static_cast<float>(m_rows);
        sprite->SetSourceRect({
            static_cast<float>(frame % m_columns)
                * cellWidth,
            static_cast<float>(frame / m_columns)
                * cellHeight,
            cellWidth,
            cellHeight });
    }
}
