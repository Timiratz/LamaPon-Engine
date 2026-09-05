#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace LamaPon::UiRecorder
{
    // 1フレームに描かれたImGuiウィジェット1個の記録です。
    //
    // ウィンドウ、ラベル、位置を記録し、自動化クライアントが
    // ウィジェットをラベルで指定できるようにします。
    struct Item final
    {
        // 属するウィンドウ名（ImGuiのウィンドウ名。"##"以降も含む）。
        std::string window;
        // 表示ラベル（ItemInfoフックが報告したもの）。
        std::string label;
        // クライアント座標の矩形（＝スクリーンショットのピクセル）。
        float x{};
        float y{};
        float width{};
        float height{};
        // ImGuiItemStatusFlags（チェック状態など）。
        std::uint32_t statusFlags{};
    };

    // 記録の有効化。ImGuiのTestEngineHookItemsを立てます。
    // リモート操作モード（--remote）だけが使います。記録には
    // フレームごとのコストがあるため、通常編集では切っておきます。
    void SetEnabled(bool enabled);

    // フレームの頭で呼びます。前のフレームの記録を「確定分」として
    // 公開し、記録先を切り替えます。
    void NextFrame();

    // 確定分（直前の完成フレーム）のアイテム一覧。
    // includeUnlabeledをtrueにすると、ラベルを報告しないウィジェット
    // （DragFloat3の各軸など）も矩形付きで返します。自動化クライアント
    // はスクリーンショットと照合し、座標指定で操作できます。
    [[nodiscard]] std::vector<Item> Snapshot(
        bool includeUnlabeled = false);
}
