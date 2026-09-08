#include "LamaPon/Editor/EditorGuiRenderer.h"

#include "LamaPon/Editor/D3D11EditorGuiRenderer.h"

#include <stdexcept>

namespace LamaPon
{
    std::unique_ptr<EditorGuiRenderer>
        CreateEditorGuiRenderer(const RenderingApi activeApi)
    {
        // TODO: D3D12用のDear ImGui renderer実装後は、ここで
        // 実効APIに応じた具象rendererを生成します。
        if (activeApi == RenderingApi::DirectX11)
        {
            return std::make_unique<D3D11EditorGuiRenderer>();
        }
        throw std::logic_error(
            "The requested editor GUI renderer is not implemented.");
    }
}
