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
        , m_initialDepth(profiler.m_sectionDepth)
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

    void GpuProfiler::BeginSection(
        const std::string_view name)
    {
        if (!IsSupported())
        {
            return;
        }
        m_backend->OpenFrame();
        if (m_backend->BeginSection(
                name,
                static_cast<std::uint32_t>(m_sectionDepth)))
        {
            ++m_sectionDepth;
        }
    }

    void GpuProfiler::EndSection()
    {
        if (m_sectionDepth == 0)
        {
            return;
        }
        if (m_backend != nullptr)
        {
            m_backend->EndSection();
        }
        --m_sectionDepth;
    }

    void GpuProfiler::EndSectionsToDepth(
        const std::size_t depth) noexcept
    {
        while (m_sectionDepth > depth)
        {
            if (m_backend != nullptr)
            {
                m_backend->EndSection();
            }
            --m_sectionDepth;
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
