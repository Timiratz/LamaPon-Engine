#include "LamaPon/Graphics/EnvironmentCache.h"

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstring>
#include <cwchar>
#include <fstream>
#include <mutex>
#include <system_error>
#include <vector>

namespace
{
    // ファイル形式の版。畳み込みの計算やキューブの構成を変えたら
    // 上げます。
    constexpr std::uint32_t FormatVersion = 1;

    constexpr char Magic[4] = { 'T', 'E', 'N', 'V' };

    // fp16 RGBA固定（EnvironmentRendererの畳み込み出力と同じ）。
    constexpr DXGI_FORMAT CubeFormat =
        DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr std::uint32_t BytesPerPixel = 8;

    // 検査用の上限。畳み込み出力は128px・8ミップまでで、これを
    // 超える値が読めたらファイルが壊れています。
    constexpr std::uint32_t MaximumSize = 1024;
    constexpr std::uint32_t MaximumMips = 11;

    std::mutex g_directoryMutex;
    std::filesystem::path g_directoryOverride;

    [[nodiscard]] std::filesystem::path DefaultDirectory()
    {
        std::wstring localAppData(32768, L'\0');
        const DWORD length = GetEnvironmentVariableW(
            L"LOCALAPPDATA",
            localAppData.data(),
            static_cast<DWORD>(localAppData.size()));
        std::filesystem::path root;
        if (length > 0 && length < localAppData.size())
        {
            localAppData.resize(length);
            root = localAppData;
        }
        else
        {
            std::error_code error;
            root = std::filesystem::temp_directory_path(error);
            if (error)
            {
                return {};
            }
        }
        return root / L"LamaPon" / L"environment-cache";
    }

    [[nodiscard]] std::filesystem::path EntryPath(
        const std::uint64_t key)
    {
        const auto directory =
            LamaPon::EnvironmentCache::CacheDirectory();
        if (directory.empty())
        {
            return {};
        }
        wchar_t name[32]{};
        swprintf_s(name, L"%016llx.tenv", key);
        return directory / name;
    }

    void WriteFileAtomically(
        const std::filesystem::path& destination,
        const std::vector<std::uint8_t>& bytes)
    {
        auto temporary = destination;
        temporary += L".tmp";
        {
            std::ofstream output(
                temporary,
                std::ios::binary | std::ios::trunc);
            if (!output)
            {
                return;
            }
            output.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
            if (!output)
            {
                output.close();
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return;
            }
        }
        std::error_code error;
        std::filesystem::rename(temporary, destination, error);
        if (error)
        {
            std::filesystem::remove(temporary, error);
        }
    }

    // 1辺sizeでミップ数mipsのキューブの、詰めて並べたときの
    // 総バイト数。
    [[nodiscard]] std::size_t CubeByteCount(
        const std::uint32_t size,
        const std::uint32_t mips) noexcept
    {
        std::size_t total = 0;
        for (std::uint32_t mip = 0; mip < mips; ++mip)
        {
            const std::size_t edge =
                std::max<std::uint32_t>(size >> mip, 1);
            total += edge * edge * BytesPerPixel * 6;
        }
        return total;
    }

    // SRVの元テクスチャをSTAGINGへコピーし、連続したデータとして
    // 読み出します。失敗時はfalseを返します。
    [[nodiscard]] bool ReadCube(
        ID3D11Device* const device,
        ID3D11DeviceContext* const context,
        ID3D11ShaderResourceView* const view,
        std::uint32_t& size,
        std::uint32_t& mips,
        std::vector<std::uint8_t>& bytes)
    {
        if (view == nullptr)
        {
            return false;
        }
        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        view->GetResource(resource.ReleaseAndGetAddressOf());
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        if (FAILED(resource.As(&texture)))
        {
            return false;
        }
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        if (description.Format != CubeFormat
            || description.ArraySize != 6
            || description.Width != description.Height)
        {
            return false;
        }
        size = description.Width;
        mips = description.MipLevels;

        D3D11_TEXTURE2D_DESC staging = description;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging.MiscFlags = 0;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> copy;
        if (FAILED(device->CreateTexture2D(
            &staging,
            nullptr,
            copy.ReleaseAndGetAddressOf())))
        {
            return false;
        }
        context->CopyResource(copy.Get(), texture.Get());

        bytes.clear();
        bytes.reserve(CubeByteCount(size, mips));
        for (std::uint32_t face = 0; face < 6; ++face)
        {
            for (std::uint32_t mip = 0; mip < mips; ++mip)
            {
                const UINT subresource =
                    D3D11CalcSubresource(
                        mip,
                        face,
                        mips);
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (FAILED(context->Map(
                    copy.Get(),
                    subresource,
                    D3D11_MAP_READ,
                    0,
                    &mapped)))
                {
                    return false;
                }
                const std::uint32_t edge =
                    std::max<std::uint32_t>(size >> mip, 1);
                const std::size_t rowBytes =
                    static_cast<std::size_t>(edge)
                    * BytesPerPixel;
                const auto* source =
                    static_cast<const std::uint8_t*>(
                        mapped.pData);
                for (std::uint32_t row = 0;
                    row < edge;
                    ++row)
                {
                    bytes.insert(
                        bytes.end(),
                        source + row * mapped.RowPitch,
                        source + row * mapped.RowPitch
                            + rowBytes);
                }
                context->Unmap(copy.Get(), subresource);
            }
        }
        return true;
    }

    // 詰めたバイト列からIMMUTABLEなキューブとSRVを作ります。
    // 復元したキューブは読むだけなので、RENDER_TARGETは要りません。
    [[nodiscard]]
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
        CreateCube(
            ID3D11Device* const device,
            const std::uint32_t size,
            const std::uint32_t mips,
            const std::uint8_t* bytes)
    {
        std::vector<D3D11_SUBRESOURCE_DATA> initialData(
            static_cast<std::size_t>(mips) * 6);
        const std::uint8_t* cursor = bytes;
        // 詰め順はReadCubeと同じ（face外側・mip内側）。
        // D3D11のサブリソース番号は mip + face * mips です。
        for (std::uint32_t face = 0; face < 6; ++face)
        {
            for (std::uint32_t mip = 0; mip < mips; ++mip)
            {
                const std::uint32_t edge =
                    std::max<std::uint32_t>(size >> mip, 1);
                const std::size_t rowBytes =
                    static_cast<std::size_t>(edge)
                    * BytesPerPixel;
                auto& data = initialData[
                    D3D11CalcSubresource(mip, face, mips)];
                data.pSysMem = cursor;
                data.SysMemPitch =
                    static_cast<UINT>(rowBytes);
                cursor += rowBytes * edge;
            }
        }

        D3D11_TEXTURE2D_DESC description{};
        description.Width = size;
        description.Height = size;
        description.MipLevels = mips;
        description.ArraySize = 6;
        description.Format = CubeFormat;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        description.MiscFlags =
            D3D11_RESOURCE_MISC_TEXTURECUBE;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device->CreateTexture2D(
            &description,
            initialData.data(),
            texture.ReleaseAndGetAddressOf())))
        {
            return {};
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
        viewDescription.Format = CubeFormat;
        viewDescription.ViewDimension =
            D3D11_SRV_DIMENSION_TEXTURECUBE;
        viewDescription.TextureCube.MipLevels = mips;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        if (FAILED(device->CreateShaderResourceView(
            texture.Get(),
            &viewDescription,
            view.ReleaseAndGetAddressOf())))
        {
            return {};
        }
        return view;
    }

    void AppendU32(
        std::vector<std::uint8_t>& output,
        const std::uint32_t value)
    {
        const auto* begin =
            reinterpret_cast<const std::uint8_t*>(&value);
        output.insert(output.end(), begin, begin + 4);
    }
}

namespace LamaPon::EnvironmentCache
{
    std::filesystem::path CacheDirectory()
    {
        {
            const std::lock_guard<std::mutex> lock(
                g_directoryMutex);
            if (!g_directoryOverride.empty())
            {
                return g_directoryOverride;
            }
        }
        return DefaultDirectory();
    }

    void SetCacheDirectoryOverride(
        std::filesystem::path directory)
    {
        const std::lock_guard<std::mutex> lock(g_directoryMutex);
        g_directoryOverride = std::move(directory);
    }

    std::uint64_t HashBytes(
        const std::span<const std::uint8_t> bytes) noexcept
    {
        std::uint64_t hash = 14695981039346656037ull;
        for (const std::uint8_t byte : bytes)
        {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
        return hash;
    }

    void Store(
        const std::uint64_t key,
        ID3D11Device* const device,
        ID3D11DeviceContext* const context,
        const EnvironmentRenderer::OwnedPrefilteredEnvironment&
            environment) noexcept
    {
        try
        {
            if (!environment.IsValid()
                || device == nullptr
                || context == nullptr)
            {
                return;
            }
            std::uint32_t specularSize{};
            std::uint32_t specularMips{};
            std::vector<std::uint8_t> specularBytes;
            std::uint32_t irradianceSize{};
            std::uint32_t irradianceMips{};
            std::vector<std::uint8_t> irradianceBytes;
            if (!ReadCube(
                    device,
                    context,
                    environment.specular.Get(),
                    specularSize,
                    specularMips,
                    specularBytes)
                || !ReadCube(
                    device,
                    context,
                    environment.irradiance.Get(),
                    irradianceSize,
                    irradianceMips,
                    irradianceBytes))
            {
                return;
            }

            const auto path = EntryPath(key);
            if (path.empty())
            {
                return;
            }
            std::error_code error;
            std::filesystem::create_directories(
                path.parent_path(),
                error);
            if (error)
            {
                return;
            }

            std::vector<std::uint8_t> output;
            output.reserve(
                64
                + specularBytes.size()
                + irradianceBytes.size());
            output.insert(
                output.end(),
                Magic,
                Magic + sizeof(Magic));
            AppendU32(output, FormatVersion);
            AppendU32(output, specularSize);
            AppendU32(output, specularMips);
            AppendU32(output, irradianceSize);
            AppendU32(output, irradianceMips);
            output.insert(
                output.end(),
                specularBytes.begin(),
                specularBytes.end());
            output.insert(
                output.end(),
                irradianceBytes.begin(),
                irradianceBytes.end());
            WriteFileAtomically(path, output);
        }
        catch (...)
        {
            // 保存できなくても、呼ぶ側のベイク／畳み込みは成功
            // しているので何もしません。
        }
    }

    EnvironmentRenderer::OwnedPrefilteredEnvironment TryLoad(
        ID3D11Device* const device,
        const std::uint64_t key)
    {
        EnvironmentRenderer::OwnedPrefilteredEnvironment result;
        try
        {
            const auto path = EntryPath(key);
            if (path.empty() || device == nullptr)
            {
                return result;
            }
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                return result;
            }
            input.seekg(0, std::ios::end);
            const std::streamoff fileSize = input.tellg();
            if (fileSize <= 0)
            {
                return result;
            }
            input.seekg(0, std::ios::beg);
            std::vector<std::uint8_t> bytes(
                static_cast<std::size_t>(fileSize));
            input.read(
                reinterpret_cast<char*>(bytes.data()),
                fileSize);
            if (!input)
            {
                return result;
            }
            input.close();

            // ヘッダー: magic + version + サイズ4つ。
            constexpr std::size_t HeaderBytes = 4 + 4 * 5;
            if (bytes.size() < HeaderBytes
                || std::memcmp(
                    bytes.data(),
                    Magic,
                    sizeof(Magic)) != 0)
            {
                return result;
            }
            const auto readU32 =
                [&bytes](const std::size_t offset)
            {
                std::uint32_t value{};
                std::memcpy(&value, bytes.data() + offset, 4);
                return value;
            };
            if (readU32(4) != FormatVersion)
            {
                return result;
            }
            const std::uint32_t specularSize = readU32(8);
            const std::uint32_t specularMips = readU32(12);
            const std::uint32_t irradianceSize = readU32(16);
            const std::uint32_t irradianceMips = readU32(20);
            if (specularSize == 0
                || specularSize > MaximumSize
                || specularMips == 0
                || specularMips > MaximumMips
                || irradianceSize == 0
                || irradianceSize > MaximumSize
                || irradianceMips == 0
                || irradianceMips > MaximumMips)
            {
                return result;
            }
            const std::size_t specularBytes =
                CubeByteCount(specularSize, specularMips);
            const std::size_t irradianceBytes =
                CubeByteCount(irradianceSize, irradianceMips);
            // サイズがぴったり合わないファイルは信用しません。
            if (bytes.size()
                != HeaderBytes + specularBytes
                    + irradianceBytes)
            {
                return result;
            }

            auto specular = CreateCube(
                device,
                specularSize,
                specularMips,
                bytes.data() + HeaderBytes);
            auto irradiance = CreateCube(
                device,
                irradianceSize,
                irradianceMips,
                bytes.data() + HeaderBytes + specularBytes);
            if (specular == nullptr || irradiance == nullptr)
            {
                return result;
            }
            result.specular = std::move(specular);
            result.irradiance = std::move(irradiance);
            result.specularMaximumMip =
                static_cast<float>(specularMips - 1);
            return result;
        }
        catch (...)
        {
            return {};
        }
    }
}
