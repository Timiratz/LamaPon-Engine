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
        // 色8＋深度4の画素当たりバイト
        constexpr std::uint64_t RenderTextureBytesPerPixel = 8 + 4;

        // 分類の収集失敗を警告へ記録する(category: 資源の分類名, exception: 収集失敗の診断)。
        void Warn(const char* category, const std::exception& exception)
        {
            Logger::Instance().Warning(
                std::string{ "メモリ内訳の" } + category
                + "を取得できませんでした: " + exception.what());
        }
    }

    MemorySnapshot CaptureMemorySnapshot(GraphicsDevice& graphics)
    {
        // 取得したメモリー情報の集合
        MemorySnapshot snapshot;
        graphics.RefreshMemoryStatistics(true);
        // 更新済みのメモリー総量
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
        // 警告へ記録する分類の収集失敗
        catch (const std::exception& exception)
        {
            Warn("アセット", exception);
        }
        try
        {
            graphics.Audio().AppendMemoryEntries(snapshot.entries);
        }
        // 警告へ記録する分類の収集失敗
        catch (const std::exception& exception)
        {
            Warn("オーディオ", exception);
        }
        try
        {
            // 集計する描画テクスチャ名
            for (const auto& name : graphics.RenderTextureNames())
            {
                // 集計する描画先の参照
                const auto* target = graphics.FindRenderTexture(name);
                if (target == nullptr)
                {
                    continue;
                }
                // 色と深度の推定メモリー項目
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
        // 警告へ記録する分類の収集失敗
        catch (const std::exception& exception)
        {
            Warn("レンダーテクスチャ", exception);
        }
        return snapshot;
    }
}
