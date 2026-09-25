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
    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    bool Near(const double left, const double right)
    {
        return std::abs(left - right) < 1.0e-6;
    }

    const LamaPon::ProfileSample* FindSample(
        const LamaPon::ProfileFrame& frame,
        const std::string& name)
    {
        for (const auto& sample : frame.samples)
        {
            if (sample.name == name)
            {
                return &sample;
            }
        }
        return nullptr;
    }

    LamaPon::ProfileSample MakeSample(
        std::string name,
        const double milliseconds,
        const std::uint32_t parent = LamaPon::ProfileSample::NoParent,
        const std::uint32_t depth = 0)
    {
        LamaPon::ProfileSample sample;
        sample.name = std::move(name);
        sample.milliseconds = milliseconds;
        sample.callCount = 1;
        sample.parent = parent;
        sample.depth = depth;
        return sample;
    }

    // Render(Shadow, Main) と Simulation を持つフレームを作ります。
    LamaPon::ProfileFrame MakeFrame(
        const std::uint64_t index,
        const double render,
        const double shadow,
        const double simulation)
    {
        LamaPon::ProfileFrame frame;
        frame.index = index;
        frame.milliseconds = render + simulation + 1.0;
        frame.samples.push_back(MakeSample("Render", render));
        frame.samples.push_back(MakeSample("Shadow", shadow, 0, 1));
        frame.samples.push_back(
            MakeSample("Main", render - shadow - 0.5, 0, 1));
        frame.samples.push_back(MakeSample("Simulation", simulation));
        return frame;
    }

    void TestHierarchicalScopes()
    {
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
        // 別スレッドの区間は、このスレッドのスタックと混ざらず最上位です。
        std::thread worker(
            []
            {
                LAMAPON_PROFILE_SCOPE("Worker");
            });
        worker.join();
        profiler.EndFrame();

        const auto frames = profiler.Snapshot();
        Require(frames.size() == 1, "The hierarchical frame was not kept.");
        const auto& frame = frames.front();
        Require(
            frame.samples.size() == 6,
            "Scopes were not aggregated per parent.");

        const auto& render = frame.samples[0];
        Require(
            render.name == "Render"
                && render.parent == LamaPon::ProfileSample::NoParent
                && render.depth == 0
                && render.callCount == 1,
            "The top-level scope is wrong.");
        const auto& shadow = frame.samples[1];
        Require(
            shadow.name == "Shadow"
                && shadow.parent == 0
                && shadow.depth == 1
                && shadow.callCount == 2,
            "Repeated child scopes were not aggregated under the parent.");
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
        for (std::size_t index{}; index < frame.samples.size(); ++index)
        {
            const auto parent = frame.samples[index].parent;
            Require(
                parent == LamaPon::ProfileSample::NoParent
                    || parent < index,
                "A child was stored before its parent.");
        }
    }

    void TestScopesDoNotCrossFrames()
    {
        auto& profiler = LamaPon::Profiler::Instance();
        profiler.Clear();
        profiler.BeginFrame();
        auto* scope = new LamaPon::ProfileScope("Open");
        profiler.EndFrame();
        profiler.BeginFrame();
        // 前フレームで開いた区間を閉じても、新しいフレームの添字を
        // 書き換えません。
        {
            LAMAPON_PROFILE_SCOPE("Fresh");
        }
        delete scope;
        {
            LAMAPON_PROFILE_SCOPE("AfterStale");
        }
        profiler.EndFrame();

        const auto frames = profiler.Snapshot();
        Require(frames.size() == 2, "Frames were not recorded.");
        const auto& second = frames.back();
        Require(
            FindSample(second, "Open") == nullptr
                && FindSample(second, "Fresh") != nullptr
                && FindSample(second, "Fresh")->callCount == 1,
            "A scope leaked across frames.");
        const auto* afterStale = FindSample(second, "AfterStale");
        Require(
            afterStale != nullptr
                && afterStale->parent
                    == LamaPon::ProfileSample::NoParent,
            "A stale scope stayed on the thread stack.");
    }

    void TestOutOfOrderScopeEnd()
    {
        auto& profiler = LamaPon::Profiler::Instance();
        profiler.Clear();
        profiler.BeginFrame();
        const auto outer = profiler.BeginScope("Outer");
        const auto inner = profiler.BeginScope("Inner");
        // GPU区間のEnd()のように、外側が内側より先に閉じられる場合です。
        profiler.EndScope(outer, std::chrono::milliseconds(1));
        const auto sibling = profiler.BeginScope("AfterOuter");
        profiler.EndScope(sibling, std::chrono::milliseconds(1));
        profiler.EndScope(inner, std::chrono::milliseconds(1));
        const auto root = profiler.BeginScope("Root");
        profiler.EndScope(root, std::chrono::milliseconds(1));
        profiler.EndFrame();

        const auto frame = profiler.Snapshot().back();
        const auto* afterOuter = FindSample(frame, "AfterOuter");
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
        void OnGpuSectionBegin(const std::string_view name) noexcept override
        {
            events.push_back("+" + std::string(name));
        }
        void OnGpuSectionEnd() noexcept override
        {
            events.push_back("-");
        }

        std::vector<std::string> events;
    };

    class MarkerBackend final : public LamaPon::GpuProfilerBackend
    {
    public:
        [[nodiscard]] bool IsSupported() const noexcept override
        {
            return supported;
        }
        void OpenFrame() override {}
        [[nodiscard]] bool BeginSection(
            std::string_view,
            std::uint32_t) override
        {
            ++timedSections;
            return true;
        }
        void EndSection() noexcept override
        {
            --timedSections;
        }
        void CloseFrame() override {}
        [[nodiscard]] const std::vector<LamaPon::GpuSectionTime>&
            LatestSections() const noexcept override
        {
            return sections;
        }
        [[nodiscard]] float LatestFrameMilliseconds() const noexcept override
        {
            return 0.0f;
        }
        [[nodiscard]] const LamaPon::GpuPipelineStatistics&
            LatestPipelineStatistics() const noexcept override
        {
            return statistics;
        }
        [[nodiscard]] bool BeginMarker(
            const std::string_view name) noexcept override
        {
            markers.emplace_back(name);
            ++openMarkers;
            return true;
        }
        void EndMarker() noexcept override
        {
            --openMarkers;
        }

        bool supported{};
        int timedSections{};
        int openMarkers{};
        std::vector<std::string> markers;
        std::vector<LamaPon::GpuSectionTime> sections;
        LamaPon::GpuPipelineStatistics statistics;
    };

    void TestGpuSectionRouting()
    {
        auto& profiler = LamaPon::Profiler::Instance();
        profiler.Clear();
        profiler.SetEnabled(true);

        LamaPon::GpuProfiler gpu;
        MarkerBackend backend;
        RecordingListener listener;
        gpu.Attach(&backend);
        gpu.SetSectionListener(&listener);

        profiler.BeginFrame();
        {
            LAMAPON_PROFILE_SCOPE("Render");
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

        const auto frame = profiler.Snapshot().back();
        const auto* shadow = FindSample(frame, "Shadow");
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

    void TestFrameTreeHelpers()
    {
        auto frame = MakeFrame(1, 6.0, 2.0, 1.0);
        frame.samples.push_back(MakeSample("Shadow", 0.5, 3, 1));
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

        const auto flat = LamaPon::FlattenProfileFrame(frame);
        Require(
            flat.front().name == "Main"
                && Near(flat.front().selfMilliseconds, 3.5),
            "The flat view is not ordered by self time.");
        bool mergedShadow{};
        for (const auto& entry : flat)
        {
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

        const std::vector<LamaPon::ProfileFrame> frames{
            MakeFrame(1, 4.0, 1.0, 2.0),
            MakeFrame(2, 6.0, 3.0, 2.0),
        };
        const auto series =
            LamaPon::MarkerMillisecondsPerFrame(frames, "Render/Shadow");
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

    void TestSnapshotSince()
    {
        auto& profiler = LamaPon::Profiler::Instance();
        profiler.Clear();
        profiler.SetFrameCapacity(3);
        for (int frame = 0; frame < 5; ++frame)
        {
            profiler.BeginFrame();
            profiler.EndFrame();
        }
        Require(
            profiler.FrameCapacity() == 3,
            "Frame capacity was not reported.");
        const auto all = profiler.Snapshot();
        Require(
            all.size() == 3 && all.front().index == 3,
            "The ring buffer did not drop old frames.");
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

    void TestAnalysisAndComparison()
    {
        const std::vector<LamaPon::ProfileFrame> baseline{
            MakeFrame(1, 4.0, 1.0, 2.0),
            MakeFrame(2, 6.0, 1.0, 2.0),
            MakeFrame(3, 8.0, 3.0, 2.0),
        };
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

        const auto* render = analysis.FindMarker("Render");
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

        std::vector<LamaPon::ProfileFrame> slower{
            MakeFrame(10, 10.0, 5.0, 2.0),
            MakeFrame(11, 10.0, 5.0, 2.0),
        };
        slower.back().samples.push_back(MakeSample("Audio", 1.0));
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
        bool foundAudio{};
        for (const auto& marker : comparison.markers)
        {
            if (marker.path == "Audio")
            {
                foundAudio = !marker.presentInA && marker.presentInB;
            }
        }
        Require(foundAudio, "A marker only in B was not reported.");

        const auto empty = LamaPon::AnalyzeProfile({});
        Require(
            empty.frameCount == 0 && empty.markers.empty(),
            "An empty profile produced statistics.");
    }

    void TestJsonRoundTrip()
    {
        const std::vector<LamaPon::ProfileFrame> frames{
            MakeFrame(7, 6.0, 2.0, 1.0),
        };
        const auto outputRoot =
            std::filesystem::current_path()
            / "test-output"
            / "profile-analysis";
        std::error_code error;
        std::filesystem::remove_all(outputRoot, error);
        const auto path = outputRoot / "capture.json";
        Require(
            LamaPon::WriteProfileJson(path, frames),
            "The capture could not be written.");

        std::vector<LamaPon::ProfileFrame> loaded;
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

        // version 1は階層を持たず、全区間を最上位として読みます。
        const std::string version1 =
            R"({"format":"LamaPonProfile","version":1,"frames":[)"
            R"({"index":1,"milliseconds":5.0,"samples":[)"
            R"({"name":"Render","milliseconds":3.0,"calls":1},)"
            R"({"name":"Simulation","milliseconds":1.0,"calls":2}]}]})";
        Require(
            LamaPon::ParseProfileJson(version1, loaded, &message)
                && loaded.size() == 1
                && loaded.front().samples.size() == 2
                && loaded.front().samples[1].callCount == 2
                && loaded.front().samples[1].parent
                    == LamaPon::ProfileSample::NoParent,
            "A version 1 capture could not be read.");

        // 前方を指さない親は循環を避けるため最上位として扱います。
        const std::string forwardParent =
            R"({"format":"LamaPonProfile","version":2,"frames":[)"
            R"({"index":1,"milliseconds":1.0,"samples":[)"
            R"({"name":"A","milliseconds":1.0,"calls":1,"parent":0}]}]})";
        Require(
            LamaPon::ParseProfileJson(forwardParent, loaded, &message)
                && loaded.front().samples.front().parent
                    == LamaPon::ProfileSample::NoParent,
            "A self-referencing parent was accepted.");

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

int main()
{
    try
    {
        TestHierarchicalScopes();
        TestScopesDoNotCrossFrames();
        TestOutOfOrderScopeEnd();
        TestGpuSectionRouting();
        TestFrameTreeHelpers();
        TestSnapshotSince();
        TestAnalysisAndComparison();
        TestJsonRoundTrip();
        std::cout << "Profile analysis tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
