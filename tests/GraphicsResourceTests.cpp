#include "LamaPon/Graphics/GraphicsResource.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace
{
    struct DestructionCounts final
    {
        int domains{};
        int textures{};
        int buffers{};
        int views{};
    };

    class TestDomain final
        : public LamaPon::Detail::GraphicsResourceDomain
    {
    public:
        explicit TestDomain(DestructionCounts& counts) noexcept
            : m_counts(counts)
        {
        }

        ~TestDomain() noexcept override
        {
            ++m_counts.domains;
        }

    private:
        DestructionCounts& m_counts;
    };

    class TestTexturePayload final
        : public LamaPon::Detail::GraphicsTexturePayload
    {
    public:
        TestTexturePayload(
            std::shared_ptr<
                LamaPon::Detail::GraphicsResourceDomain> domain,
            DestructionCounts& counts)
            : GraphicsTexturePayload(std::move(domain))
            , m_counts(counts)
        {
        }

        ~TestTexturePayload() noexcept override
        {
            ++m_counts.textures;
        }

    private:
        DestructionCounts& m_counts;
    };

    class TestBufferPayload final
        : public LamaPon::Detail::GraphicsBufferPayload
    {
    public:
        TestBufferPayload(
            std::shared_ptr<
                LamaPon::Detail::GraphicsResourceDomain> domain,
            DestructionCounts& counts)
            : GraphicsBufferPayload(std::move(domain))
            , m_counts(counts)
        {
        }

        ~TestBufferPayload() noexcept override
        {
            ++m_counts.buffers;
        }

    private:
        DestructionCounts& m_counts;
    };

    class TestViewPayload final
        : public LamaPon::Detail::GraphicsViewPayload
    {
    public:
        TestViewPayload(
            std::shared_ptr<
                LamaPon::Detail::GraphicsResourceDomain> domain,
            const LamaPon::GraphicsViewKind kind,
            LamaPon::GraphicsTextureHandle resource,
            DestructionCounts& counts)
            : GraphicsViewPayload(
                std::move(domain),
                kind,
                std::move(resource))
            , m_counts(counts)
        {
        }

        TestViewPayload(
            std::shared_ptr<
                LamaPon::Detail::GraphicsResourceDomain> domain,
            const LamaPon::GraphicsViewKind kind,
            LamaPon::GraphicsBufferHandle resource,
            DestructionCounts& counts)
            : GraphicsViewPayload(
                std::move(domain),
                kind,
                std::move(resource))
            , m_counts(counts)
        {
        }

        ~TestViewPayload() noexcept override
        {
            ++m_counts.views;
        }

    private:
        DestructionCounts& m_counts;
    };

    void Require(const bool condition, const char* const message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    template <typename Function>
    void RequireInvalidArgument(
        Function&& function,
        const char* const message)
    {
        try
        {
            std::forward<Function>(function)();
        }
        catch (const std::invalid_argument&)
        {
            return;
        }
        catch (...)
        {
        }
        throw std::runtime_error(message);
    }
}

static_assert(std::is_nothrow_default_constructible_v<
    LamaPon::GraphicsTextureHandle>);
static_assert(std::is_nothrow_copy_constructible_v<
    LamaPon::GraphicsTextureHandle>);
static_assert(std::is_nothrow_move_constructible_v<
    LamaPon::GraphicsTextureHandle>);
static_assert(std::is_nothrow_copy_assignable_v<
    LamaPon::GraphicsTextureHandle>);
static_assert(std::is_nothrow_move_assignable_v<
    LamaPon::GraphicsTextureHandle>);
static_assert(std::is_nothrow_destructible_v<
    LamaPon::GraphicsTextureHandle>);

static_assert(std::is_nothrow_default_constructible_v<
    LamaPon::GraphicsBufferHandle>);
static_assert(std::is_nothrow_copy_constructible_v<
    LamaPon::GraphicsBufferHandle>);
static_assert(std::is_nothrow_move_constructible_v<
    LamaPon::GraphicsBufferHandle>);
static_assert(std::is_nothrow_copy_assignable_v<
    LamaPon::GraphicsBufferHandle>);
static_assert(std::is_nothrow_move_assignable_v<
    LamaPon::GraphicsBufferHandle>);
static_assert(std::is_nothrow_destructible_v<
    LamaPon::GraphicsBufferHandle>);

static_assert(std::is_nothrow_default_constructible_v<
    LamaPon::GraphicsViewHandle>);
static_assert(std::is_nothrow_copy_constructible_v<
    LamaPon::GraphicsViewHandle>);
static_assert(std::is_nothrow_move_constructible_v<
    LamaPon::GraphicsViewHandle>);
static_assert(std::is_nothrow_copy_assignable_v<
    LamaPon::GraphicsViewHandle>);
static_assert(std::is_nothrow_move_assignable_v<
    LamaPon::GraphicsViewHandle>);
static_assert(std::is_nothrow_destructible_v<
    LamaPon::GraphicsViewHandle>);

static_assert(noexcept(
    std::declval<const LamaPon::GraphicsTextureHandle&>()
        == std::declval<const LamaPon::GraphicsTextureHandle&>()));
static_assert(noexcept(
    std::declval<const LamaPon::GraphicsTextureHandle&>()
        == nullptr));
static_assert(noexcept(
    std::declval<LamaPon::GraphicsTextureHandle&>().Reset()));
static_assert(noexcept(
    std::declval<const LamaPon::GraphicsBufferHandle&>()
        == std::declval<const LamaPon::GraphicsBufferHandle&>()));
static_assert(noexcept(
    std::declval<const LamaPon::GraphicsBufferHandle&>()
        == nullptr));
static_assert(noexcept(
    std::declval<LamaPon::GraphicsBufferHandle&>().Reset()));
static_assert(noexcept(
    std::declval<const LamaPon::GraphicsViewHandle&>()
        == std::declval<const LamaPon::GraphicsViewHandle&>()));
static_assert(noexcept(
    std::declval<const LamaPon::GraphicsViewHandle&>()
        == nullptr));
static_assert(noexcept(
    std::declval<const LamaPon::GraphicsViewHandle&>().Kind()));
static_assert(noexcept(
    std::declval<LamaPon::GraphicsViewHandle&>().Reset()));

int main()
{
    try
    {
        using LamaPon::Detail::GraphicsResourceHandleAccess;

        LamaPon::GraphicsTextureHandle emptyTexture;
        LamaPon::GraphicsBufferHandle emptyBuffer;
        LamaPon::GraphicsViewHandle emptyView;
        Require(!emptyTexture && emptyTexture == nullptr,
            "A default texture handle must be empty");
        Require(!emptyBuffer && emptyBuffer == nullptr,
            "A default buffer handle must be empty");
        Require(!emptyView && emptyView == nullptr
                && emptyView.Kind()
                    == LamaPon::GraphicsViewKind::None,
            "A default view handle must be empty and have no kind");
        Require(GraphicsResourceHandleAccess::Domain(emptyTexture)
                    == nullptr
                && GraphicsResourceHandleAccess::Domain(emptyBuffer)
                    == nullptr
                && GraphicsResourceHandleAccess::Domain(emptyView)
                    == nullptr,
            "Empty handles must not expose a resource domain");

        DestructionCounts counts;
        auto domain = std::make_shared<TestDomain>(counts);
        auto texturePayload =
            std::make_shared<TestTexturePayload>(domain, counts);
        auto texture = GraphicsResourceHandleAccess::MakeTexture(
            texturePayload);
        auto textureAlias = GraphicsResourceHandleAccess::MakeTexture(
            texturePayload);
        texturePayload.reset();
        Require(texture && texture == textureAlias,
            "Handles made from the same payload must share identity");

        auto textureCopy = texture;
        Require(textureCopy == texture,
            "Copying a texture handle must preserve identity");
        auto movedTexture = std::move(textureCopy);
        Require(movedTexture == texture && !textureCopy,
            "Moving a texture handle must transfer identity");
        textureAlias.Reset();

        auto viewPayload = std::make_shared<TestViewPayload>(
            domain,
            LamaPon::GraphicsViewKind::ShaderResource,
            texture,
            counts);
        auto view = GraphicsResourceHandleAccess::MakeView(
            std::move(viewPayload));
        auto viewCopy = view;
        Require(viewCopy == view
                && view.Kind()
                    == LamaPon::GraphicsViewKind::ShaderResource,
            "A copied view must preserve identity and kind");
        Require(
            GraphicsResourceHandleAccess::TextureResource(view)
                != nullptr
                && GraphicsResourceHandleAccess::BufferResource(view)
                    == nullptr,
            "A texture view must retain its texture resource type");

        texture.Reset();
        movedTexture.Reset();
        Require(counts.textures == 0,
            "A view must strongly own its texture resource");
        view.Reset();
        Require(counts.textures == 0 && counts.views == 0,
            "A copied view must share ownership of its payload");
        auto movedView = std::move(viewCopy);
        Require(!viewCopy
                && movedView.Kind()
                    == LamaPon::GraphicsViewKind::ShaderResource,
            "Moving a view must empty the source and preserve its kind");
        movedView.Reset();
        Require(counts.textures == 1 && counts.views == 1,
            "The last view must release its view and texture payloads");

        auto bufferPayload =
            std::make_shared<TestBufferPayload>(domain, counts);
        auto buffer = GraphicsResourceHandleAccess::MakeBuffer(
            std::move(bufferPayload));
        auto bufferCopy = buffer;
        Require(bufferCopy == buffer,
            "Copying a buffer handle must preserve identity");
        auto movedBuffer = std::move(bufferCopy);
        Require(movedBuffer == buffer && !bufferCopy,
            "Moving a buffer handle must transfer identity");
        auto bufferViewPayload = std::make_shared<TestViewPayload>(
            domain,
            LamaPon::GraphicsViewKind::UnorderedAccess,
            buffer,
            counts);
        auto bufferView = GraphicsResourceHandleAccess::MakeView(
            std::move(bufferViewPayload));
        Require(
            GraphicsResourceHandleAccess::TextureResource(bufferView)
                == nullptr
                && GraphicsResourceHandleAccess::BufferResource(bufferView)
                    != nullptr,
            "A buffer view must retain its buffer resource type");
        buffer.Reset();
        movedBuffer.Reset();
        Require(counts.buffers == 0,
            "A view must strongly own its buffer resource");
        bufferView.Reset();
        Require(counts.buffers == 1 && counts.views == 2,
            "The last buffer view must release its buffer payload");

        auto foreignDomain =
            std::make_shared<TestDomain>(counts);
        auto foreignTexture =
            GraphicsResourceHandleAccess::MakeTexture(
                std::make_shared<TestTexturePayload>(
                    foreignDomain,
                    counts));
        RequireInvalidArgument(
            [&]
            {
                static_cast<void>(
                    std::make_shared<TestViewPayload>(
                        domain,
                        LamaPon::GraphicsViewKind::ShaderResource,
                        foreignTexture,
                        counts));
            },
            "A view must reject a resource from another domain");
        RequireInvalidArgument(
            [&]
            {
                static_cast<void>(
                    std::make_shared<TestViewPayload>(
                        foreignDomain,
                        LamaPon::GraphicsViewKind::None,
                        foreignTexture,
                        counts));
            },
            "A non-empty view payload must reject the None kind");
        RequireInvalidArgument(
            [&]
            {
                static_cast<void>(
                    std::make_shared<TestViewPayload>(
                        domain,
                        LamaPon::GraphicsViewKind::ShaderResource,
                        LamaPon::GraphicsTextureHandle{},
                        counts));
            },
            "A view payload must reject an empty resource");

        foreignTexture.Reset();
        foreignDomain.reset();
        Require(counts.textures == 2 && counts.domains == 1,
            "A resource payload must strongly own its domain");
        domain.reset();
        Require(counts.domains == 2,
            "The last resource owner must release its domain");
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
