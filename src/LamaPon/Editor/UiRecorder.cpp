#include "LamaPon/Editor/UiRecorder.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdarg>
#include <unordered_map>
#include <utility>

namespace
{
    // ウィジェット記録が有効か
    bool g_enabled{};

    // 描画中フレームの項目の記録先
    std::vector<LamaPon::UiRecorder::Item> g_building;
    // 記録中のImGui IDと配列位置の対応
    std::unordered_map<ImGuiID, std::size_t> g_buildingIndex;
    // 直前の完成フレームの公開記録
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
        // 返却する前フレームの項目一覧
        std::vector<Item> items;
        items.reserve(g_published.size());
        // 列挙・追加・更新する項目
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



// 可視範囲と重なる項目の矩形を記録する(ctx: 使用中のImGui context, id: 非0の項目ID, bb: クライアント座標の矩形, itemData: hook互換用・未使用)。
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
    // 列挙・追加・更新する項目
    LamaPon::UiRecorder::Item item;
    if (ctx->CurrentWindow != nullptr)
    {
        item.window = ctx->CurrentWindow->Name;
        // クリックできない完全な画面外の矩形をリモート操作へ渡さない。
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

// 記録済みの項目にラベルと状態を付ける(ctx: hook互換用・未使用, id: 記録済み項目のID, label: hookから通知されたラベル, flags: チェック状態等の項目フラグ)。
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
    // 項目IDに対応する記録位置
    const auto found = g_buildingIndex.find(id);
    if (found == g_buildingIndex.end())
    {
        return;
    }
    // 列挙・追加・更新する項目
    auto& item = g_building[found->second];
    item.label = label;
    item.statusFlags =
        static_cast<std::uint32_t>(flags);
}

// hookのログ要求を受け取り記録せずに終了する(ctx: hook互換用・未使用, format: hook互換用・未使用)。
void ImGuiTestEngineHook_Log(
    ImGuiContext* ctx,
    const char* format,
    ...)
{
    static_cast<void>(ctx);
    static_cast<void>(format);
}

// 記録中の項目ラベルを変更まで借用し不在ならnullを返す(ctx: hook互換用・未使用, id: 探す項目のID)。
const char* ImGuiTestEngine_FindItemDebugLabel(
    ImGuiContext* ctx,
    const ImGuiID id)
{
    static_cast<void>(ctx);
    // 項目IDに対応する記録位置
    if (const auto found = g_buildingIndex.find(id);
        found != g_buildingIndex.end())
    {
        return g_building[found->second].label.c_str();
    }
    return nullptr;
}
