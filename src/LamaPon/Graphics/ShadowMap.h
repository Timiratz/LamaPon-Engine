#pragma once

#include "LamaPon/Graphics/GraphicsResource.h"

#include <cstdint>
#include <memory>

namespace LamaPon
{
    namespace Detail
    {
        struct ShadowMapBackendAccess;
        struct ShadowMapBackendState;
    }

    // 描画APIに依存しない影mapの公開facadeです。native資源とbind状態は
    // Backend専用stateが所有し、この公開ヘッダーには具象API型を出しません。
    class ShadowMap final
    {
    public:
        ShadowMap() noexcept;
        ~ShadowMap() noexcept;

        ShadowMap(const ShadowMap&) = delete;
        ShadowMap& operator=(const ShadowMap&) = delete;
        ShadowMap(ShadowMap&&) = delete;
        ShadowMap& operator=(ShadowMap&&) = delete;

        [[nodiscard]] GraphicsViewHandle ViewHandle() const noexcept;
        [[nodiscard]] std::uint32_t Resolution() const noexcept;
        [[nodiscard]] std::uint32_t CascadeCount() const noexcept;
        [[nodiscard]] bool IsValid() const noexcept;

    private:
        friend struct Detail::ShadowMapBackendAccess;

        // API 55以前のGame ModuleがAPI不一致の案内まで到達できるよう、
        // 旧D3D11 member symbolの転送先だけをAPI-neutralな形で残します。
        // MSVC x64では旧関数と同じくthisをRCX、pointer戻り値をRAXで
        // 扱うため、GraphicsDeviceLegacyExports.defからaliasできます。
        [[nodiscard]] void* LegacyNativeView() const noexcept;

        // Backendはnative資源と公開handleを完成させた後、このpointerを
        // 一度に差し替えます。作成失敗時に旧世代を壊しません。
        std::unique_ptr<Detail::ShadowMapBackendState> m_backendState;
    };
}
