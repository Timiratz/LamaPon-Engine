#pragma once

#include "LamaPon/Assets/TextureLoader.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>

namespace LamaPon::TextureCache
{
    // PNG/JPGをWICでデコードし、CPUでミップを生成してBC1/BC3へ
    // 圧縮した結果を保存します。次回はPreparedTextureDataを直接読み、
    // 同じ変換を省略します。キャッシュを読めない場合は再生成し、
    // 保存できない場合はキャッシュを使わずに処理を続けます。

    // デコード＋ミップ生成＋圧縮の結果ひとそろい。
    struct CachedTexture final
    {
        TextureLoader::PreparedTextureData data;
        // 段階アップロード中の仮表示に使う1x1の平均色（RGBA）。
        // ミップ列の末尾がこれに当たりますが、キャッシュヒット時は
        // ミップ列そのものを作らないので別に持ちます。
        std::array<std::uint8_t, 4> placeholderPixel{};
    };

    // 置き場所。既定は shader-cache の隣
    // （%LOCALAPPDATA%\LamaPon\texture-cache）。
    [[nodiscard]] std::filesystem::path CacheDirectory();

    // 置き場所の差し替え。テストが本物のキャッシュを汚さない
    // ためのもので、空を渡すと既定へ戻ります。
    void SetCacheDirectoryOverride(std::filesystem::path directory);

    // 鍵はパスや更新日時ではなく、元ファイルの内容から作ります。
    // 出力を変える圧縮設定と用途も鍵に含めます。
    [[nodiscard]] std::uint64_t ComputeKey(
        std::span<const std::uint8_t> sourceBytes,
        bool compress,
        TextureLoader::TextureUsage usage
            = TextureLoader::TextureUsage::Color) noexcept;

    // キャッシュがない、破損している、または版が異なる場合は
    // nulloptを返します。
    [[nodiscard]] std::optional<CachedTexture> TryLoad(
        std::uint64_t key);

    // 保存に失敗した場合も例外を投げず、キャッシュなしで処理を
    // 続けます。
    void Store(std::uint64_t key, const CachedTexture& value) noexcept;
}
