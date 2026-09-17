#include "LamaPon/Editor/EditorGuiRenderer.h"

#include "LamaPon/Editor/D3D11EditorGuiRenderer.h"
#include "LamaPon/Editor/D3D12EditorGuiRenderer.h"

#include <stdexcept>

namespace LamaPon
{
    std::unique_ptr<EditorGuiRenderer>
        CreateEditorGuiRenderer(const RenderingApi activeApi)
    {
        if (activeApi == RenderingApi::DirectX11)
        {
            return std::make_unique<D3D11EditorGuiRenderer>();
        }
        if (activeApi == RenderingApi::DirectX12Experimental)
        {
            return std::make_unique<D3D12EditorGuiRenderer>();
        }
        throw std::logic_error(
            "The requested editor GUI renderer is not implemented.");
    }
}
