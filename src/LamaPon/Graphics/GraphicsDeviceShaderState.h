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
        // 材質用コンパイル済み効果
        std::unique_ptr<LitEffect> effect;

        // コンパイルする追加マクロ一覧
        std::vector<std::string> keywords;
        // 終了時はワーカーの完了を待ってから、その参照先を解放する。
        // 非同期コンパイルの完了待ち
        std::future<void> warming;

        // 標準効果で描く準備待ち
        bool pending{};
        // 確認したソース更新時刻
        std::filesystem::file_time_type writeTime{};
        // 依存ファイルの更新識別子
        std::uint64_t dependencyRevision{};
        // コンパイル済み世代番号
        std::uint64_t generation{};
        // 直近のコンパイルエラー
        std::string error;
        // 次回ソース確認時刻
        std::chrono::steady_clock::time_point nextCheck{};
        // ソース確認実施済み
        bool observed{};
        // 確認時のソース存在有無
        bool sourceExists{};
        // 次回確認で再読込する
        bool forceReload{};
    };

    struct GraphicsDevice::SpriteShaderEntry final
    {
        // SpriteBatchのEndまで開始時の効果の世代を保持し、再読込の影響を避ける。
        // 共有する粒子・画像用効果
        std::shared_ptr<SpriteEffect> effect;
        // 確認したソース更新時刻
        std::filesystem::file_time_type writeTime{};
        // 依存ファイルの更新識別子
        std::uint64_t dependencyRevision{};
        // コンパイル済み世代番号
        std::uint64_t generation{};
        // 直近のコンパイルエラー
        std::string error;
        // 次回ソース確認時刻
        std::chrono::steady_clock::time_point nextCheck{};
        // ソース確認実施済み
        bool observed{};
        // 確認時のソース存在有無
        bool sourceExists{};
        // 次回確認で再読込する
        bool forceReload{};
    };

    struct GraphicsDevice::ScreenShaderEntry final
    {
        // 予約した処理の実行まで投入時の効果の世代を保持し、再読込の影響を避ける。
        // 共有する画面用効果
        std::shared_ptr<ScreenEffect> effect;
        // 確認したソース更新時刻
        std::filesystem::file_time_type writeTime{};
        // 依存ファイルの更新識別子
        std::uint64_t dependencyRevision{};
        // コンパイル済み世代番号
        std::uint64_t generation{};
        // 直近のコンパイルエラー
        std::string error;
        // 次回ソース確認時刻
        std::chrono::steady_clock::time_point nextCheck{};
        // ソース確認実施済み
        bool observed{};
        // 確認時のソース存在有無
        bool sourceExists{};
        // 次回確認で再読込する
        bool forceReload{};
    };

    struct GraphicsDevice::ComputeShaderEntry final
    {
        // 計算用コンパイル済み効果
        std::unique_ptr<ComputeEffect> effect;
        // 確認したソース更新時刻
        std::filesystem::file_time_type writeTime{};
        // 依存ファイルの更新識別子
        std::uint64_t dependencyRevision{};
        // 直近のコンパイルエラー
        std::string error;
        // 次回ソース確認時刻
        std::chrono::steady_clock::time_point nextCheck{};
        // ソース確認実施済み
        bool observed{};
        // 確認時のソース存在有無
        bool sourceExists{};
        // 次回確認で再読込する
        bool forceReload{};
    };

    struct GraphicsDevice::QueuedScreenEffect final
    {
        // 予約時の画面効果
        std::shared_ptr<ScreenEffect> effect;
        // 保持する補助テクスチャ
        std::array<
            std::shared_ptr<const TextureAsset>,
            2> auxiliaryTextures{};
        // 画面効果の独自パラメーター
        ScreenEffect::CustomParameters parameters{};
        // 画面効果の実行位置
        ScreenEffectPoint point{
            ScreenEffectPoint::AfterToneMapping };
    };
}
