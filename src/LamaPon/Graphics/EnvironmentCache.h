#pragma once

#include "LamaPon/Graphics/EnvironmentRenderer.h"

#include <cstdint>
#include <filesystem>
#include <span>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace LamaPon::EnvironmentCache
{
    // 鏡面・拡散のRGBA16Fキューブを保存し、入力の同一性は呼出し側の識別子で表す。


    // キャッシュの配置先を返す。
    // 既定はLOCALAPPDATA配下のLamaPon/environment-cacheとし、取得不能なら一時領域を使う。
    [[nodiscard]] std::filesystem::path CacheDirectory();


    // 配置先を差し替え、空なら既定へ戻す(directory: 任意の配置先)。
    void SetCacheDirectoryOverride(std::filesystem::path directory);


    // バイト列のFNV-1a 64識別子を求める(bytes: 識別するバイト列)。
    [[nodiscard]] std::uint64_t HashBytes(
        std::span<const std::uint8_t> bytes) noexcept;


    // 二キューブをGPUから読み戻して保存し、失敗は無視する(key: キャッシュ識別子, device: 描画デバイス, context: 描画コンテキスト, environment: 保存する二キューブ)。
    // GPUとの同期で待機するためベイク直後に呼び、同じ識別子への並行保存は呼出し側で直列化する。
    void Store(
        std::uint64_t key,
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        const EnvironmentRenderer::OwnedPrefilteredEnvironment&
            environment) noexcept;


    // 規定形式の二キューブを復元し、失敗なら無効な組を返す(device: 描画デバイス, key: キャッシュ識別子)。
    [[nodiscard]]
    EnvironmentRenderer::OwnedPrefilteredEnvironment TryLoad(
        ID3D11Device* device,
        std::uint64_t key);
}
