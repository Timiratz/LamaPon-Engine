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
    // リフレクションプローブとSkyboxの決定的な畳み込み結果を保存し、
    // 同じ入力に対するGPUでの再計算を省略します。1組はスペキュラ8ミップと
    // 放射照度のfp16キューブ2枚で構成されます。
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

    // GPUのキューブ2枚をCPUへ読み戻して保存します。GPUとの同期が
    // 必要なため、ベイクや畳み込みの直後に呼びます。失敗時は保存を
    // 行いません。
    void Store(
        std::uint64_t key,
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        const EnvironmentRenderer::OwnedPrefilteredEnvironment&
            environment) noexcept;

    // キャッシュが無い、または破損している場合はIsValid()が偽の値を返し、
    // 呼び出し側でベイクまたは畳み込みを実行します。
    [[nodiscard]]
    EnvironmentRenderer::OwnedPrefilteredEnvironment TryLoad(
        ID3D11Device* device,
        std::uint64_t key);
}
