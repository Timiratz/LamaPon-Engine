#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>

namespace LamaPon
{
    // ViewがGPU資源をどの用途で参照するかを表します。値は描画APIの
    // descriptor種別とは独立しており、具象Backendがnative viewへ
    // 解決するときの型検証に使います。
    enum class GraphicsViewKind : std::uint8_t
    {
        None,
        ShaderResource,
        RenderTarget,
        DepthStencil,
        UnorderedAccess
    };

    namespace Detail
    {
        class GraphicsResourceDomain;
        class GraphicsTexturePayload;
        class GraphicsBufferPayload;
        class GraphicsViewPayload;
        class GraphicsResourceHandleAccess;
    }

    // API固有のtexture実体を強所有する不透明handleです。copyは同じ
    // 実体を共有し、default構築・move後・Reset後はemptyになります。
    class GraphicsTextureHandle final
    {
    public:
        GraphicsTextureHandle() noexcept = default;
        GraphicsTextureHandle(
            const GraphicsTextureHandle&) noexcept = default;
        GraphicsTextureHandle(
            GraphicsTextureHandle&&) noexcept = default;
        GraphicsTextureHandle& operator=(
            const GraphicsTextureHandle&) noexcept = default;
        GraphicsTextureHandle& operator=(
            GraphicsTextureHandle&&) noexcept = default;
        ~GraphicsTextureHandle() noexcept = default;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return m_payload != nullptr;
        }

        void Reset() noexcept
        {
            m_payload.reset();
        }

        friend bool operator==(
            const GraphicsTextureHandle& left,
            const GraphicsTextureHandle& right) noexcept
        {
            return left.m_payload == right.m_payload;
        }

        friend bool operator==(
            const GraphicsTextureHandle& handle,
            std::nullptr_t) noexcept
        {
            return handle.m_payload == nullptr;
        }

    private:
        explicit GraphicsTextureHandle(
            std::shared_ptr<Detail::GraphicsTexturePayload>
                payload) noexcept
            : m_payload(std::move(payload))
        {
        }

        std::shared_ptr<Detail::GraphicsTexturePayload>
            m_payload;

        friend class Detail::GraphicsViewPayload;
        friend class Detail::GraphicsResourceHandleAccess;
    };

    // API固有のbuffer実体を強所有する不透明handleです。
    class GraphicsBufferHandle final
    {
    public:
        GraphicsBufferHandle() noexcept = default;
        GraphicsBufferHandle(
            const GraphicsBufferHandle&) noexcept = default;
        GraphicsBufferHandle(
            GraphicsBufferHandle&&) noexcept = default;
        GraphicsBufferHandle& operator=(
            const GraphicsBufferHandle&) noexcept = default;
        GraphicsBufferHandle& operator=(
            GraphicsBufferHandle&&) noexcept = default;
        ~GraphicsBufferHandle() noexcept = default;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return m_payload != nullptr;
        }

        void Reset() noexcept
        {
            m_payload.reset();
        }

        friend bool operator==(
            const GraphicsBufferHandle& left,
            const GraphicsBufferHandle& right) noexcept
        {
            return left.m_payload == right.m_payload;
        }

        friend bool operator==(
            const GraphicsBufferHandle& handle,
            std::nullptr_t) noexcept
        {
            return handle.m_payload == nullptr;
        }

    private:
        explicit GraphicsBufferHandle(
            std::shared_ptr<Detail::GraphicsBufferPayload>
                payload) noexcept
            : m_payload(std::move(payload))
        {
        }

        std::shared_ptr<Detail::GraphicsBufferPayload>
            m_payload;

        friend class Detail::GraphicsViewPayload;
        friend class Detail::GraphicsResourceHandleAccess;
    };

    namespace Detail
    {
        // Backendの初期化世代を表すidentity兼lifetime anchorです。
        // 具象BackendはAPI固有のdomainを派生させ、deviceやdescriptor
        // allocatorなど、handleの遅延破棄に必要な状態をここへ保持できます。
        // Initializeのたびに新しいdomainを作り、native解決時は同じdomain
        // から作られたpayloadだけを受け付けてください。最後のhandleは任意の
        // threadで破棄され得ます。D3D12実装ではGPU fence完了前のresourceや
        // descriptorを直接解放せず、domain所有の遅延破棄queueへ退避します。
        class GraphicsResourceDomain
        {
        public:
            virtual ~GraphicsResourceDomain() noexcept = default;

            GraphicsResourceDomain(
                const GraphicsResourceDomain&) = delete;
            GraphicsResourceDomain& operator=(
                const GraphicsResourceDomain&) = delete;

        protected:
            GraphicsResourceDomain() noexcept = default;
        };

        // Backend固有payloadの共通基底です。domainを強所有するため、
        // Backend facadeのShutdown後に外部のhandleが最後の参照になっても
        // API固有の解放処理に必要なlifetime anchorは失われません。
        class GraphicsResourcePayload
        {
        public:
            virtual ~GraphicsResourcePayload() noexcept = default;

            GraphicsResourcePayload(
                const GraphicsResourcePayload&) = delete;
            GraphicsResourcePayload& operator=(
                const GraphicsResourcePayload&) = delete;

            [[nodiscard]] const std::shared_ptr<
                GraphicsResourceDomain>& Domain() const noexcept
            {
                return m_domain;
            }

        protected:
            explicit GraphicsResourcePayload(
                std::shared_ptr<GraphicsResourceDomain> domain)
                : m_domain(std::move(domain))
            {
                if (m_domain == nullptr)
                {
                    throw std::invalid_argument(
                        "A graphics resource payload requires a domain.");
                }
            }

        private:
            std::shared_ptr<GraphicsResourceDomain> m_domain;
        };

        class GraphicsTexturePayload
            : public GraphicsResourcePayload
        {
        public:
            ~GraphicsTexturePayload() noexcept override = default;

        protected:
            using GraphicsResourcePayload::GraphicsResourcePayload;
        };

        class GraphicsBufferPayload
            : public GraphicsResourcePayload
        {
        public:
            ~GraphicsBufferPayload() noexcept override = default;

        protected:
            using GraphicsResourcePayload::GraphicsResourcePayload;
        };

        // Viewはdescriptor/native viewに加えて、その参照先resourceを
        // 強所有します。D3D11のCOM viewが暗黙にresourceを保持する挙動へ
        // 依存せず、resourceを所有しないD3D12 descriptorでも同じ寿命契約に
        // なります。
        class GraphicsViewPayload
            : public GraphicsResourcePayload
        {
        public:
            ~GraphicsViewPayload() noexcept override = default;

            [[nodiscard]] GraphicsViewKind Kind() const noexcept
            {
                return m_kind;
            }

        protected:
            GraphicsViewPayload(
                std::shared_ptr<GraphicsResourceDomain> domain,
                const GraphicsViewKind kind,
                GraphicsTextureHandle resource)
                : GraphicsResourcePayload(std::move(domain))
                , m_kind(kind)
                , m_resource(std::move(resource))
            {
                ValidateTextureResource();
            }

            GraphicsViewPayload(
                std::shared_ptr<GraphicsResourceDomain> domain,
                const GraphicsViewKind kind,
                GraphicsBufferHandle resource)
                : GraphicsResourcePayload(std::move(domain))
                , m_kind(kind)
                , m_resource(std::move(resource))
            {
                ValidateBufferResource();
            }

        private:
            void ValidateKind() const
            {
                if (m_kind == GraphicsViewKind::None)
                {
                    throw std::invalid_argument(
                        "A graphics view payload requires a view kind.");
                }
            }

            void ValidateTextureResource() const
            {
                ValidateKind();
                const auto& resource =
                    std::get<GraphicsTextureHandle>(m_resource);
                if (resource.m_payload == nullptr)
                {
                    throw std::invalid_argument(
                        "A graphics view payload requires a resource.");
                }
                if (resource.m_payload->Domain().get()
                    != Domain().get())
                {
                    throw std::invalid_argument(
                        "A graphics view and its resource must share a domain.");
                }
            }

            void ValidateBufferResource() const
            {
                ValidateKind();
                const auto& resource =
                    std::get<GraphicsBufferHandle>(m_resource);
                if (resource.m_payload == nullptr)
                {
                    throw std::invalid_argument(
                        "A graphics view payload requires a resource.");
                }
                if (resource.m_payload->Domain().get()
                    != Domain().get())
                {
                    throw std::invalid_argument(
                        "A graphics view and its resource must share a domain.");
                }
            }

            GraphicsViewKind m_kind{ GraphicsViewKind::None };
            std::variant<
                GraphicsTextureHandle,
                GraphicsBufferHandle> m_resource;

            friend class GraphicsResourceHandleAccess;
        };
    }

    // API固有のview/descriptorと参照先resourceを強所有する不透明handle
    // です。Kind()はempty handleに対してNoneを返します。
    class GraphicsViewHandle final
    {
    public:
        GraphicsViewHandle() noexcept = default;
        GraphicsViewHandle(
            const GraphicsViewHandle&) noexcept = default;
        GraphicsViewHandle(
            GraphicsViewHandle&&) noexcept = default;
        GraphicsViewHandle& operator=(
            const GraphicsViewHandle&) noexcept = default;
        GraphicsViewHandle& operator=(
            GraphicsViewHandle&&) noexcept = default;
        ~GraphicsViewHandle() noexcept = default;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return m_payload != nullptr;
        }

        [[nodiscard]] GraphicsViewKind Kind() const noexcept
        {
            return m_payload != nullptr
                ? m_payload->Kind()
                : GraphicsViewKind::None;
        }

        void Reset() noexcept
        {
            m_payload.reset();
        }

        friend bool operator==(
            const GraphicsViewHandle& left,
            const GraphicsViewHandle& right) noexcept
        {
            return left.m_payload == right.m_payload;
        }

        friend bool operator==(
            const GraphicsViewHandle& handle,
            std::nullptr_t) noexcept
        {
            return handle.m_payload == nullptr;
        }

    private:
        explicit GraphicsViewHandle(
            std::shared_ptr<Detail::GraphicsViewPayload>
                payload) noexcept
            : m_payload(std::move(payload))
        {
        }

        std::shared_ptr<Detail::GraphicsViewPayload> m_payload;

        friend class Detail::GraphicsResourceHandleAccess;
    };

    namespace Detail
    {
        // Backend実装だけがpayloadと公開handleの境界を越えるための
        // access pointです。共通の利用側はhandleのempty/identity/Kind
        // 以外を観測できません。
        class GraphicsResourceHandleAccess final
        {
        public:
            [[nodiscard]] static GraphicsTextureHandle MakeTexture(
                std::shared_ptr<GraphicsTexturePayload>
                    payload) noexcept
            {
                return GraphicsTextureHandle{ std::move(payload) };
            }

            [[nodiscard]] static GraphicsBufferHandle MakeBuffer(
                std::shared_ptr<GraphicsBufferPayload>
                    payload) noexcept
            {
                return GraphicsBufferHandle{ std::move(payload) };
            }

            [[nodiscard]] static GraphicsViewHandle MakeView(
                std::shared_ptr<GraphicsViewPayload>
                    payload) noexcept
            {
                return GraphicsViewHandle{ std::move(payload) };
            }

            [[nodiscard]] static const GraphicsTexturePayload* Payload(
                const GraphicsTextureHandle& handle) noexcept
            {
                return handle.m_payload.get();
            }

            [[nodiscard]] static const GraphicsBufferPayload* Payload(
                const GraphicsBufferHandle& handle) noexcept
            {
                return handle.m_payload.get();
            }

            [[nodiscard]] static const GraphicsViewPayload* Payload(
                const GraphicsViewHandle& handle) noexcept
            {
                return handle.m_payload.get();
            }

            [[nodiscard]] static const GraphicsResourceDomain* Domain(
                const GraphicsTextureHandle& handle) noexcept
            {
                return handle.m_payload != nullptr
                    ? handle.m_payload->Domain().get()
                    : nullptr;
            }

            [[nodiscard]] static const GraphicsResourceDomain* Domain(
                const GraphicsBufferHandle& handle) noexcept
            {
                return handle.m_payload != nullptr
                    ? handle.m_payload->Domain().get()
                    : nullptr;
            }

            [[nodiscard]] static const GraphicsResourceDomain* Domain(
                const GraphicsViewHandle& handle) noexcept
            {
                return handle.m_payload != nullptr
                    ? handle.m_payload->Domain().get()
                    : nullptr;
            }

            [[nodiscard]] static const GraphicsTextureHandle*
                TextureResource(
                    const GraphicsViewHandle& handle) noexcept
            {
                if (handle.m_payload == nullptr)
                {
                    return nullptr;
                }
                return std::get_if<GraphicsTextureHandle>(
                    &handle.m_payload->m_resource);
            }

            [[nodiscard]] static const GraphicsBufferHandle*
                BufferResource(
                    const GraphicsViewHandle& handle) noexcept
            {
                if (handle.m_payload == nullptr)
                {
                    return nullptr;
                }
                return std::get_if<GraphicsBufferHandle>(
                    &handle.m_payload->m_resource);
            }
        };
    }
}
