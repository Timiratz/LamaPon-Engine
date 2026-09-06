#include "LamaPon/Editor/EditorExtensionRegistry.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
    void Require(const bool condition, const char* const message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }
}

int main()
{
    int result{};
    try
    {
        LamaPon::EditorExtensionRegistry registry;
        int attachCount{};
        int updateCount{};
        int drawCount{};
        int shutdownCount{};

        // 拡張DLLなどが設定読み込みより後に現れても、保存済みの
        // 表示状態が登録時に適用されることを確認します。
        registry.RestorePanelVisibility(
            "example.inspector",
            true);

        LamaPon::EditorExtensionDefinition extension;
        extension.id = "example.tools";
        extension.displayName = "Example Tools";
        extension.panels.push_back({
            "example.inspector",
            "Example Inspector",
            false,
            true,
            [&](bool& open)
            {
                Require(open, "Closed panels must not be drawn.");
                ++drawCount;
            }
        });
        extension.onAttach = [&] { ++attachCount; };
        extension.onUpdate = [&] { ++updateCount; };
        extension.onShutdown = [&] { ++shutdownCount; };

        std::string error;
        Require(
            registry.Register(std::move(extension), &error),
            "A valid extension must register.");
        Require(error.empty() && attachCount == 1,
            "Registration must attach the extension once.");
        Require(registry.Extensions().size() == 1
                && registry.Panels().size() == 1,
            "Extension and panel records must be discoverable.");
        Require(registry.IsPanelOpen("example.inspector"),
            "Late registrations must consume saved visibility by id.");

        registry.DrawPanels();
        Require(drawCount == 1,
            "A restored open panel must be drawn.");
        Require(registry.SetPanelOpen("example.inspector", false),
            "Registered panel visibility must be writable by id.");
        registry.DrawPanels();
        Require(drawCount == 1,
            "Closed panel callbacks must be skipped.");
        Require(registry.SetPanelOpen("example.inspector", true),
            "Registered panel visibility must be writable by id.");
        registry.DrawPanels();
        registry.Update();
        Require(drawCount == 2 && updateCount == 1,
            "Open panels and extension updates must be dispatched.");

        registry.ResetPanelVisibility();
        Require(!registry.IsPanelOpen("example.inspector"),
            "Layout reset must restore each registered default.");

        LamaPon::EditorExtensionDefinition duplicatePanel;
        duplicatePanel.id = "example.duplicate";
        duplicatePanel.displayName = "Duplicate";
        duplicatePanel.panels.push_back({
            "example.inspector",
            "Conflicting Inspector",
            true,
            true,
            [](bool&) {}
        });
        Require(!registry.Register(std::move(duplicatePanel), &error)
                && !error.empty(),
            "Duplicate panel ids must be rejected atomically.");
        Require(registry.Extensions().size() == 1
                && registry.Panels().size() == 1,
            "A rejected extension must not leave partial records.");

        Require(registry.Unregister("example.tools"),
            "Registered extensions must be removable by id.");
        Require(shutdownCount == 1
                && registry.Extensions().empty()
                && registry.Panels().empty(),
            "Unregister must shut down and remove owned panels.");
        Require(!registry.Unregister("example.tools"),
            "Removing an unknown extension must report failure.");

        std::cout << "Editor extension registry tests passed.\n";
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        result = 1;
    }
    return result;
}
