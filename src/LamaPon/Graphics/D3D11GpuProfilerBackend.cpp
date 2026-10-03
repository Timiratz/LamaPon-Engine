#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/D3D11Backend.h"
#include "LamaPon/Graphics/GpuProfiler.h"

#include <d3d11_1.h>
#include <wrl/client.h>

#include <array>
#include <memory>
#include <utility>

namespace LamaPon
{
    namespace
    {
        // D3D11の計測クエリを所有し、共通プロファイラーへ結果を返す。
        class D3D11GpuProfilerBackend final
            : public GpuProfilerBackend
        {
        public:
            // デバイスを保持して計測クエリを準備する(device: 任意の描画デバイス, context: 任意の描画コンテキスト)。
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
                // 時間計測の可否にかかわらず、対応する環境では描画マーカーを取得する。
                static_cast<void>(context->QueryInterface(
                    IID_PPV_ARGS(m_annotation.GetAddressOf())));

                // 時間クエリの事前生成に一つでも失敗した場合は、時間計測を無効にする。
                // 時刻有効性クエリの仕様
                D3D11_QUERY_DESC disjointDescription{};
                disjointDescription.Query =
                    D3D11_QUERY_TIMESTAMP_DISJOINT;
                // 描画統計クエリの仕様
                D3D11_QUERY_DESC pipelineDescription{};
                pipelineDescription.Query =
                    D3D11_QUERY_PIPELINE_STATISTICS;
                // 操作するフレームのスロット
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
                    // パイプライン統計の生成失敗は、時間計測の可否に影響させない。
                    static_cast<void>(m_device->CreateQuery(
                        &pipelineDescription,
                        frame.pipelineStatistics
                            .ReleaseAndGetAddressOf()));
                }
                m_supported = true;
            }

            // GPU時間計測が利用可能か返す。
            [[nodiscard]] bool
                IsSupported() const noexcept override
            {
                return m_supported;
            }

            // 再利用可能なスロットでフレーム計測を開始する。
            void OpenFrame() override
            {
                if (!m_supported)
                {
                    return;
                }
                // 操作するフレームのスロット
                auto& frame = m_frames[m_writeIndex];
                if (frame.open)
                {
                    return;
                }
                if (frame.pending)
                {
                    // 未完了クエリの再利用を避け、GPUが追いつくまでこのフレームの計測を休止する。
                    PollPendingFrames();
                    if (frame.pending)
                    {
                        return;
                    }
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

            // 計測区間を開始し、成功を返す(name: 区間の表示名, depth: 入れ子の深さ)。
            [[nodiscard]] bool BeginSection(
                const std::string_view name,
                const std::uint32_t depth) override
            {
                if (!m_supported)
                {
                    return false;
                }
                // 操作するフレームのスロット
                auto& frame = m_frames[m_writeIndex];
                if (!frame.open)
                {
                    return false;
                }
                if (frame.usedSections >= frame.sections.size())
                {
                    // 操作する区間のクエリ
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
                // 操作する区間のクエリ
                auto& section =
                    frame.sections[frame.usedSections];
                section.name = name;
                section.depth = depth;
                // 記録領域の確保を時刻送信より先に行い、失敗時に未完了区間を残さない。
                m_sectionStack.push_back(frame.usedSections);
                m_context->End(section.begin.Get());
                ++frame.usedSections;
                return true;
            }

            // 最後に開始した計測区間を終了する。
            void EndSection() noexcept override
            {
                if (m_sectionStack.empty())
                {
                    return;
                }
                // 操作するフレームのスロット
                auto& frame = m_frames[m_writeIndex];
                // フレームまたは区間の番号
                const auto index = m_sectionStack.back();
                m_sectionStack.pop_back();
                if (m_supported && frame.open)
                {
                    m_context->End(
                        frame.sections[index].end.Get());
                }
            }

            // 未完了区間を閉じてフレーム計測を終了する。
            void CloseFrame() override
            {
                if (!m_supported)
                {
                    m_sectionStack.clear();
                    return;
                }
                // 操作するフレームのスロット
                auto& frame = m_frames[m_writeIndex];
                if (!frame.open)
                {
                    return;
                }
                // 未完了の計測区間をすべて閉じてからフレームを終了する。
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

            // 次回更新まで有効な直近の区間一覧を借用する。
            [[nodiscard]] const std::vector<GpuSectionTime>&
                LatestSections() const noexcept override
            {
                return m_latestSections;
            }

            // 直近のフレーム時間をミリ秒で返す。
            [[nodiscard]] float
                LatestFrameMilliseconds() const noexcept override
            {
                return m_latestFrameMilliseconds;
            }

            // 次回更新まで有効な直近の描画統計を借用する。
            [[nodiscard]] const GpuPipelineStatistics&
                LatestPipelineStatistics() const noexcept override
            {
                return m_latestPipelineStatistics;
            }

            // 描画マーカーを開始し、成功を返す(name: マーカーの表示名)。
            [[nodiscard]] bool BeginMarker(
                const std::string_view name) noexcept override
            {
                // キャプチャー未接続時はマーカー名の変換を省く。
                if (m_annotation == nullptr || !m_annotation->GetStatus())
                {
                    return false;
                }
                try
                {
                    // UTF-16のマーカー名
                    const auto wideName = Utf8ToWide(name);
                    m_annotation->BeginEvent(wideName.c_str());
                    return true;
                }
                catch (...)
                {
                    return false;
                }
            }

            // 最後に開始した描画マーカーを終了する。
            void EndMarker() noexcept override
            {
                if (m_annotation != nullptr)
                {
                    m_annotation->EndEvent();
                }
            }

        private:
            struct SectionQueries final
            {
                // 区間の表示名
                std::string name;
                // 区間開始の時刻クエリ
                // 区間開始の時刻値
                Microsoft::WRL::ComPtr<ID3D11Query> begin;
                // 区間終了の時刻クエリ
                // 区間終了の時刻値
                Microsoft::WRL::ComPtr<ID3D11Query> end;
                // 区間の入れ子の深さ
                std::uint32_t depth{};
            };

            struct FrameQueries final
            {
                // 時刻周波数の有効性クエリ
                Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
                // フレーム開始の時刻クエリ
                // フレーム開始の時刻値
                Microsoft::WRL::ComPtr<ID3D11Query> frameBegin;
                // フレーム終了の時刻クエリ
                // フレーム終了の時刻値
                Microsoft::WRL::ComPtr<ID3D11Query> frameEnd;
                // 描画統計のクエリ
                Microsoft::WRL::ComPtr<ID3D11Query>
                    pipelineStatistics;
                // 再利用する区間クエリ
                // 取得した区間の時間一覧
                std::vector<SectionQueries> sections;
                // このフレームの使用区間数
                std::size_t usedSections{};
                // フレーム計測中
                bool open{};
                // GPUの結果待ち
                bool pending{};
            };

            // 時刻クエリを生成し、生成失敗なら空を返す。
            [[nodiscard]] Microsoft::WRL::ComPtr<ID3D11Query>
                CreateTimestampQuery() const
            {
                // 時刻クエリの仕様
                D3D11_QUERY_DESC description{};
                description.Query = D3D11_QUERY_TIMESTAMP;
                // 生成する時刻クエリ
                Microsoft::WRL::ComPtr<ID3D11Query> query;
                if (FAILED(m_device->CreateQuery(
                        &description,
                        query.ReleaseAndGetAddressOf())))
                {
                    return nullptr;
                }
                return query;
            }

            // GPUを待たず全クエリの完了を確認し、有効な結果を採用する。
            void PollPendingFrames()
            {
                // 完了したフレームの結果を確認順に採用する。
                // スロット確認の移動数
                for (std::size_t offset = 1;
                    offset <= FrameCount;
                    ++offset)
                {
                    // フレームまたは区間の番号
                    const auto index =
                        (m_writeIndex + offset) % FrameCount;
                    // 操作するフレームのスロット
                    auto& frame = m_frames[index];
                    if (!frame.pending)
                    {
                        continue;
                    }
                    // 読み出した時刻の有効性
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
                    // 時刻をミリ秒に換算する係数
                    const double toMilliseconds =
                        disjointData.Frequency != 0
                        ? 1000.0 / static_cast<double>(
                            disjointData.Frequency)
                        : 0.0;

                    // 時刻と区間と統計の全クエリが読めるまで、フレームのスロットを再利用しない。
                    // GPUを待たず結果を読む(query: 読むクエリ, value: 結果の格納先, size: 格納先のバイト数)。
                    const auto tryRead =
                        [this](ID3D11Query* const query,
                            void* const value,
                            const UINT size)
                    {
                        return m_context->GetData(
                                query,
                                value,
                                size,
                                D3D11_ASYNC_GETDATA_DONOTFLUSH)
                            == S_OK;
                    };

                    // フレーム開始の時刻値
                    std::uint64_t frameBegin{};
                    // フレーム終了の時刻値
                    std::uint64_t frameEnd{};
                    if (!tryRead(
                            frame.frameBegin.Get(),
                            &frameBegin,
                            sizeof(frameBegin))
                        || !tryRead(
                            frame.frameEnd.Get(),
                            &frameEnd,
                            sizeof(frameEnd)))
                    {
                        continue;
                    }

                    // 読み出した描画統計
                    D3D11_QUERY_DATA_PIPELINE_STATISTICS statistics{};
                    // 描画統計の取得済み
                    bool hasPipelineStatistics = false;
                    if (frame.pipelineStatistics)
                    {
                        if (!tryRead(
                                frame.pipelineStatistics.Get(),
                                &statistics,
                                sizeof(statistics)))
                        {
                            continue;
                        }
                        hasPipelineStatistics = true;
                    }

                    // 取得した区間の時間一覧
                    std::vector<GpuSectionTime> sections;
                    sections.reserve(frame.usedSections);
                    // 読み出す区間番号
                    for (std::size_t sectionIndex = 0;
                        sectionIndex < frame.usedSections;
                        ++sectionIndex)
                    {
                        // 操作する区間のクエリ
                        const auto& section =
                            frame.sections[sectionIndex];
                        // 区間開始の時刻値
                        std::uint64_t begin{};
                        // 区間終了の時刻値
                        std::uint64_t end{};
                        if (!tryRead(
                                section.begin.Get(),
                                &begin,
                                sizeof(begin))
                            || !tryRead(
                                section.end.Get(),
                                &end,
                                sizeof(end)))
                        {
                            // 一部の区間が未完了なら、先に読めたクエリも含めてスロットを保持する。
                            break;
                        }
                        sections.push_back({
                            section.name,
                            static_cast<float>(
                                static_cast<double>(end - begin)
                                * toMilliseconds),
                            section.depth });
                    }

                    if (sections.size() != frame.usedSections)
                    {
                        continue;
                    }

                    // 全クエリを読めた時点でスロットを再利用可能にし、周波数が不安定な結果は捨てる。
                    frame.pending = false;
                    if (disjointData.Disjoint
                        || disjointData.Frequency == 0)
                    {
                        continue;
                    }

                    m_latestFrameMilliseconds =
                        static_cast<float>(
                            static_cast<double>(
                                frameEnd - frameBegin)
                            * toMilliseconds);

                    if (hasPipelineStatistics)
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

                    m_latestSections = std::move(sections);
                }
            }

            // クエリをデバイスとコンテキストより先に解放できるよう、所有元を先に宣言する。
            // 保持する描画デバイス
            Microsoft::WRL::ComPtr<ID3D11Device> m_device;
            // 保持する描画コンテキスト
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;

            // 保持する描画マーカーAPI
            Microsoft::WRL::ComPtr<ID3DUserDefinedAnnotation> m_annotation;

            // 遅延読取用のスロット数
            static constexpr std::size_t FrameCount = 4;
            // 遅延読取のフレームスロット
            std::array<FrameQueries, FrameCount> m_frames;
            // 次に計測するスロット番号
            std::size_t m_writeIndex{};
            // 未完了区間の番号スタック
            std::vector<std::size_t> m_sectionStack;
            // GPU時間計測が利用可能
            bool m_supported{};
            // 直近に取得した区間一覧
            std::vector<GpuSectionTime> m_latestSections;
            // 直近のフレーム時間、ミリ秒
            float m_latestFrameMilliseconds{};
            // 直近に取得した描画統計
            GpuPipelineStatistics m_latestPipelineStatistics;
        };
    }

    // D3D11のGPUプロファイラーを生成する(device: 任意の描画デバイス, context: 任意の描画コンテキスト)。
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
