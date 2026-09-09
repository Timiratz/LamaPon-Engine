#include "LamaPon/Editor/EditorModelPreviewRenderer.h"

#include "LamaPon/Editor/D3D11EditorModelPreviewRenderer.h"

#include <stdexcept>

namespace LamaPon
{
    std::unique_ptr<EditorModelPreviewRenderer>
        CreateEditorModelPreviewRenderer(
            const RenderingApi activeApi,
            GraphicsDevice& graphics)
    {
        // TODO: D3D12用のモデルプレビューrenderer実装後は、ここで
        // 実効APIに応じた具象rendererを生成します。
        if (activeApi == RenderingApi::DirectX11)
        {
            return std::make_unique<
                D3D11EditorModelPreviewRenderer>(graphics);
        }
        throw std::logic_error(
            "The requested editor model preview renderer is not implemented.");
    }
}
