#pragma once

#include "LamaPon/Core/MemorySnapshot.h"

namespace LamaPon
{
    class GraphicsDevice;

    // 描画デバイスの資源内訳とプロセスのメモリー量を収集する(graphics: 対象の描画デバイス)。
    // メインスレッドで呼び、labelとcapturedAtは呼出側で設定する。
    // 分類ごとの標準例外を捕捉して警告し、取得済みの情報を保持して次の分類へ進む。
    [[nodiscard]] MemorySnapshot CaptureMemorySnapshot(
        GraphicsDevice& graphics);
}
