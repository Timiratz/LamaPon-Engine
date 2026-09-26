#include "LamaPon/Graphics/MemorySnapshotCapture.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Audio/AudioSystem.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/RenderTarget.h"

#include <exception>
#include <string>

namespace LamaPon
{
    namespace
    {
        // レンダーテクスチャはHDRの色（RGBA16F）と深度（D24S8）を
        // 1組で持つため、1ピクセルあたり12バイトとして見積もります。
        constexpr std::uint64_t RenderTextureBytesPerPixel = 8 + 4;

        void Warn(const char* category, const std::exception& exception)
        {
            Logger::Instance().Warning(
                std::string{ "メモリ内訳の" } + category
                + "を取得できませんでした: " + exception.what());
        }
    }

    MemorySnapshot CaptureMemorySnapshot(GraphicsDevice& graphics)
    {
        MemorySnapshot snapshot;
        graphics.RefreshMemoryStatistics(true);
        const auto& statistics = graphics.MemoryStats();
        snapshot.process.processWorkingSetBytes =
            statistics.processWorkingSetBytes;
        snapshot.process.processPrivateBytes =
            statistics.processPrivateBytes;
        snapshot.process.localVideoMemoryUsageBytes =
            statistics.localVideoMemoryUsageBytes;
        snapshot.process.nonLocalVideoMemoryUsageBytes =
            statistics.nonLocalVideoMemoryUsageBytes;
        snapshot.process.videoMemoryAvailable =
            statistics.videoMemoryBudgetAvailable;

        if (!graphics.IsInitialized())
        {
            return snapshot;
        }
        try
        {
            graphics.Assets().AppendMemoryEntries(snapshot.entries);
        }
        catch (const std::exception& exception)
        {
            Warn("アセット", exception);
        }
        try
        {
            graphics.Audio().AppendMemoryEntries(snapshot.entries);
        }
        catch (const std::exception& exception)
        {
            Warn("オーディオ", exception);
        }
        try
        {
            for (const auto& name : graphics.RenderTextureNames())
            {
                const auto* target = graphics.FindRenderTexture(name);
                if (target == nullptr)
                {
                    continue;
                }
                MemorySnapshotEntry entry;
                entry.category = MemoryCategory::RenderTexture;
                entry.name = name;
                entry.detail =
                    std::to_string(target->Width()) + "x"
                    + std::to_string(target->Height())
                    + "（色+深度の推定）";
                entry.gpuBytes =
                    static_cast<std::uint64_t>(target->Width())
                    * target->Height()
                    * RenderTextureBytesPerPixel;
                snapshot.entries.push_back(std::move(entry));
            }
        }
        catch (const std::exception& exception)
        {
            Warn("レンダーテクスチャ", exception);
        }
        return snapshot;
    }
}
