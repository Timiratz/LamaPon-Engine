#pragma once

#include <memory>

namespace LamaPon
{
    namespace Detail
    {
        struct GraphicsDeviceResourceLeaseState;
    }

    class GraphicsDevice;

    // Sceneや外部rendererがGraphicsDevice由来のresourceを保持している
    // 期間を表します。このleaseが1つでも生きている間は、Deviceを
    // 作り直すInitializeを安全側で拒否します。
    //
    // lease自身はBackend resourceを所有しません。GraphicsDeviceは
    // leaseより長く生存させてください。
    class GraphicsDeviceResourceLease final
    {
    public:
        GraphicsDeviceResourceLease() noexcept = default;
        ~GraphicsDeviceResourceLease() noexcept;

        GraphicsDeviceResourceLease(
            GraphicsDeviceResourceLease&& other) noexcept;
        GraphicsDeviceResourceLease& operator=(
            GraphicsDeviceResourceLease&& other) noexcept;

        GraphicsDeviceResourceLease(
            const GraphicsDeviceResourceLease&) = delete;
        GraphicsDeviceResourceLease& operator=(
            const GraphicsDeviceResourceLease&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return m_state != nullptr;
        }

        void Reset() noexcept;

    private:
        explicit GraphicsDeviceResourceLease(
            std::shared_ptr<
                Detail::GraphicsDeviceResourceLeaseState> state) noexcept;

        std::shared_ptr<
            Detail::GraphicsDeviceResourceLeaseState> m_state;

        friend class GraphicsDevice;
    };
}
