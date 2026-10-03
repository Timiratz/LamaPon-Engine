#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace LamaPon::UiRecorder
{
    // 直前のフレームのウィジェット名・矩形・状態を記録してリモート操作から参照する。
    struct Item final
    {
        // 所属ウィンドウ名・内部IDも含む
        std::string window;
        // hookが通知したウィジェット名
        std::string label;
        // クライアント座標の左端ピクセル
        float x{};
        // クライアント座標の上端ピクセル
        float y{};
        // 矩形の幅・ピクセル
        float width{};
        // 矩形の高さ・ピクセル
        float height{};
        // ImGuiの項目状態フラグ
        std::uint32_t statusFlags{};
    };

    // リモート操作用の記録を切り替え無効化時は全記録を消す(enabled: 項目の記録を有効にするか)。
    void SetEnabled(bool enabled);

    // フレームの先頭で前の記録を公開して新しい記録を始める。
    void NextFrame();

    // 直前の完成フレームの項目一覧をコピーして返す(includeUnlabeled: ラベルのない矩形も含めるか)。
    [[nodiscard]] std::vector<Item> Snapshot(
        bool includeUnlabeled = false);
}
