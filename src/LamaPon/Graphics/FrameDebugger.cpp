#include "LamaPon/Graphics/FrameDebugger.h"

#include <utility>

namespace LamaPon
{
    void FrameDebugger::SetEnabled(const bool enabled) noexcept
    {
        if (m_enabled == enabled)
        {
            return;
        }
        m_enabled = enabled;
        // 途中から有効にしたフレームは区間の入れ子が不完全なので、
        // 次のEndFrameまでの記録は捨てられる前提で空から始めます。
        m_sections.clear();
        m_droppedSections = 0;
        m_currentFrame.clear();
        m_nextIndex = 0;
        if (!enabled)
        {
            m_lastFrame.clear();
        }
    }

    void FrameDebugger::SetEventLimit(
        const std::optional<std::uint32_t> limit) noexcept
    {
        m_limit = limit;
    }

    std::string FrameDebugger::CurrentSectionPath() const
    {
        std::string path;
        for (const auto& section : m_sections)
        {
            if (!path.empty())
            {
                path += '/';
            }
            path += section;
        }
        return path;
    }

    bool FrameDebugger::SubmitDrawEvent(
        const FrameDebugEventKind kind,
        const FrameDebugPass pass,
        const std::uint64_t objectId,
        const std::string_view objectName,
        const std::string_view componentType,
        FrameDebugDrawDescription description) noexcept
    {
        if (!m_enabled)
        {
            return true;
        }
        const auto index = m_nextIndex++;
        const bool draw = !m_limit || index <= *m_limit;
        try
        {
            FrameDebugEvent event;
            event.index = index;
            event.kind = kind;
            event.pass = pass;
            event.sectionPath = CurrentSectionPath();
            event.objectId = objectId;
            event.objectName = std::string(objectName);
            event.componentType = std::string(componentType);
            event.description = std::move(description);
            event.skipped = !draw;
            m_currentFrame.push_back(std::move(event));
        }
        catch (...)
        {
            // 記録できなくてもゲームの描画は続けます。番号がずれないよう、
            // 以降のイベントも上限の判定だけは同じ規則で行います。
        }
        return draw;
    }

    void FrameDebugger::EndFrame() noexcept
    {
        if (!m_enabled)
        {
            return;
        }
        m_lastFrame.swap(m_currentFrame);
        m_currentFrame.clear();
        m_nextIndex = 0;
        m_sections.clear();
        m_droppedSections = 0;
        ++m_completedFrames;
    }

    void FrameDebugger::OnGpuSectionBegin(
        const std::string_view name) noexcept
    {
        if (!m_enabled)
        {
            return;
        }
        try
        {
            m_sections.emplace_back(name);
        }
        catch (...)
        {
            ++m_droppedSections;
        }
    }

    void FrameDebugger::OnGpuSectionEnd() noexcept
    {
        if (m_droppedSections > 0)
        {
            --m_droppedSections;
            return;
        }
        // 有効化の前に始まった区間の終了は、対応する開始が無いので
        // 無視します。
        if (!m_sections.empty())
        {
            m_sections.pop_back();
        }
    }
}
