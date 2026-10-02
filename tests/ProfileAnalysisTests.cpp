#include "LamaPon/Core/ProfileAnalysis.h"
#include "LamaPon/Core/Profiler.h"
#include "LamaPon/Graphics/GpuProfiler.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
    // Require(condition: 条件, message: 失敗時の説明)でテスト失敗を通知します。
    void Require(const bool condition, const char* message)
    {
        // テスト条件の成否を判定します。
        if (!condition)
        {
            // 条件違反をテスト失敗にします。
            throw std::runtime_error(message);
        }
    }

    // Near(left: 値A, right: 値B)で数値の誤差を比較します。
    bool Near(const double left, const double right)
    {
        // 許容誤差内かどうかを返します。
        return std::abs(left - right) < 1.0e-6;
    }

    // FindSample(frame: 対象フレーム, name: 区間名)で計測サンプルを探します。
    const LamaPon::ProfileSample* FindSample(
        const LamaPon::ProfileFrame& frame,
        const std::string& name)
    {
        // 計測サンプル
        for (const auto& sample : frame.samples)
        {
            // 区間名が一致するか調べます。
            if (sample.name == name)
            {
                // 見つかった区間を返します。
                return &sample;
            }
        }
        // 一致する区間がないことを返します。
        return nullptr;
    }

    // MakeSample(name: 区間名, milliseconds: 計測時間, parent: 親番号, depth: 階層)を作成します。
    LamaPon::ProfileSample MakeSample(
        std::string name,
        const double milliseconds,
        const std::uint32_t parent = LamaPon::ProfileSample::NoParent,
        const std::uint32_t depth = 0)
    {
        // 計測サンプル
        LamaPon::ProfileSample sample;
        sample.name = std::move(name);
        sample.milliseconds = milliseconds;
        sample.callCount = 1;
        sample.parent = parent;
        sample.depth = depth;
        // 作成したサンプルを返します。
        return sample;
    }

    // MakeFrame(index: フレーム番号, render: 描画ms, shadow: 影描画ms, simulation: 更新ms)を作成します。
    LamaPon::ProfileFrame MakeFrame(
        const std::uint64_t index,
        const double render,
        const double shadow,
        const double simulation)
    {
        // 計測フレーム
        LamaPon::ProfileFrame frame;
        frame.index = index;
        frame.milliseconds = render + simulation + 1.0;
        frame.samples.push_back(MakeSample("Render", render));
        frame.samples.push_back(MakeSample("Shadow", shadow, 0, 1));
        frame.samples.push_back(
            MakeSample("Main", render - shadow - 0.5, 0, 1));
        frame.samples.push_back(MakeSample("Simulation", simulation));
        // 組み立てた計測フレームを返します。
        return frame;
    }

    // 親子関係を持つscope集計を検証します。
    void TestHierarchicalScopes()
    {
        // CPUプロファイラー
        auto& profiler = LamaPon::Profiler::Instance();
        profiler.Clear();
        profiler.SetEnabled(true);
        profiler.SetFrameCapacity(8);

        profiler.BeginFrame();
        {
            LAMAPON_PROFILE_SCOPE("Render");
            {
                LAMAPON_PROFILE_SCOPE("Shadow");
            }
            {
                LAMAPON_PROFILE_SCOPE("Shadow");

            }
            // 開いている区間の子として加算されます。
            profiler.Record("Upload", std::chrono::milliseconds(2));
        }
        {
            LAMAPON_PROFILE_SCOPE("Simulation");
            // 別の親の下の同名区間は別サンプルです。
            LAMAPON_PROFILE_SCOPE("Shadow");
        }
        // 独立した最上位scope
        std::thread worker(
            []
            {
                LAMAPON_PROFILE_SCOPE("Worker");
            });
        worker.join();
        profiler.EndFrame();

        // 記録済みフレーム
        const auto frames = profiler.Snapshot();
        Require(frames.size() == 1, "The hierarchical frame was not kept.");
        // 最新フレーム
        const auto& frame = frames.front();
        Require(
            frame.samples.size() == 6,
            "Scopes were not aggregated per parent.");

        // Render区間
        const auto& render = frame.samples[0];
        Require(
            render.name == "Render"
                && render.parent == LamaPon::ProfileSample::NoParent
                && render.depth == 0
                && render.callCount == 1,
            "The top-level scope is wrong.");
        // Shadow区間
        const auto& shadow = frame.samples[1];
        Require(
            shadow.name == "Shadow"
                && shadow.parent == 0
                && shadow.depth == 1
                && shadow.callCount == 2,
            "Repeated child scopes were not aggregated under the parent.");
        // Upload区間
        const auto& upload = frame.samples[2];
        Require(
            upload.name == "Upload"
                && upload.parent == 0
                && upload.depth == 1
                && upload.milliseconds >= 2.0,
            "Record did not attach to the open scope.");
        Require(
            render.milliseconds >= 0.0
                && frame.samples[3].name == "Simulation"
                && frame.samples[3].parent
                    == LamaPon::ProfileSample::NoParent,
            "The second top-level scope is wrong.");
        Require(
            frame.samples[4].name == "Shadow"
                && frame.samples[4].parent == 3
                && frame.samples[4].depth == 1,
            "A same-named scope under another parent was merged.");
        Require(
            frame.samples[5].name == "Worker"
                && frame.samples[5].parent
                    == LamaPon::ProfileSample::NoParent,
            "A worker thread scope inherited the main thread parent.");
        // フレーム内の親番号を検証します(index: サンプル番号)。
        for (std::size_t index{}; index < frame.samples.size(); ++index)
        {
            // 親区間番号
            const auto parent = frame.samples[index].parent;
            Require(
                parent == LamaPon::ProfileSample::NoParent
                    || parent < index,
                "A child was stored before its parent.");
        }
    }

    // フレームをまたぐscope漏れを検証します。
    void TestScopesDoNotCrossFrames()
    {
        // CPUプロファイラー
        auto& profiler = LamaPon::Profiler::Instance();
        profiler.Clear();
        profiler.BeginFrame();
        // 開いた区間
        auto* scope = new LamaPon::ProfileScope("Open");
        profiler.EndFrame();
        profiler.BeginFrame();
        // 前フレームで開いた区間を閉じても、新しいフレームの添字を書き換えません。
        {
            LAMAPON_PROFILE_SCOPE("Fresh");
        }
        delete scope;
        {
            LAMAPON_PROFILE_SCOPE("AfterStale");
        }
        profiler.EndFrame();

        // 記録済みフレーム
        const auto frames = profiler.Snapshot();
        Require(frames.size() == 2, "Frames were not recorded.");
        // 2つ目のフレーム
        const auto& second = frames.back();
        Require(
            FindSample(second, "Open") == nullptr
                && FindSample(second, "Fresh") != nullptr
                && FindSample(second, "Fresh")->callCount == 1,
            "A scope leaked across frames.");
        // 古いscope後の区間
        const auto* afterStale = FindSample(second, "AfterStale");
        Require(
            afterStale != nullptr
                && afterStale->parent
                    == LamaPon::ProfileSample::NoParent,
            "A stale scope stayed on the thread stack.");
    }

    // 終了順が崩れたscopeの親を検証します。
    void TestOutOfOrderScopeEnd()
    {
        // CPUプロファイラー
        auto& profiler = LamaPon::Profiler::Instance();
        profiler.Clear();
        profiler.BeginFrame();
        // 外側区間トークン
        const auto outer = profiler.BeginScope("Outer");
        // 内側区間トークン
        const auto inner = profiler.BeginScope("Inner");
        // GPU区間のEnd()のように、外側が内側より先に閉じられる場合です。
        profiler.EndScope(outer, std::chrono::milliseconds(1));
        // 外側終了後の区間
        const auto sibling = profiler.BeginScope("AfterOuter");
        profiler.EndScope(sibling, std::chrono::milliseconds(1));
        profiler.EndScope(inner, std::chrono::milliseconds(1));
        // 新しい最上位区間
        const auto root = profiler.BeginScope("Root");
        profiler.EndScope(root, std::chrono::milliseconds(1));
        profiler.EndFrame();

        // 対象フレーム
        const auto frame = profiler.Snapshot().back();
        // 親終了後の区間
        const auto* afterOuter = FindSample(frame, "AfterOuter");
        // 最上位サンプル
        const auto* rootSample = FindSample(frame, "Root");
        Require(
            afterOuter != nullptr
                && frame.samples[afterOuter->parent].name == "Inner",
            "A scope opened after an early outer end lost its open parent.");
        Require(
            rootSample != nullptr
                && rootSample->parent == LamaPon::ProfileSample::NoParent,
            "Closed scopes stayed on the thread stack.");
    }

    class RecordingListener final : public LamaPon::GpuSectionListener
    {
    public:
        // GPU区間の開始を記録します(name: 区間名)。
        void OnGpuSectionBegin(const std::string_view name) noexcept override
        {
            events.push_back("+" + std::string(name));
        }
        // GPU区間の終了を記録します。
        void OnGpuSectionEnd() noexcept override
        {
            events.push_back("-");
        }

        // 通知イベント
        std::vector<std::string> events;
    };

    class MarkerBackend final : public LamaPon::GpuProfilerBackend
    {
    public:
        // IsSupported() 計測バックエンドの対応状態を返します。
        [[nodiscard]] bool IsSupported() const noexcept override
        {
            // timestamp対応可否
            return supported;
        }
        // OpenFrame() フレーム計測を開始します。
        void OpenFrame() override {}
        // BeginSection() 時間計測区間を開始します。
        [[nodiscard]] bool BeginSection(
            std::string_view,
            std::uint32_t) override
        {
            ++timedSections;
            // 計測中区間を増やしたことを返します。
            return true;
        }
        // EndSection() 計測中区間を1つ閉じます。
        void EndSection() noexcept override
        {
            --timedSections;
        }
        // CloseFrame() フレーム計測を終了します。
        void CloseFrame() override {}
        // LatestSections() 最新フレームのGPU区間を返します。
        [[nodiscard]] const std::vector<LamaPon::GpuSectionTime>&
            LatestSections() const noexcept override
        {
            // 区間履歴
            return sections;
        }
        // LatestFrameMilliseconds() GPUフレーム時間を返します。
        [[nodiscard]] float LatestFrameMilliseconds() const noexcept override
        {
            // 未計測時の値を返します。
            return 0.0f;
        }
        // LatestPipelineStatistics() 最新の描画統計を返します。
        [[nodiscard]] const LamaPon::GpuPipelineStatistics&
            LatestPipelineStatistics() const noexcept override
        {
            // pipeline統計値
            return statistics;
        }
        // BeginMarker(name: marker名)で描画markerを開始します。
        [[nodiscard]] bool BeginMarker(
            const std::string_view name) noexcept override
        {
            markers.emplace_back(name);
            ++openMarkers;
            // markerを記録したことを返します。
            return true;
        }
        // EndMarker() 開いている描画markerを1つ閉じます。
        void EndMarker() noexcept override
        {
            --openMarkers;
        }

        // timestamp対応可否
        bool supported{};
        // 計測中区間数
        int timedSections{};
        // 開いているmarker数
        int openMarkers{};
        // marker履歴
        std::vector<std::string> markers;
        // section履歴
        std::vector<LamaPon::GpuSectionTime> sections;
        // pipeline統計値
        LamaPon::GpuPipelineStatistics statistics;
    };

    // GPU区間の転送とmarker整合性を検証します。
    void TestGpuSectionRouting()
    {
        // CPUプロファイラー
        auto& profiler = LamaPon::Profiler::Instance();
        profiler.Clear();
        profiler.SetEnabled(true);

        // GPU計測器
        LamaPon::GpuProfiler gpu;
        // 計測バックエンド
        MarkerBackend backend;
        // 区間通知listener
        RecordingListener listener;
        gpu.Attach(&backend);
        gpu.SetSectionListener(&listener);

        profiler.BeginFrame();
        {
            LAMAPON_PROFILE_SCOPE("Render");
            // Shadow計測scope
            LamaPon::GpuProfiler::SectionScope pass{ gpu, "Shadow" };
            gpu.BeginSection("Cascade");
            // 計測に非対応でもmarkerとlistenerには届きます。
            Require(
                backend.openMarkers == 2 && backend.timedSections == 0,
                "Markers must not depend on timestamp support.");
        }
        profiler.EndFrame();

        Require(
            backend.openMarkers == 0
                && backend.markers
                    == std::vector<std::string>{ "Shadow", "Cascade" },
            "Scope unwinding did not close every marker.");
        Require(
            listener.events
                == std::vector<std::string>{
                    "+Shadow", "+Cascade", "-", "-" },
            "The section listener did not observe balanced sections.");

        // 記録済みフレーム
        const auto frame = profiler.Snapshot().back();
        // Shadow区間
        const auto* shadow = FindSample(frame, "Shadow");
        // Cascade区間
        const auto* cascade = FindSample(frame, "Cascade");
        Require(
            shadow != nullptr
                && cascade != nullptr
                && frame.samples[shadow->parent].name == "Render"
                && frame.samples[cascade->parent].name == "Shadow",
            "GPU sections were not mirrored into the CPU hierarchy.");

        // 計測対応時はtimestampも入れ子で閉じます。
        backend.supported = true;
        {
            // Main計測scope
            LamaPon::GpuProfiler::SectionScope pass{ gpu, "Main" };
            gpu.BeginSection("Opaque");
            Require(
                backend.timedSections == 2,
                "Supported sections were not timed.");
        }
        Require(
            backend.timedSections == 0 && backend.openMarkers == 0,
            "Supported sections were not closed.");

        gpu.SetSectionListener(nullptr);
        gpu.BeginSection("Unobserved");
        gpu.EndSection();
        Require(
            listener.events.size() == 8,
            "A detached listener still received sections.");
        gpu.Detach();
    }

    // 階層ツリーとflat表示の集計を検証します。
    void TestFrameTreeHelpers()
    {
        // 親子サンプルを含むフレーム
        auto frame = MakeFrame(1, 6.0, 2.0, 1.0);
        frame.samples.push_back(MakeSample("Shadow", 0.5, 3, 1));
        // 階層ツリー
        const auto tree = LamaPon::BuildProfileFrameTree(frame);
        Require(
            tree.roots == std::vector<std::uint32_t>{ 0, 3 }
                && tree.children[0]
                    == std::vector<std::uint32_t>{ 1, 2 }
                && tree.children[3] == std::vector<std::uint32_t>{ 4 },
            "The frame tree does not follow parent indices.");
        Require(
            Near(tree.selfMilliseconds[0], 0.5)
                && Near(tree.selfMilliseconds[3], 0.5)
                && Near(tree.rootMilliseconds, 7.0),
            "Frame tree self times are wrong.");

        // 時間順に並べたflat表示
        const auto flat = LamaPon::FlattenProfileFrame(frame);
        Require(
            flat.front().name == "Main"
                && Near(flat.front().selfMilliseconds, 3.5),
            "The flat view is not ordered by self time.");
        // Shadow名の統合結果
        bool mergedShadow{};
        // flat区間
        for (const auto& entry : flat)
        {
            // Shadow区間か調べます。
            if (entry.name == "Shadow")
            {
                mergedShadow =
                    Near(entry.totalMilliseconds, 2.5)
                    && entry.calls == 2;
            }
        }
        Require(
            mergedShadow,
            "Same-named scopes under different parents were not merged.");

        // 比較対象フレーム
        const std::vector<LamaPon::ProfileFrame> frames{
            MakeFrame(1, 4.0, 1.0, 2.0),
            MakeFrame(2, 6.0, 3.0, 2.0),
        };
        // Render/Shadowの時系列
        const auto series =
            LamaPon::MarkerMillisecondsPerFrame(frames, "Render/Shadow");
        // 存在しない区間の時系列
        const auto missing =
            LamaPon::MarkerMillisecondsPerFrame(frames, "Missing");
        Require(
            series.size() == 2
                && Near(series[0], 1.0)
                && Near(series[1], 3.0)
                && missing.size() == 2
                && Near(missing[1], 0.0),
            "The per-frame marker series is wrong.");
    }

    // リング保持範囲と差分取得を検証します。
    void TestSnapshotSince()
    {
        // CPUプロファイラー
        auto& profiler = LamaPon::Profiler::Instance();
        profiler.Clear();
        profiler.SetFrameCapacity(3);
        // フレーム番号
        for (int frame = 0; frame < 5; ++frame)
        {
            profiler.BeginFrame();
            profiler.EndFrame();
        }
        Require(
            profiler.FrameCapacity() == 3,
            "Frame capacity was not reported.");
        // 保持中の全フレーム
        const auto all = profiler.Snapshot();
        Require(
            all.size() == 3 && all.front().index == 3,
            "The ring buffer did not drop old frames.");
        // 指定番号以降のフレーム
        const auto newer = profiler.SnapshotSince(4);
        Require(
            newer.size() == 1 && newer.front().index == 5,
            "SnapshotSince returned the wrong frames.");
        Require(
            profiler.SnapshotSince(5).empty(),
            "SnapshotSince returned already known frames.");
        Require(
            profiler.SnapshotSince(0).size() == 3,
            "SnapshotSince(0) did not return every frame.");
    }

    // 区間統計と比較結果を検証します。
    void TestAnalysisAndComparison()
    {
        // 基準プロファイル
        const std::vector<LamaPon::ProfileFrame> baseline{
            MakeFrame(1, 4.0, 1.0, 2.0),
            MakeFrame(2, 6.0, 1.0, 2.0),
            MakeFrame(3, 8.0, 3.0, 2.0),
        };
        // 基準プロファイル分析
        const auto analysis = LamaPon::AnalyzeProfile(baseline);
        Require(
            analysis.frameCount == 3
                && analysis.firstFrameIndex == 1
                && analysis.lastFrameIndex == 3,
            "The analysed range is wrong.");
        Require(
            Near(analysis.frameMilliseconds.median, 9.0)
                && Near(analysis.frameMilliseconds.minimum, 7.0)
                && Near(analysis.frameMilliseconds.maximum, 11.0)
                && analysis.frameMilliseconds.maximumFrameIndex == 3,
            "Frame time statistics are wrong.");

        // Render統計
        const auto* render = analysis.FindMarker("Render");
        // Shadow統計
        const auto* shadow = analysis.FindMarker("Render/Shadow");
        Require(
            render != nullptr && shadow != nullptr,
            "Marker paths were not built from the hierarchy.");
        Require(
            render->depth == 0
                && shadow->depth == 1
                && render->presentFrameCount == 3
                && Near(render->milliseconds.mean, 6.0)
                && Near(render->milliseconds.median, 6.0)
                && Near(render->milliseconds.percentile95, 8.0),
            "Marker statistics are wrong.");
        // Renderの自己時間は子(Shadow + Main)を除いた0.5msです。
        Require(
            Near(render->selfMilliseconds.mean, 0.5),
            "Self time did not exclude child scopes.");
        Require(
            analysis.markers.front().path == "Render"
                && analysis.markers[1].path == "Render/Shadow",
            "Markers are not ordered parent first.");

        // 比較用の遅いプロファイル
        std::vector<LamaPon::ProfileFrame> slower{
            MakeFrame(10, 10.0, 5.0, 2.0),
            MakeFrame(11, 10.0, 5.0, 2.0),
        };
        slower.back().samples.push_back(MakeSample("Audio", 1.0));
        // プロファイル差分
        const auto comparison = LamaPon::CompareProfiles(
            analysis,
            LamaPon::AnalyzeProfile(slower));
        Require(
            Near(comparison.frameMedianDifference, 13.0 - 9.0),
            "Frame median difference is wrong.");
        Require(
            comparison.markers.front().path == "Render"
                && Near(comparison.markers.front().medianDifference, 4.0)
                && Near(
                    comparison.markers.front().medianRelativeChange,
                    4.0 / 6.0),
            "Comparison is not ordered by the largest change.");
        // Audio区間の有無
        bool foundAudio{};
        // 区間比較
        for (const auto& marker : comparison.markers)
        {
            // Audio区間か調べます。
            if (marker.path == "Audio")
            {
                foundAudio = !marker.presentInA && marker.presentInB;
            }
        }
        Require(foundAudio, "A marker only in B was not reported.");

        // 空プロファイルの分析結果
        const auto empty = LamaPon::AnalyzeProfile({});
        Require(
            empty.frameCount == 0 && empty.markers.empty(),
            "An empty profile produced statistics.");
    }

    // JSON保存と旧形式読込を検証します。
    void TestJsonRoundTrip()
    {
        // JSON化する計測フレーム
        const std::vector<LamaPon::ProfileFrame> frames{
            MakeFrame(7, 6.0, 2.0, 1.0),
        };
        // 保存先ディレクトリ
        const auto outputRoot =
            std::filesystem::current_path()
            / "test-output"
            / "profile-analysis";
        // ファイル操作エラー
        std::error_code error;
        std::filesystem::remove_all(outputRoot, error);
        // 保存するJSONファイル
        const auto path = outputRoot / "capture.json";
        Require(
            LamaPon::WriteProfileJson(path, frames),
            "The capture could not be written.");

        // JSONから読み込んだフレーム
        std::vector<LamaPon::ProfileFrame> loaded;
        // JSON読込エラー
        std::string message;
        Require(
            LamaPon::LoadProfileJson(path, loaded, &message),
            "The written capture could not be read.");
        Require(
            loaded.size() == 1
                && loaded.front().index == 7
                && loaded.front().samples.size() == 4
                && loaded.front().samples[1].parent == 0
                && loaded.front().samples[1].depth == 1
                && loaded.front().samples[3].parent
                    == LamaPon::ProfileSample::NoParent,
            "The hierarchy did not survive a JSON round trip.");

        // version 1の旧形式JSON
        const std::string version1 =
            R"({"format":"LamaPonProfile","version":1,"frames":[)"
            R"({"index":1,"milliseconds":5.0,"samples":[)"
            R"({"name":"Render","milliseconds":3.0,"calls":1},)"
            R"({"name":"Simulation","milliseconds":1.0,"calls":2}]}]})";
        // version 1は親階層がなく、全区間を最上位として読みます。
        Require(
            LamaPon::ParseProfileJson(version1, loaded, &message)
                && loaded.size() == 1
                && loaded.front().samples.size() == 2
                && loaded.front().samples[1].callCount == 2
                && loaded.front().samples[1].parent
                    == LamaPon::ProfileSample::NoParent,
            "A version 1 capture could not be read.");

        // 循環検証用JSON
        const std::string forwardParent =
            R"({"format":"LamaPonProfile","version":2,"frames":[)"
            R"({"index":1,"milliseconds":1.0,"samples":[)"
            R"({"name":"A","milliseconds":1.0,"calls":1,"parent":0}]}]})";
        // 前方を指す親は循環防止のため最上位になります。
        Require(
            LamaPon::ParseProfileJson(forwardParent, loaded, &message)
                && loaded.front().samples.front().parent
                    == LamaPon::ProfileSample::NoParent,
            "A self-referencing parent was accepted.");

        // 読み込み前の件数
        const auto before = loaded.size();
        Require(
            !LamaPon::ParseProfileJson("{", loaded, &message)
                && !message.empty()
                && loaded.size() == before,
            "Broken JSON was accepted.");
        Require(
            !LamaPon::ParseProfileJson(
                R"({"format":"Other","version":1,"frames":[]})",
                loaded,
                &message),
            "A foreign format was accepted.");
        Require(
            !LamaPon::ParseProfileJson(
                R"({"format":"LamaPonProfile","version":"2","frames":[]})",
                loaded,
                &message),
            "A mistyped version was accepted.");
        Require(
            !LamaPon::LoadProfileJson(
                outputRoot / "missing.json",
                loaded,
                &message),
            "A missing file was reported as loaded.");
    }
}

// 全プロファイル分析テストを実行します。
int main()
{
    // テスト例外を捕捉します。
    try
    {
        // CPU/GPU階層、snapshot、分析、JSON互換性を順に検証します。
        TestHierarchicalScopes();
        TestScopesDoNotCrossFrames();
        TestOutOfOrderScopeEnd();
        TestGpuSectionRouting();
        TestFrameTreeHelpers();
        TestSnapshotSince();
        TestAnalysisAndComparison();
        TestJsonRoundTrip();
        std::cout << "Profile analysis tests passed.\n";
        // 全テストの成功を返します。
        return 0;
    }
    // テスト失敗を捕捉します(exception: 失敗理由)。
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        // テスト失敗を返します。
        return 1;
    }
}
