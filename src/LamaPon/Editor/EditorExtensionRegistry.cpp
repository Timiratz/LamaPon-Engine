#include "LamaPon/Editor/EditorExtensionRegistry.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <unordered_set>
#include <utility>

namespace
{
    // 出力先があれば登録失敗の理由を移す(destination: 失敗理由の出力先・省略可, message: 失敗理由の所有先)。
    void SetRegistrationError(
        std::string* const destination,
        std::string message)
    {
        if (destination != nullptr)
        {
            *destination = std::move(message);
        }
    }
}

namespace LamaPon
{
    EditorExtensionRegistry::~EditorExtensionRegistry()
    {
        Shutdown();
    }

    bool EditorExtensionRegistry::Register(
        EditorExtensionDefinition definition,
        std::string* const error)
    {
        if (definition.id.empty() || definition.displayName.empty())
        {
            SetRegistrationError(
                error,
                "Extension id and display name must not be empty.");
            return false;
        }
        // 同じ拡張IDの重複登録を拒否する(extension: 登録済みの拡張)。
        if (std::ranges::any_of(
                m_extensions,
                [&](const RegisteredEditorExtension& extension)
                {
                    return extension.id == definition.id;
                }))
        {
            SetRegistrationError(
                error,
                "Extension id is already registered: "
                    + definition.id);
            return false;
        }

        // 同じ登録要求内のパネルID集合
        std::unordered_set<std::string> pendingPanelIds;
        // 検証・追加・操作するパネル
        for (const auto& panel : definition.panels)
        {
            if (panel.id.empty()
                || panel.displayName.empty()
                || !panel.draw)
            {
                SetRegistrationError(
                    error,
                    "Panel id, display name, and draw callback are required.");
                return false;
            }
            if (!pendingPanelIds.insert(panel.id).second
                || FindPanel(panel.id) != nullptr)
            {
                SetRegistrationError(
                    error,
                    "Panel id is already registered: " + panel.id);
                return false;
            }
        }

        // 今回の追加パネルの開始位置
        const auto panelStart = m_panels.size();
        // 検証・追加・操作するパネル
        for (auto& panel : definition.panels)
        {
            // 復元状態を反映する初回表示状態
            bool open = panel.defaultOpen;
            // 復元待ちのパネル表示状態
            if (const auto saved =
                    m_pendingPanelVisibility.find(panel.id);
                saved != m_pendingPanelVisibility.end())
            {
                open = saved->second;
                m_pendingPanelVisibility.erase(saved);
            }
            m_panels.push_back(RegisteredEditorPanel{
                definition.id,
                std::move(panel.id),
                std::move(panel.displayName),
                panel.defaultOpen,
                open,
                panel.showInWindowMenu,
                std::move(panel.draw),
                std::move(panel.windowMenuGroup)
            });
        }
        m_extensions.push_back(RegisteredEditorExtension{
            std::move(definition.id),
            std::move(definition.displayName),
            std::move(definition.onUpdate),
            std::move(definition.drawMenu),
            std::move(definition.onShutdown),
            definition.menuInline
        });

        // 追加パネルの表示状態を退避して登録を戻す
        const auto rollbackRegistration = [&]
        {
            // 検証・追加・操作するパネル
            for (auto panel = m_panels.begin()
                    + static_cast<std::ptrdiff_t>(panelStart);
                panel != m_panels.end();
                ++panel)
            {
                m_pendingPanelVisibility[panel->id] = panel->open;
            }
            m_panels.erase(
                m_panels.begin()
                    + static_cast<std::ptrdiff_t>(panelStart),
                m_panels.end());
            m_extensions.pop_back();
        };

        try
        {
            if (definition.onAttach)
            {
                definition.onAttach();
            }
        }
        // 拡張の初期化処理が通知した失敗
        catch (const std::exception& exception)
        {
            rollbackRegistration();
            SetRegistrationError(error, exception.what());
            return false;
        }
        catch (...)
        {
            rollbackRegistration();
            SetRegistrationError(
                error,
                "Extension attach callback failed.");
            return false;
        }

        if (error != nullptr)
        {
            error->clear();
        }
        return true;
    }

    bool EditorExtensionRegistry::Unregister(
        const std::string_view extensionId) noexcept
    {
        // 指定IDの登録を探す(candidate: 登録済みの拡張)。
        const auto extension = std::ranges::find_if(
            m_extensions,
            [&](const RegisteredEditorExtension& candidate)
            {
                return candidate.id == extensionId;
            });
        if (extension == m_extensions.end())
        {
            return false;
        }
        if (extension->onShutdown)
        {
            try
            {
                extension->onShutdown();
            }
            catch (...)
            {
            }
        }
        // 検証・追加・操作するパネル
        for (const auto& panel : m_panels)
        {
            if (panel.extensionId == extensionId)
            {
                m_pendingPanelVisibility[panel.id] = panel.open;
            }
        }
        // 指定拡張の全パネルを取り除く(panel: 登録済みのパネル)。
        std::erase_if(
            m_panels,
            [&](const RegisteredEditorPanel& panel)
            {
                return panel.extensionId == extensionId;
            });
        m_extensions.erase(extension);
        return true;
    }

    void EditorExtensionRegistry::Shutdown() noexcept
    {
        // 検査・解除・更新する拡張
        for (auto extension = m_extensions.rbegin();
            extension != m_extensions.rend();
            ++extension)
        {
            if (!extension->onShutdown)
            {
                continue;
            }
            try
            {
                extension->onShutdown();
            }
            catch (...)
            {
            }
        }
        m_panels.clear();
        m_extensions.clear();
        m_pendingPanelVisibility.clear();
    }

    void EditorExtensionRegistry::Update()
    {
        // 検査・解除・更新する拡張
        for (const auto& extension : m_extensions)
        {
            if (extension.onUpdate)
            {
                extension.onUpdate();
            }
        }
    }

    void EditorExtensionRegistry::DrawPanels()
    {
        // 検証・追加・操作するパネル
        for (auto& panel : m_panels)
        {
            if (panel.open)
            {
                panel.draw(panel.open);
            }
        }
    }

    void EditorExtensionRegistry::ResetPanelVisibility() noexcept
    {
        m_pendingPanelVisibility.clear();
        // 検証・追加・操作するパネル
        for (auto& panel : m_panels)
        {
            panel.open = panel.defaultOpen;
        }
    }

    RegisteredEditorPanel* EditorExtensionRegistry::FindPanel(
        const std::string_view panelId) noexcept
    {
        // 指定IDのパネルを探す(candidate: 登録済みのパネル)。
        const auto panel = std::ranges::find_if(
            m_panels,
            [&](const RegisteredEditorPanel& candidate)
            {
                return candidate.id == panelId;
            });
        return panel == m_panels.end() ? nullptr : &*panel;
    }

    const RegisteredEditorPanel* EditorExtensionRegistry::FindPanel(
        const std::string_view panelId) const noexcept
    {
        // 指定IDのパネルを探す(candidate: 登録済みのパネル)。
        const auto panel = std::ranges::find_if(
            m_panels,
            [&](const RegisteredEditorPanel& candidate)
            {
                return candidate.id == panelId;
            });
        return panel == m_panels.end() ? nullptr : &*panel;
    }

    bool EditorExtensionRegistry::SetPanelOpen(
        const std::string_view panelId,
        const bool open) noexcept
    {
        // 検証・追加・操作するパネル
        auto* const panel = FindPanel(panelId);
        if (panel == nullptr)
        {
            return false;
        }
        panel->open = open;
        return true;
    }

    void EditorExtensionRegistry::RestorePanelVisibility(
        const std::string_view panelId,
        const bool open)
    {
        // 検証・追加・操作するパネル
        if (auto* const panel = FindPanel(panelId))
        {
            panel->open = open;
            return;
        }
        if (!panelId.empty())
        {
            m_pendingPanelVisibility[std::string{ panelId }] = open;
        }
    }

    bool EditorExtensionRegistry::IsPanelOpen(
        const std::string_view panelId) const noexcept
    {
        // 検証・追加・操作するパネル
        const auto* const panel = FindPanel(panelId);
        return panel != nullptr && panel->open;
    }
}
