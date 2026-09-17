#include "LamaPon/Editor/EditorModelPreviewRenderer.h"

#include "LamaPon/Editor/D3D11EditorModelPreviewRenderer.h"
#include "LamaPon/Editor/D3D12EditorModelPreviewRenderer.h"

#include <stdexcept>

namespace LamaPon
{
    std::unique_ptr<EditorModelPreviewRenderer>
        CreateEditorModelPreviewRenderer(
            const RenderingApi activeApi,
            GraphicsDevice& graphics)
    {
        if (activeApi == RenderingApi::DirectX11)
        {
            return std::make_unique<
                D3D11EditorModelPreviewRenderer>(graphics);
        }
        if (activeApi == RenderingApi::DirectX12Experimental)
        {
            return std::make_unique<
                D3D12EditorModelPreviewRenderer>(graphics);
        }
        throw std::logic_error(
            "The requested editor model preview renderer is not implemented.");
    }
}
