#include "LamaPon/Core/MemorySnapshot.h"

#include <filesystem>
#include <iostream>
#include <limits>
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

    LamaPon::MemorySnapshotEntry Entry(
        const LamaPon::MemoryCategory category,
        std::string name,
        const std::uint64_t gpuBytes,
        const std::uint64_t cpuBytes = 0)
    {
        LamaPon::MemorySnapshotEntry entry;
        entry.category = category;
        entry.name = std::move(name);
        entry.gpuBytes = gpuBytes;
        entry.cpuBytes = cpuBytes;
        return entry;
    }

    void TestSummaryAndComparison()
    {
        using LamaPon::MemoryCategory;
        LamaPon::MemorySnapshot before;
        before.process.processPrivateBytes = 1000;
        before.entries = {
            Entry(MemoryCategory::Texture, "textures/a.png", 400),
            Entry(MemoryCategory::Texture, "textures/b.png", 100),
            Entry(MemoryCategory::Model, "models/hero.glb", 300, 200),
            Entry(MemoryCategory::Audio, "audio/se.wav", 0, 50),
        };
        LamaPon::MemorySnapshot after = before;
        after.process.processPrivateBytes = 900;
        // aは消え、bは大きくなり、cが増え、モデルと音は変わりません。
        after.entries = {
            Entry(MemoryCategory::Texture, "textures/b.png", 250),
            Entry(MemoryCategory::Texture, "textures/c.png", 1000),
            Entry(MemoryCategory::Model, "models/hero.glb", 300, 200),
            Entry(MemoryCategory::Audio, "audio/se.wav", 0, 50),
        };

        const auto totals = LamaPon::SummarizeMemorySnapshot(before);
        const auto& textures =
            totals[static_cast<std::size_t>(MemoryCategory::Texture)];
        const auto& models =
            totals[static_cast<std::size_t>(MemoryCategory::Model)];
        Require(
            textures.count == 2
                && textures.gpuBytes == 500
                && models.gpuBytes == 300
                && models.cpuBytes == 200,
            "Category totals are wrong.");

        const auto comparison =
            LamaPon::CompareMemorySnapshots(before, after);
        Require(
            comparison.processPrivateDelta == -100,
            "The process delta is wrong.");
        Require(
            comparison.entries.size() == 3,
            "Unchanged resources must not be reported.");
        Require(
            comparison.entries[0].name == "textures/c.png"
                && comparison.entries[0].change
                    == LamaPon::MemoryEntryChange::Added
                && comparison.entries[0].DeltaBytes() == 1000,
            "The largest change must come first.");
        Require(
            comparison.entries[1].name == "textures/a.png"
                && comparison.entries[1].change
                    == LamaPon::MemoryEntryChange::Removed
                && comparison.entries[1].DeltaBytes() == -400,
            "A removed resource was not reported.");
        Require(
            comparison.entries[2].name == "textures/b.png"
                && comparison.entries[2].change
                    == LamaPon::MemoryEntryChange::Changed
                && comparison.entries[2].DeltaBytes() == 150,
            "A resized resource was not reported.");
    }

    void TestFormattingAndEstimates()
    {
        Require(
            LamaPon::FormatMemoryBytes(512) == "512 B"
                && LamaPon::FormatMemoryBytes(1536) == "1.5 KiB"
                && LamaPon::FormatMemoryBytes(3ull * 1024 * 1024)
                    == "3.00 MiB",
            "Byte formatting is wrong.");
        Require(
            LamaPon::FormatMemoryDelta(-2048) == "-2.0 KiB"
                && LamaPon::FormatMemoryDelta(10) == "+10 B"
                && !LamaPon::FormatMemoryDelta(
                    std::numeric_limits<std::int64_t>::min()).empty(),
            "Delta formatting is wrong.");

        // 4x4 RGBA8の完全なミップ列: 64 + 16 + 4 = 84バイト。
        Require(
            LamaPon::EstimateTextureBytes(4, 4, 1, 3, 32, false) == 84,
            "Uncompressed mip estimate is wrong.");
        // BC1(4bpp)の各ミップは4x4ブロックへ切り上げ: 8 + 8 + 8。
        Require(
            LamaPon::EstimateTextureBytes(4, 4, 1, 3, 4, true) == 24,
            "Block-compressed mip padding is wrong.");
        // キューブマップは6面分です。
        Require(
            LamaPon::EstimateTextureBytes(2, 2, 6, 1, 32, false)
                == 6 * 16,
            "Array slices were not multiplied.");
    }

    void TestJsonRoundTrip()
    {
        LamaPon::MemorySnapshot snapshot;
        snapshot.label = "起動直後";
        snapshot.capturedAt = "20260925-120000";
        snapshot.process.processWorkingSetBytes = 11;
        snapshot.process.processPrivateBytes = 22;
        snapshot.process.localVideoMemoryUsageBytes = 33;
        snapshot.process.videoMemoryAvailable = true;
        snapshot.entries = {
            Entry(
                LamaPon::MemoryCategory::RenderTexture,
                "minimap",
                4096),
            Entry(LamaPon::MemoryCategory::Animation, "run.anim", 0, 77),
        };
        snapshot.entries.front().detail = "256x256";

        const auto root =
            std::filesystem::current_path()
            / "test-output"
            / "memory-snapshot";
        std::error_code error;
        std::filesystem::remove_all(root, error);
        const auto path = root / "snapshot.json";
        Require(
            LamaPon::WriteMemorySnapshotJson(path, snapshot),
            "The snapshot could not be written.");

        LamaPon::MemorySnapshot loaded;
        std::string message;
        Require(
            LamaPon::LoadMemorySnapshotJson(path, loaded, &message),
            "The written snapshot could not be read.");
        Require(
            loaded.label == snapshot.label
                && loaded.process.processPrivateBytes == 22
                && loaded.process.videoMemoryAvailable
                && loaded.entries.size() == 2
                && loaded.entries[0].category
                    == LamaPon::MemoryCategory::RenderTexture
                && loaded.entries[0].detail == "256x256"
                && loaded.entries[1].cpuBytes == 77,
            "The snapshot did not survive a JSON round trip.");

        // 未知の分類は「その他」として読み、壊れたJSONは拒否します。
        const std::string unknown =
            R"({"format":"LamaPonMemorySnapshot","version":1,)"
            R"("entries":[{"category":"future","name":"x","gpuBytes":1}]})";
        Require(
            LamaPon::ParseMemorySnapshotJson(unknown, loaded, &message)
                && loaded.entries.size() == 1
                && loaded.entries[0].category
                    == LamaPon::MemoryCategory::Other,
            "An unknown category was not mapped to Other.");
        Require(
            !LamaPon::ParseMemorySnapshotJson("[", loaded, &message)
                && loaded.entries.size() == 1,
            "Broken JSON replaced the snapshot.");
        Require(
            !LamaPon::ParseMemorySnapshotJson(
                R"({"format":"LamaPonMemorySnapshot","version":1,)"
                R"("entries":[{"gpuBytes":"big"}]})",
                loaded,
                &message)
                && !message.empty(),
            "A mistyped size was accepted.");
    }
}

int main()
{
    try
    {
        TestSummaryAndComparison();
        TestFormattingAndEstimates();
        TestJsonRoundTrip();
        std::cout << "Memory snapshot tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
