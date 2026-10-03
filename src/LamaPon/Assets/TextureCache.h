#pragma once

#include "LamaPon/Assets/TextureLoader.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>

namespace LamaPon::TextureCache
{
    // 転送用の画素・ミップ列を保存し、キャッシュ未取得時は元画像から再生成する。

    // GPUへ転送する画素列と段階読み込み用の仮表示色。
    struct CachedTexture final
    {
        // 転送用の形式と各ミップ画素
        TextureLoader::PreparedTextureData data;
        // 段階転送の仮表示用1×1RGBA
        std::array<std::uint8_t, 4> placeholderPixel{};
    };

    // 差し替え先またはOS標準領域のキャッシュ保存先を返す。
    [[nodiscard]] std::filesystem::path CacheDirectory();

    // 保存先を差し替え、空なら既定へ戻す(directory: 差し替える保存先)。
    void SetCacheDirectoryOverride(std::filesystem::path directory);

    // 内容・圧縮指定・用途・形式版からキーを作る(sourceBytes: 元画像のバイト列, compress: 圧縮の指定, usage: 色または法線の用途)。
    [[nodiscard]] std::uint64_t ComputeKey(
        std::span<const std::uint8_t> sourceBytes,
        bool compress,
        TextureLoader::TextureUsage usage
            = TextureLoader::TextureUsage::Color) noexcept;

    // 形式とミップを検証し、未保存・不正な結果はnulloptにする(key: 内容由来のキー)。
    [[nodiscard]] std::optional<CachedTexture> TryLoad(
        std::uint64_t key);

    // 小さい結果を除いて保存し、失敗は呼び出し側へ伝播しない(key: 内容由来のキー, value: 転送用画素と仮表示色)。
    void Store(std::uint64_t key, const CachedTexture& value) noexcept;
}
