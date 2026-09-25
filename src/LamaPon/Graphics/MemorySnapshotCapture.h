#pragma once

#include "LamaPon/Core/MemorySnapshot.h"

namespace LamaPon
{
    class GraphicsDevice;

    // GraphicsDeviceが所有するアセット・音声・レンダーテクスチャと、
    // プロセス全体のメモリ量を1つのスナップショットへまとめます。
    // labelとcapturedAtは呼び出し側が設定します。メインスレッドから
    // 呼んでください。取得できない分類は空のまま続行します。
    [[nodiscard]] MemorySnapshot CaptureMemorySnapshot(
        GraphicsDevice& graphics);
}
