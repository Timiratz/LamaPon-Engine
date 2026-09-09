#include "LamaPon/Graphics/GpuProfiler.h"

#include "LamaPon/Graphics/D3D11Backend.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <memory>
#include <utility>

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

        // D3D11 queryの実装は具象Backendが所有し、共通facadeから
        // Device / Contextを見えなくします。D3D12 Backendでは
        // timestamp heap / readback / fenceを用いる別driverを実装します。
        class D3D11GpuProfilerBackend final
            : public GpuProfilerBackend
        {
        public:
            D3D11GpuProfilerBackend(
                ID3D11Device* const device,
                ID3D11DeviceContext* const context)
                : m_device(device)
                , m_context(context)
            {
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

            [[nodiscard]] bool
                IsSupported() const noexcept override
            {
                return m_supported;
            }

            void OpenFrame() override
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

            [[nodiscard]] bool BeginSection(
                const std::string_view name,
                const std::uint32_t depth) override
            {
                if (!m_supported)
                {
                    return false;
                }
                auto& frame = m_frames[m_writeIndex];
                if (!frame.open)
                {
                    return false;
                }
                if (frame.usedSections >= frame.sections.size())
                {
                    SectionQueries section;
                    section.begin = CreateTimestampQuery();
                    section.end = CreateTimestampQuery();
                    if (!section.begin || !section.end)
                    {
                        m_supported = false;
                        m_sectionStack.clear();
                        return false;
                    }
                    frame.sections.push_back(
                        std::move(section));
                }
                auto& section =
                    frame.sections[frame.usedSections];
                section.name = name;
                section.depth = depth;
                // スタック拡張が失敗するときは、begin timestampを
                // 送る前に例外にして半端な区間を残しません。
                m_sectionStack.push_back(frame.usedSections);
                m_context->End(section.begin.Get());
                ++frame.usedSections;
                return true;
            }

            void EndSection() noexcept override
            {
                if (m_sectionStack.empty())
                {
                    return;
                }
                auto& frame = m_frames[m_writeIndex];
                const auto index = m_sectionStack.back();
                m_sectionStack.pop_back();
                if (m_supported && frame.open)
                {
                    m_context->End(
                        frame.sections[index].end.Get());
                }
            }

            void CloseFrame() override
            {
                if (!m_supported)
                {
                    m_sectionStack.clear();
                    return;
                }
                auto& frame = m_frames[m_writeIndex];
                if (!frame.open)
                {
                    return;
                }
                // facadeの閉じ忘れ防止に加え、driver単体でも
                // 未完了区間を必ず閉じます。
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

            [[nodiscard]] const std::vector<GpuSectionTime>&
                LatestSections() const noexcept override
            {
                return m_latestSections;
            }

            [[nodiscard]] float
                LatestFrameMilliseconds() const noexcept override
            {
                return m_latestFrameMilliseconds;
            }

            [[nodiscard]] const GpuPipelineStatistics&
                LatestPipelineStatistics() const noexcept override
            {
                return m_latestPipelineStatistics;
            }

        private:
            struct SectionQueries final
            {
                std::string name;
                Microsoft::WRL::ComPtr<ID3D11Query> begin;
                Microsoft::WRL::ComPtr<ID3D11Query> end;
                std::uint32_t depth{};
            };

            struct FrameQueries final
            {
                Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
                Microsoft::WRL::ComPtr<ID3D11Query> frameBegin;
                Microsoft::WRL::ComPtr<ID3D11Query> frameEnd;
                Microsoft::WRL::ComPtr<ID3D11Query>
                    pipelineStatistics;
                std::vector<SectionQueries> sections;
                // 今フレームで使った区間数（クエリはプール再利用）。
                std::size_t usedSections{};
                bool open{};
                bool pending{};
            };

            [[nodiscard]] Microsoft::WRL::ComPtr<ID3D11Query>
                CreateTimestampQuery() const
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

            void PollPendingFrames()
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

            // Queryとdriverの所有期間はBackend内で一致しますが、
            // COM参照も保持してraw pointerを残しません。宣言順により、
            // 逆順破棄時はQueryがDevice / Contextより先に解放されます。
            Microsoft::WRL::ComPtr<ID3D11Device> m_device;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
            // 読み出し遅延用のリングバッファ。
            static constexpr std::size_t FrameCount = 4;
            std::array<FrameQueries, FrameCount> m_frames;
            std::size_t m_writeIndex{};
            std::vector<std::size_t> m_sectionStack;
            bool m_supported{};
            std::vector<GpuSectionTime> m_latestSections;
            float m_latestFrameMilliseconds{};
            GpuPipelineStatistics m_latestPipelineStatistics;
        };
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

    std::unique_ptr<GpuProfilerBackend>
        D3D11Backend::CreateProfilerBackend(
            ID3D11Device* const device,
            ID3D11DeviceContext* const context)
    {
        return std::make_unique<D3D11GpuProfilerBackend>(
            device,
            context);
    }
}
