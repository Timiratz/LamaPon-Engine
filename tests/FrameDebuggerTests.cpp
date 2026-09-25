#include "LamaPon/Graphics/FrameDebugger.h"
#include "LamaPon/Graphics/GpuProfiler.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    bool Submit(
        LamaPon::FrameDebugger& debugger,
        const std::uint64_t objectId,
        const char* objectName,
        const LamaPon::FrameDebugPass pass =
            LamaPon::FrameDebugPass::Color)
    {
        LamaPon::FrameDebugDrawDescription description;
        description.geometry = "立方体";
        description.triangleCount = 12;
        return debugger.SubmitDrawEvent(
            LamaPon::FrameDebugEventKind::Draw3D,
            pass,
            objectId,
            objectName,
            "MeshRenderer",
            std::move(description));
    }

    void TestDisabledDebuggerIsTransparent()
    {
        LamaPon::FrameDebugger debugger;
        debugger.SetEventLimit(0);
        Require(
            Submit(debugger, 1, "A") && Submit(debugger, 2, "B"),
            "A disabled frame debugger must never skip drawing.");
        debugger.EndFrame();
        Require(
            debugger.LastFrameEvents().empty()
                && debugger.CompletedFrames() == 0,
            "A disabled frame debugger must not record events.");
    }

    void TestEventsFollowGpuSections()
    {
        LamaPon::FrameDebugger debugger;
        LamaPon::GpuProfiler gpu;
        gpu.SetSectionListener(&debugger);
        debugger.SetEnabled(true);

        {
            LamaPon::GpuProfiler::SectionScope shadow{ gpu, "シャドウ" };
            Require(
                Submit(
                    debugger,
                    1,
                    "Player",
                    LamaPon::FrameDebugPass::ShadowDepth),
                "Recording without a limit must draw.");
        }
        {
            LamaPon::GpuProfiler::SectionScope main{ gpu, "3D描画" };
            LamaPon::GpuProfiler::SectionScope inner{ gpu, "不透明" };
            Require(
                Submit(debugger, 1, "Player")
                    && Submit(debugger, 2, "Ground"),
                "Recording without a limit must draw.");
        }
        Require(
            Submit(debugger, 3, "HUD"),
            "Recording without a limit must draw.");
        gpu.CloseFrame();
        debugger.EndFrame();

        const auto& events = debugger.LastFrameEvents();
        Require(
            events.size() == 4 && debugger.CompletedFrames() == 1,
            "Every submitted draw must be recorded.");
        Require(
            events[0].sectionPath == "シャドウ"
                && events[0].pass == LamaPon::FrameDebugPass::ShadowDepth
                && events[1].sectionPath == "3D描画/不透明"
                && events[2].objectName == "Ground"
                && events[3].sectionPath.empty(),
            "Events were not grouped by the open GPU sections.");
        for (std::uint32_t index{}; index < events.size(); ++index)
        {
            Require(
                events[index].index == index
                    && !events[index].skipped
                    && events[index].componentType == "MeshRenderer"
                    && events[index].description.triangleCount == 12,
                "Event metadata was not preserved.");
        }
        gpu.SetSectionListener(nullptr);
    }

    void TestLimitSkipsLaterDraws()
    {
        LamaPon::FrameDebugger debugger;
        debugger.SetEnabled(true);
        debugger.SetEventLimit(1);
        const bool first = Submit(debugger, 1, "A");
        const bool second = Submit(debugger, 2, "B");
        const bool third = Submit(debugger, 3, "C");
        debugger.EndFrame();
        Require(
            first && second && !third,
            "Only draws up to the limit may be issued.");
        const auto& events = debugger.LastFrameEvents();
        Require(
            events.size() == 3
                && !events[1].skipped
                && events[2].skipped,
            "Skipped draws must still be listed so they can be selected.");

        // 次のフレームは番号が0から振り直されます。
        debugger.SetEventLimit(std::nullopt);
        Require(
            Submit(debugger, 1, "A")
                && Submit(debugger, 2, "B")
                && Submit(debugger, 3, "C"),
            "Clearing the limit must draw every event again.");
        debugger.EndFrame();
        Require(
            debugger.LastFrameEvents().back().index == 2
                && debugger.CompletedFrames() == 2,
            "Event indices must restart every frame.");
    }

    void TestToggleAndUnbalancedSections()
    {
        LamaPon::FrameDebugger debugger;
        // 有効化より前に始まった区間の終了は無視されます。
        debugger.OnGpuSectionBegin("before");
        debugger.SetEnabled(true);
        debugger.OnGpuSectionEnd();
        debugger.OnGpuSectionBegin("after");
        Require(
            Submit(debugger, 1, "A"),
            "Recording without a limit must draw.");
        debugger.OnGpuSectionEnd();
        debugger.OnGpuSectionEnd();
        debugger.EndFrame();
        Require(
            debugger.LastFrameEvents().size() == 1
                && debugger.LastFrameEvents().front().sectionPath
                    == "after",
            "An end without a matching begin corrupted the section path.");

        debugger.SetEnabled(false);
        Require(
            debugger.LastFrameEvents().empty(),
            "Disabling must release the recorded frame.");
    }
}

int main()
{
    try
    {
        TestDisabledDebuggerIsTransparent();
        TestEventsFollowGpuSections();
        TestLimitSkipsLaterDraws();
        TestToggleAndUnbalancedSections();
        std::cout << "Frame debugger tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
