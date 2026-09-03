#include "LamaPon/Editor/Editor.h"

#include "LamaPon/Core/Application.h"
#include "LamaPon/Editor/EditorLayer.h"

#include <memory>
#include <utility>

namespace LamaPon
{
    void EnableEditor(
        Application& application,
        std::filesystem::path scenePath,
        std::filesystem::path engineRoot,
        std::string buildConfiguration,
        const bool safeMode,
        const EditorScreenshotOptions* screenshot)
    {
        auto layer = std::make_unique<EditorLayer>(
            application.WindowHandle(),
            application.Graphics(),
            application.ActiveScene(),
            application.Preferences(),
            application.Saves(),
            std::move(scenePath),
            std::move(engineRoot),
            std::move(buildConfiguration));
        layer->SetSafeMode(safeMode);
        if (screenshot != nullptr
            && (!screenshot->imagePath.empty()
                || !screenshot->remoteDirectory.empty()))
        {
            layer->SetScreenshotRequest(*screenshot);
        }
        application.AttachLayer(std::move(layer));
    }

    bool WasNormalModeRestartRequested() noexcept
    {
        return EditorLayer::NormalModeRestartRequested();
    }
}
