#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    // CPU側のオブジェクト数ではなく、LOD・カリング・
    // インスタンシングを通過してGPUへ届いた実仕事量です。
    struct GpuPipelineStatistics final
    {
        std::uint64_t inputAssemblerVertices{};
        std::uint64_t inputAssemblerPrimitives{};
        std::uint64_t vertexShaderInvocations{};
        std::uint64_t pixelShaderInvocations{};
        std::uint64_t hullShaderInvocations{};
        std::uint64_t domainShaderInvocations{};
        std::uint64_t geometryShaderInvocations{};
        std::uint64_t computeShaderInvocations{};
        bool valid{};
    };

    struct GpuSectionTime final
    {
        std::string name;
        float milliseconds{};
        // 入れ子の深さ（0が最上位）。区間は入れ子にできるので、
        // 一覧をそのまま足すと内側が二重に数えられます。
        // GPU全体との比較は depth==0 だけを足してください。
        std::uint32_t depth{};
    };

    // API固有のtimestamp/query実装です。GraphicsBackendが所有し、
    // GpuProfilerが非所有ポインターで参照します。呼び出し側に
    // Device / Context等のAPI固有型は公開しません。
    class GpuProfilerBackend
    {
    public:
        virtual ~GpuProfilerBackend() = default;

        GpuProfilerBackend() = default;
        GpuProfilerBackend(const GpuProfilerBackend&) = delete;
        GpuProfilerBackend& operator=(
            const GpuProfilerBackend&) = delete;

        [[nodiscard]] virtual bool
            IsSupported() const noexcept = 0;
        virtual void OpenFrame() = 0;
        // 計測を開始できたときだけtrue。depthはfacadeが
        // 管理する論理的な入れ子深度です。
        [[nodiscard]] virtual bool BeginSection(
            std::string_view name,
            std::uint32_t depth) = 0;
        virtual void EndSection() noexcept = 0;
        virtual void CloseFrame() = 0;

        [[nodiscard]] virtual const std::vector<GpuSectionTime>&
            LatestSections() const noexcept = 0;
        [[nodiscard]] virtual float
            LatestFrameMilliseconds() const noexcept = 0;
        [[nodiscard]] virtual const GpuPipelineStatistics&
            LatestPipelineStatistics() const noexcept = 0;
    };

    // GPU計測のAPI非依存facadeです。表示や描画パイプラインは
    // この型だけを使い、timestamp/queryの詳細は選択中の
    // GraphicsBackendが差し込みます。未接続・非対応時は安全な
    // no-opと空の結果へ倒れます。
    class GpuProfiler final
    {
    public:
        // 従来の公開型名はaliasで維持します。
        using PipelineStatistics = GpuPipelineStatistics;
        using SectionTime = GpuSectionTime;

        // 例外で内側の描画処理が中断されても、構築時より後に開始した
        // 区間をすべて閉じます。単純なEndSection 1回では、内側の
        // 区間だけを閉じて外側を残すため、深さをtokenとして使います。
        class SectionScope final
        {
        public:
            SectionScope(
                GpuProfiler& profiler,
                std::string_view name);
            ~SectionScope() noexcept;

            void End() noexcept;
            [[nodiscard]] std::size_t InitialDepth() const noexcept
            {
                return m_initialDepth;
            }

            SectionScope(const SectionScope&) = delete;
            SectionScope& operator=(const SectionScope&) = delete;

        private:
            GpuProfiler* m_profiler{};
            std::size_t m_initialDepth{};
            std::uint64_t m_generation{};
        };

        // backendの寿命はこのfacadeより長く保ち、backendを破棄する
        // 前にDetachします。nullptrは未接続状態として扱います。
        void Attach(GpuProfilerBackend* backend) noexcept;
        void Detach() noexcept;

        // フレームの計測を開始します（開始済みなら何もしません）。
        void OpenFrame();
        // 区間の計測を開始/終了します（入れ子可）。
        void BeginSection(std::string_view name);
        void EndSection();
        // フレームを閉じ、準備できた過去フレームの結果を
        // 取り込みます。
        void CloseFrame();

        [[nodiscard]] bool IsSupported() const noexcept;
        // 直近の確定フレームの区間一覧。
        [[nodiscard]] const std::vector<SectionTime>&
            LatestSections() const noexcept;
        // 直近の確定フレームのGPU全体時間（ミリ秒）。
        [[nodiscard]] float
            LatestFrameMilliseconds() const noexcept;
        [[nodiscard]] const PipelineStatistics&
            LatestPipelineStatistics() const noexcept;

    private:
        void EndSectionsToDepth(std::size_t depth) noexcept;

        GpuProfilerBackend* m_backend{};
        std::size_t m_sectionDepth{};
        // detach / reattachをまたぐ古いSectionScopeが、新しい
        // backendの区間を閉じないようにする世代番号です。
        std::uint64_t m_generation{};
    };
}
