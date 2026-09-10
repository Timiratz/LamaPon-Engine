#pragma once

#include "LamaPon/Graphics/GraphicsDevice.h"

#include "LamaPon/Graphics/ComputeEffect.h"
#include "LamaPon/Graphics/LitEffect.h"
#include "LamaPon/Graphics/ScreenEffect.h"
#include "LamaPon/Graphics/SpriteEffect.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace LamaPon
{
    struct TextureAsset;

    struct GraphicsDevice::MaterialShaderEntry final
    {
        std::unique_ptr<LitEffect> effect;
        // このエントリーのバリアント（#pragma multi_compileの
        // キーワード）。同じHLSLでも組み合わせごとに別エントリーです。
        std::vector<std::string> keywords;
        // 非同期コンパイル中の待ち合わせ。std::asyncのfutureは
        // デストラクターが完了を待つので、GraphicsDeviceを畳んだ
        // ときにワーカーが取り残されることはありません。
        std::future<void> warming;
        // バイトコードの用意待ち。trueの間は標準Litで描きます。
        bool pending{};
        std::filesystem::file_time_type writeTime{};
        std::uint64_t generation{};
        std::string error;
        std::chrono::steady_clock::time_point nextCheck{};
        bool observed{};
        bool sourceExists{};
        bool forceReload{};
    };

    struct GraphicsDevice::SpriteShaderEntry final
    {
        std::unique_ptr<SpriteEffect> effect;
        std::filesystem::file_time_type writeTime{};
        std::uint64_t generation{};
        std::string error;
        std::chrono::steady_clock::time_point nextCheck{};
        bool observed{};
        bool sourceExists{};
        bool forceReload{};
    };

    struct GraphicsDevice::ScreenShaderEntry final
    {
        std::unique_ptr<ScreenEffect> effect;
        std::filesystem::file_time_type writeTime{};
        std::uint64_t generation{};
        std::string error;
        std::chrono::steady_clock::time_point nextCheck{};
        bool observed{};
        bool sourceExists{};
        bool forceReload{};
    };

    struct GraphicsDevice::ComputeShaderEntry final
    {
        std::unique_ptr<ComputeEffect> effect;
        std::filesystem::file_time_type writeTime{};
        std::string error;
        std::chrono::steady_clock::time_point nextCheck{};
        bool observed{};
        bool sourceExists{};
        bool forceReload{};
    };

    struct GraphicsDevice::QueuedScreenEffect final
    {
        ScreenEffect* effect{};
        std::array<
            std::shared_ptr<const TextureAsset>,
            2> auxiliaryTextures{};
        ScreenEffect::CustomParameters parameters{};
        ScreenEffectPoint point{
            ScreenEffectPoint::AfterToneMapping };
    };
}
