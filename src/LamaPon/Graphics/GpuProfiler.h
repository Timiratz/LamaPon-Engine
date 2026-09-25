#pragma once

#include "LamaPon/Core/Profiler.h"

#include <chrono>
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

        // PIXやRenderDocのイベント一覧へ区間名を出すdebug markerです。
        // timestampとは独立しており、計測に非対応の環境でも呼ばれます。
        // 記録できたときだけtrueを返し、trueを返したmarkerにだけ
        // EndMarkerが対応します。既定は何もしません。
        [[nodiscard]] virtual bool BeginMarker(
            std::string_view name) noexcept
        {
            static_cast<void>(name);
            return false;
        }
        virtual void EndMarker() noexcept {}
    };

    // GPU区間の開始・終了を、描画を行う側から観測するための通知先です。
    // フレームデバッガーが描画イベントをパスごとに分類するために使います。
    // 通知は区間と同じ順で入れ子になり、例外を送出してはいけません。
    class GpuSectionListener
    {
    public:
        virtual ~GpuSectionListener() = default;

        GpuSectionListener() = default;
        GpuSectionListener(const GpuSectionListener&) = delete;
        GpuSectionListener& operator=(
            const GpuSectionListener&) = delete;

        virtual void OnGpuSectionBegin(
            std::string_view name) noexcept = 0;
        virtual void OnGpuSectionEnd() noexcept = 0;
    };

    // GPU計測のAPI非依存facadeです。表示や描画パイプラインは
    // この型だけを使い、timestamp/queryの詳細は選択中の
    // GraphicsBackendが差し込みます。未接続・非対応時は安全な
    // no-opと空の結果へ倒れます。
    //
    // 区間は描画パスの名前として、GPU時間のほかに次へも流します。
    // - CPUプロファイラー（同名の入れ子区間。発行側のCPU時間）
    // - backendのdebug marker（PIX / RenderDoc）
    // - GpuSectionListener（フレームデバッガーのパス分類）
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
        // nullptrで解除します。listenerはこのfacadeより長く生存させるか、
        // 破棄前に解除してください。
        void SetSectionListener(
            GpuSectionListener* listener) noexcept;
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
        // 開いている区間ごとに、どの通知先へ開始を伝えたかを記録します。
        // 終了時は開始できたものだけを閉じ、各通知先の入れ子を保ちます。
        struct OpenSection final
        {
            ProfileScopeToken cpuScope;
            std::chrono::steady_clock::time_point cpuStart;
            bool timed{};
            bool marker{};
            bool listened{};
        };

        void EndSectionsToDepth(std::size_t depth) noexcept;
        void EndTopSection() noexcept;

        GpuProfilerBackend* m_backend{};
        GpuSectionListener* m_listener{};
        std::vector<OpenSection> m_sections;
        // timestampを発行できた区間の入れ子深度です。backendへ渡す
        // depthはこちらで、SectionScopeの復帰位置はm_sectionsの長さです。
        std::size_t m_timedDepth{};
        // detach / reattachをまたぐ古いSectionScopeが、新しい
        // backendの区間を閉じないようにする世代番号です。
        std::uint64_t m_generation{};
    };
}
