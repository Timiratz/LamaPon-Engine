#include "LamaPon/Graphics/GpuProfiler.h"

namespace LamaPon
{
    void GpuProfiler::Initialize(
        ID3D11Device* const device,
        ID3D11DeviceContext* const context)
    {
        m_latestPipelineStatistics = {};
        m_device = device;
        m_context = context;
        m_supported = false;
        if (device == nullptr || context == nullptr)
        {
            return;
        }

        // 全フレームスロットのクエリを事前に作成します。
        // 1つでも失敗したら計測を無効にします。
        D3D11_QUERY_DESC disjointDescription{};
        disjointDescription.Query =
            D3D11_QUERY_TIMESTAMP_DISJOINT;
        D3D11_QUERY_DESC pipelineDescription{};
        pipelineDescription.Query =
            D3D11_QUERY_PIPELINE_STATISTICS;
        for (auto& frame : m_frames)
        {
            if (FAILED(m_device->CreateQuery(
                    &disjointDescription,
                    frame.disjoint
                        .ReleaseAndGetAddressOf())))
            {
                return;
            }
            frame.frameBegin = CreateTimestampQuery();
            frame.frameEnd = CreateTimestampQuery();
            if (!frame.frameBegin || !frame.frameEnd)
            {
                return;
            }
            // 一部の古い／仮想ドライバーでは非対応です。失敗しても
            // GPU時間計測まで無効にせず、統計だけ省略します。
            static_cast<void>(m_device->CreateQuery(
                &pipelineDescription,
                frame.pipelineStatistics
                    .ReleaseAndGetAddressOf()));
        }
        m_supported = true;
    }

    Microsoft::WRL::ComPtr<ID3D11Query>
        GpuProfiler::CreateTimestampQuery() const
    {
        D3D11_QUERY_DESC description{};
        description.Query = D3D11_QUERY_TIMESTAMP;
        Microsoft::WRL::ComPtr<ID3D11Query> query;
        if (FAILED(m_device->CreateQuery(
                &description,
                query.ReleaseAndGetAddressOf())))
        {
            return nullptr;
        }
        return query;
    }

    void GpuProfiler::OpenFrame()
    {
        if (!m_supported)
        {
            return;
        }
        auto& frame = m_frames[m_writeIndex];
        if (frame.open)
        {
            return;
        }
        if (frame.pending)
        {
            // リングを一周してもまだ結果が取れていない
            // 古いフレームは破棄します。
            frame.pending = false;
        }
        frame.open = true;
        frame.usedSections = 0;
        m_sectionStack.clear();
        m_context->Begin(frame.disjoint.Get());
        if (frame.pipelineStatistics)
        {
            m_context->Begin(frame.pipelineStatistics.Get());
        }
        m_context->End(frame.frameBegin.Get());
    }

    void GpuProfiler::BeginSection(
        const std::string_view name)
    {
        if (!m_supported)
        {
            return;
        }
        OpenFrame();
        auto& frame = m_frames[m_writeIndex];
        if (frame.usedSections >= frame.sections.size())
        {
            SectionQueries section;
            section.begin = CreateTimestampQuery();
            section.end = CreateTimestampQuery();
            if (!section.begin || !section.end)
            {
                m_supported = false;
                return;
            }
            frame.sections.push_back(
                std::move(section));
        }
        auto& section =
            frame.sections[frame.usedSections];
        section.name = name;
        // 積む前のスタックの高さが、そのまま入れ子の深さです。
        section.depth = static_cast<std::uint32_t>(
            m_sectionStack.size());
        m_context->End(section.begin.Get());
        m_sectionStack.push_back(frame.usedSections);
        ++frame.usedSections;
    }

    void GpuProfiler::EndSection()
    {
        if (!m_supported || m_sectionStack.empty())
        {
            return;
        }
        auto& frame = m_frames[m_writeIndex];
        const auto index = m_sectionStack.back();
        m_sectionStack.pop_back();
        m_context->End(
            frame.sections[index].end.Get());
    }

    void GpuProfiler::CloseFrame()
    {
        if (!m_supported)
        {
            return;
        }
        auto& frame = m_frames[m_writeIndex];
        if (!frame.open)
        {
            return;
        }
        // 閉じ忘れた区間があればここで閉じます。
        while (!m_sectionStack.empty())
        {
            EndSection();
        }
        m_context->End(frame.frameEnd.Get());
        if (frame.pipelineStatistics)
        {
            m_context->End(frame.pipelineStatistics.Get());
        }
        m_context->End(frame.disjoint.Get());
        frame.open = false;
        frame.pending = true;
        m_writeIndex =
            (m_writeIndex + 1) % FrameCount;
        PollPendingFrames();
    }

    void GpuProfiler::PollPendingFrames()
    {
        // 古い順に確認し、準備できた最新のフレームを採用します。
        for (std::size_t offset = 1;
            offset <= FrameCount;
            ++offset)
        {
            const auto index =
                (m_writeIndex + offset) % FrameCount;
            auto& frame = m_frames[index];
            if (!frame.pending)
            {
                continue;
            }
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT
                disjointData{};
            if (m_context->GetData(
                    frame.disjoint.Get(),
                    &disjointData,
                    sizeof(disjointData),
                    D3D11_ASYNC_GETDATA_DONOTFLUSH)
                != S_OK)
            {
                continue;
            }
            frame.pending = false;
            if (disjointData.Disjoint
                || disjointData.Frequency == 0)
            {
                continue;
            }

            const auto readTimestamp =
                [this](ID3D11Query* query,
                    std::uint64_t& value)
            {
                return m_context->GetData(
                        query,
                        &value,
                        sizeof(value),
                        0)
                    == S_OK;
            };
            const double toMilliseconds =
                1000.0
                / static_cast<double>(
                    disjointData.Frequency);

            std::uint64_t frameBegin{};
            std::uint64_t frameEnd{};
            if (!readTimestamp(
                    frame.frameBegin.Get(),
                    frameBegin)
                || !readTimestamp(
                    frame.frameEnd.Get(),
                    frameEnd))
            {
                continue;
            }
            m_latestFrameMilliseconds =
                static_cast<float>(
                    static_cast<double>(
                        frameEnd - frameBegin)
                    * toMilliseconds);

            if (frame.pipelineStatistics)
            {
                D3D11_QUERY_DATA_PIPELINE_STATISTICS statistics{};
                if (m_context->GetData(
                        frame.pipelineStatistics.Get(),
                        &statistics,
                        sizeof(statistics),
                        0) == S_OK)
                {
                    m_latestPipelineStatistics = {
                        statistics.IAVertices,
                        statistics.IAPrimitives,
                        statistics.VSInvocations,
                        statistics.PSInvocations,
                        statistics.HSInvocations,
                        statistics.DSInvocations,
                        statistics.GSInvocations,
                        statistics.CSInvocations,
                        true
                    };
                }
            }

            m_latestSections.clear();
            for (std::size_t sectionIndex = 0;
                sectionIndex < frame.usedSections;
                ++sectionIndex)
            {
                const auto& section =
                    frame.sections[sectionIndex];
                std::uint64_t begin{};
                std::uint64_t end{};
                if (!readTimestamp(
                        section.begin.Get(),
                        begin)
                    || !readTimestamp(
                        section.end.Get(),
                        end))
                {
                    continue;
                }
                m_latestSections.push_back({
                    section.name,
                    static_cast<float>(
                        static_cast<double>(end - begin)
                        * toMilliseconds),
                    section.depth });
            }
        }
    }
}
