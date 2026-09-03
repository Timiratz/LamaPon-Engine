#pragma once

#include "LamaPon/Graphics/EnvironmentRenderer.h"

#include <cstdint>
#include <filesystem>
#include <span>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace LamaPon::EnvironmentCache
{
    // 事前畳み込み済み環境（IBL）のディスクキャッシュ。
    //
    // リフレクションプローブはシーンを開くたびに自動で再ベイク
    // されます（6面の描画＋GGX畳み込み×プローブ数）。SkyboxのIBLも
    // キューブマップを設定するたびに畳み込みが走ります。どちらも
    // 結果は前回と同じなのにGPUでやり直していました。ここでは
    // 畳み込みの**出力**（スペキュラ8ミップ＋放射照度、fp16キューブ
    // 2枚で1組あたり約1MB）を保存し、2回目以降はファイルから
    // テクスチャを作るだけにします。
    //
    // 何をもって「同じ」とするかは呼ぶ側が鍵で表します（プローブは
    // シーンのパス＋位置＋範囲、Skyはキューブマップの内容ハッシュ）。

    // 置き場所（既定は%LOCALAPPDATA%\LamaPon\environment-cache）。
    [[nodiscard]] std::filesystem::path CacheDirectory();

    // テスト用の差し替え。空で既定へ戻ります。
    void SetCacheDirectoryOverride(std::filesystem::path directory);

    // 鍵作りに使うFNV-1a 64（texture-cache等と同じもの）。
    [[nodiscard]] std::uint64_t HashBytes(
        std::span<const std::uint8_t> bytes) noexcept;

    // 保存。GPUのキューブ2枚をCPUへ読み戻すので、ベイク直後や
    // 畳み込み直後（どうせGPUを待つ場面）で呼びます。失敗しても
    // 何も起きません。
    void Store(
        std::uint64_t key,
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        const EnvironmentRenderer::OwnedPrefilteredEnvironment&
            environment) noexcept;

    // 読み込み。無い・壊れているときは IsValid() が偽の値を返します
    // （呼ぶ側は普通にベイク／畳み込みをすればよい）。
    [[nodiscard]]
    EnvironmentRenderer::OwnedPrefilteredEnvironment TryLoad(
        ID3D11Device* device,
        std::uint64_t key);
}
