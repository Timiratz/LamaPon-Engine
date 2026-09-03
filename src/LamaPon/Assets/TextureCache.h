#pragma once

#include "LamaPon/Assets/TextureLoader.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>

namespace LamaPon::TextureCache
{
    // テクスチャのディスクキャッシュ。
    //
    // PNG/JPGの読み込みは WICデコード → CPUミップ生成 → BC1/BC3
    // 圧縮 の3段で、結果は毎回同じなのに毎起動やり直していました。
    // 2048x2048で1枚数十ms級のCPU仕事です。ここでは最終形
    // （PreparedTextureData）を丸ごとファイルへ残し、2回目以降は
    // 読むだけにします。shader-cacheで実証済みの構図
    // （3484ms→25ms）のテクスチャ版です。
    //
    // キャッシュはあくまで高速化で、無くても壊れていても正しさには
    // 影響しません。読めなければ作り直し、書けなければ諦めます。

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

    // 鍵は**元ファイルの内容**から作ります（パスやmtimeではなく）。
    // robocopyやgitはmtimeを保ったり変えたりしますが内容とは無関係
    // で、mtimeを信じて古い結果を読む事故をVM検証で繰り返しました。
    // 圧縮設定と用途は結果の形そのものを変えるので鍵に混ぜます。
    [[nodiscard]] std::uint64_t ComputeKey(
        std::span<const std::uint8_t> sourceBytes,
        bool compress,
        TextureLoader::TextureUsage usage
            = TextureLoader::TextureUsage::Color) noexcept;

    // 読み込み。無い・壊れている・版が違うときはnullopt
    // （呼ぶ側は普通に作り直せばよい）。
    [[nodiscard]] std::optional<CachedTexture> TryLoad(
        std::uint64_t key);

    // 保存。失敗しても何も起きません（書けないディスクや権限は
    // 高速化を諦める理由にはなっても、読み込み失敗の理由には
    // ならないため）。
    void Store(std::uint64_t key, const CachedTexture& value) noexcept;
}
