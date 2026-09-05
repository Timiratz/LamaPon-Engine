// ImGuiのテストエンジンフックの実装です。
//
// IMGUI_ENABLE_TEST_ENGINE を定義してビルドすると、ImGui本体が
// ウィジェットの登録時に ImGuiTestEngineHook_* を呼びます。ここでは
// 本家のテストエンジンの代わりに、可視ウィジェットの「ウィンドウ・
// ラベル・矩形・状態」を毎フレーム記録します。リモート操作モードの
// dump / click-label コマンドがこれを読みます。
//
// このファイルはDear ImGuiターゲットに入れます。フックのシンボルは
// imgui.obj から参照されるので、エディター側のライブラリに置くと
// imguiだけ使うターゲット（LamaPonHub）がリンクできなくなります。
#include "LamaPon/Editor/UiRecorder.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdarg>
#include <unordered_map>
#include <utility>

namespace
{
    bool g_enabled{};

    // 記録中のフレーム（ImGuiが描いている最中に積む）。
    std::vector<LamaPon::UiRecorder::Item> g_building;
    std::unordered_map<ImGuiID, std::size_t> g_buildingIndex;
    // 確定分（直前の完成フレーム）。Snapshotはこちらを返す。
    std::vector<LamaPon::UiRecorder::Item> g_published;
}

namespace LamaPon::UiRecorder
{
    void SetEnabled(const bool enabled)
    {
        g_enabled = enabled;
        if (ImGui::GetCurrentContext() != nullptr)
        {
            ImGui::GetCurrentContext()
                ->TestEngineHookItems = enabled;
        }
        if (!enabled)
        {
            g_building.clear();
            g_buildingIndex.clear();
            g_published.clear();
        }
    }

    void NextFrame()
    {
        if (!g_enabled)
        {
            return;
        }
        // フラグはコンテキスト再作成で消えることがあるので毎回立てる。
        if (ImGui::GetCurrentContext() != nullptr)
        {
            ImGui::GetCurrentContext()
                ->TestEngineHookItems = true;
        }
        g_published = std::move(g_building);
        g_building.clear();
        g_buildingIndex.clear();
    }

    std::vector<Item> Snapshot(
        const bool includeUnlabeled)
    {
        std::vector<Item> items;
        items.reserve(g_published.size());
        for (const auto& item : g_published)
        {
            if (!item.label.empty()
                || includeUnlabeled)
            {
                items.push_back(item);
            }
        }
        return items;
    }
}

// ImGuiが要求するフックの実体

void ImGuiTestEngineHook_ItemAdd(
    ImGuiContext* ctx,
    const ImGuiID id,
    const ImRect& bb,
    const ImGuiLastItemData* itemData)
{
    static_cast<void>(itemData);
    if (!g_enabled || ctx == nullptr || id == 0)
    {
        return;
    }
    LamaPon::UiRecorder::Item item;
    if (ctx->CurrentWindow != nullptr)
    {
        item.window = ctx->CurrentWindow->Name;
        // 完全に画面外（クリップ済み）のものは記録しません。
        // クリックできない座標をAIへ渡さないためです。
        if (!ctx->CurrentWindow->ClipRect.Overlaps(bb))
        {
            return;
        }
    }
    item.x = bb.Min.x;
    item.y = bb.Min.y;
    item.width = bb.GetWidth();
    item.height = bb.GetHeight();
    g_buildingIndex[id] = g_building.size();
    g_building.push_back(std::move(item));
}

void ImGuiTestEngineHook_ItemInfo(
    ImGuiContext* ctx,
    const ImGuiID id,
    const char* label,
    const ImGuiItemStatusFlags flags)
{
    static_cast<void>(ctx);
    if (!g_enabled || label == nullptr || id == 0)
    {
        return;
    }
    const auto found = g_buildingIndex.find(id);
    if (found == g_buildingIndex.end())
    {
        return;
    }
    auto& item = g_building[found->second];
    item.label = label;
    item.statusFlags =
        static_cast<std::uint32_t>(flags);
}

void ImGuiTestEngineHook_Log(
    ImGuiContext* ctx,
    const char* format,
    ...)
{
    static_cast<void>(ctx);
    static_cast<void>(format);
}

const char* ImGuiTestEngine_FindItemDebugLabel(
    ImGuiContext* ctx,
    const ImGuiID id)
{
    static_cast<void>(ctx);
    if (const auto found = g_buildingIndex.find(id);
        found != g_buildingIndex.end())
    {
        return g_building[found->second].label.c_str();
    }
    return nullptr;
}
