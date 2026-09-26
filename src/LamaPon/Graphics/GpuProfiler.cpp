#include "LamaPon/Graphics/GpuProfiler.h"

namespace LamaPon
{
    namespace
    {
        [[nodiscard]] const std::vector<GpuSectionTime>&
            EmptySections() noexcept
        {
            static const std::vector<GpuSectionTime> sections;
            return sections;
        }

        [[nodiscard]] const GpuPipelineStatistics&
            EmptyPipelineStatistics() noexcept
        {
            static const GpuPipelineStatistics statistics;
            return statistics;
        }

    }

    GpuProfiler::SectionScope::SectionScope(
        GpuProfiler& profiler,
        const std::string_view name)
        : m_profiler(&profiler)
        , m_initialDepth(profiler.m_sections.size())
        , m_generation(profiler.m_generation)
    {
        profiler.BeginSection(name);
    }

    GpuProfiler::SectionScope::~SectionScope() noexcept
    {
        End();
    }

    void GpuProfiler::SectionScope::End() noexcept
    {
        if (m_profiler == nullptr)
        {
            return;
        }
        if (m_profiler->m_generation == m_generation)
        {
            m_profiler->EndSectionsToDepth(m_initialDepth);
        }
        m_profiler = nullptr;
    }

    void GpuProfiler::Attach(
        GpuProfilerBackend* const backend) noexcept
    {
        Detach();
        m_backend = backend;
    }

    void GpuProfiler::Detach() noexcept
    {
        EndSectionsToDepth(0);
        m_backend = nullptr;
        ++m_generation;
    }

    void GpuProfiler::OpenFrame()
    {
        if (IsSupported())
        {
            m_backend->OpenFrame();
        }
    }

    void GpuProfiler::SetSectionListener(
        GpuSectionListener* const listener) noexcept
    {
        m_listener = listener;
    }

    void GpuProfiler::BeginSection(
        const std::string_view name)
    {
        // 以降の副作用より前に確保し、登録できない区間の開始だけが
        // 各通知先へ残ることを防ぎます。
        m_sections.reserve(m_sections.size() + 1);

        OpenSection section;
        if (IsSupported())
        {
            m_backend->OpenFrame();
            section.timed = m_backend->BeginSection(
                name,
                static_cast<std::uint32_t>(m_timedDepth));
            if (section.timed)
            {
                ++m_timedDepth;
            }
        }
        // ここから先は例外を送出しないため、開始した通知先は
        // 必ずm_sectionsから終了されます。
        if (m_backend != nullptr)
        {
            section.marker = m_backend->BeginMarker(name);
        }
        if (m_listener != nullptr)
        {
            m_listener->OnGpuSectionBegin(name);
            section.listened = true;
        }
        try
        {
            section.cpuScope =
                Profiler::Instance().BeginScope(name);
            if (section.cpuScope.IsValid())
            {
                section.cpuStart = std::chrono::steady_clock::now();
            }
        }
        catch (...)
        {
            // CPU計測の失敗で描画パスを止めません。
            section.cpuScope = {};
        }
        m_sections.push_back(section);
    }

    void GpuProfiler::EndSection()
    {
        if (m_sections.empty())
        {
            return;
        }
        EndTopSection();
    }

    void GpuProfiler::EndTopSection() noexcept
    {
        const auto section = m_sections.back();
        m_sections.pop_back();
        if (section.cpuScope.IsValid())
        {
            Profiler::Instance().EndScope(
                section.cpuScope,
                std::chrono::steady_clock::now()
                    - section.cpuStart);
        }
        if (section.listened && m_listener != nullptr)
        {
            m_listener->OnGpuSectionEnd();
        }
        if (section.marker && m_backend != nullptr)
        {
            m_backend->EndMarker();
        }
        if (section.timed)
        {
            if (m_backend != nullptr)
            {
                m_backend->EndSection();
            }
            --m_timedDepth;
        }
    }

    void GpuProfiler::EndSectionsToDepth(
        const std::size_t depth) noexcept
    {
        while (m_sections.size() > depth)
        {
            EndTopSection();
        }
    }

    void GpuProfiler::CloseFrame()
    {
        EndSectionsToDepth(0);
        if (IsSupported())
        {
            m_backend->CloseFrame();
        }
    }

    bool GpuProfiler::IsSupported() const noexcept
    {
        return m_backend != nullptr
            && m_backend->IsSupported();
    }

    const std::vector<GpuProfiler::SectionTime>&
        GpuProfiler::LatestSections() const noexcept
    {
        return IsSupported()
            ? m_backend->LatestSections()
            : EmptySections();
    }

    float GpuProfiler::LatestFrameMilliseconds() const noexcept
    {
        return IsSupported()
            ? m_backend->LatestFrameMilliseconds()
            : 0.0f;
    }

    const GpuProfiler::PipelineStatistics&
        GpuProfiler::LatestPipelineStatistics() const noexcept
    {
        return IsSupported()
            ? m_backend->LatestPipelineStatistics()
            : EmptyPipelineStatistics();
    }

}
