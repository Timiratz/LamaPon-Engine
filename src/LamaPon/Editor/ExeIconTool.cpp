#include "LamaPon/Editor/ExeIconTool.h"

#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Resources/WindowsResource.h"

#include <Windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
    // ICO／リソースの各構造体は2バイト境界で定義されています。
#pragma pack(push, 2)
    struct IcoHeader final
    {
        std::uint16_t reserved;
        std::uint16_t type;
        std::uint16_t count;
    };

    struct IcoDirEntry final
    {
        std::uint8_t width;
        std::uint8_t height;
        std::uint8_t colorCount;
        std::uint8_t reserved;
        std::uint16_t planes;
        std::uint16_t bitCount;
        std::uint32_t bytesInResource;
        std::uint32_t imageOffset;
    };

    struct GroupIconDirEntry final
    {
        std::uint8_t width;
        std::uint8_t height;
        std::uint8_t colorCount;
        std::uint8_t reserved;
        std::uint16_t planes;
        std::uint16_t bitCount;
        std::uint32_t bytesInResource;
        std::uint16_t resourceId;
    };
#pragma pack(pop)

    // 呼び出しスレッドのCOMを初期化するRAII。
    struct ComScope final
    {
        bool uninitialize{};

        ComScope()
        {
            const HRESULT result = CoInitializeEx(
                nullptr,
                COINIT_MULTITHREADED);
            uninitialize = SUCCEEDED(result);
        }

        ~ComScope()
        {
            if (uninitialize)
            {
                CoUninitialize();
            }
        }
    };

    void ThrowIfFailed(
        const HRESULT result,
        const char* message)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(
                std::string(message)
                + " (HRESULT "
                + std::to_string(
                    static_cast<unsigned long>(result))
                + ")");
        }
    }

    void Append(
        std::vector<std::byte>& destination,
        const void* data,
        const std::size_t size)
    {
        const auto* bytes =
            static_cast<const std::byte*>(data);
        destination.insert(
            destination.end(),
            bytes,
            bytes + size);
    }

    // 1画像をICO格納用の32bit DIB（ヘッダー＋ボトムアップのピクセル＋
    // ANDマスク）へ変換します。
    std::vector<std::byte> BuildDibEntry(
        const LamaPon::IconImage& image)
    {
        const std::size_t rowBytes =
            static_cast<std::size_t>(image.width) * 4;
        const std::size_t maskRowBytes =
            ((image.width + 31) / 32) * 4;

        BITMAPINFOHEADER header{};
        header.biSize = sizeof(header);
        header.biWidth = static_cast<LONG>(image.width);
        // ICOのDIB高さはXORとANDの合計で2倍を指定します。
        header.biHeight =
            static_cast<LONG>(image.height) * 2;
        header.biPlanes = 1;
        header.biBitCount = 32;
        header.biCompression = BI_RGB;
        header.biSizeImage = static_cast<DWORD>(
            rowBytes * image.height
            + maskRowBytes * image.height);

        std::vector<std::byte> entry;
        entry.reserve(
            sizeof(header) + header.biSizeImage);
        Append(entry, &header, sizeof(header));
        for (std::uint32_t row = image.height;
            row > 0;
            --row)
        {
            Append(
                entry,
                image.bgraPixels.data()
                    + static_cast<std::size_t>(row - 1)
                        * rowBytes,
                rowBytes);
        }
        // アルファ付き32bitなのでANDマスクは全ビット0で足ります。
        entry.resize(
            entry.size()
                + maskRowBytes * image.height,
            std::byte{});
        return entry;
    }

    std::vector<std::byte> ReadFileBytes(
        const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error(
                "Could not open the icon image: "
                + LamaPon::PathToUtf8(path));
        }
        std::vector<std::byte> bytes(
            std::filesystem::file_size(path));
        input.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        if (!input)
        {
            throw std::runtime_error(
                "Could not read the icon image: "
                + LamaPon::PathToUtf8(path));
        }
        return bytes;
    }

    // .icoバイト列を検証し、各画像の格納位置を取り出します。
    std::vector<IcoDirEntry> ParseIcoEntries(
        const std::vector<std::byte>& icoBytes)
    {
        IcoHeader header{};
        if (icoBytes.size() < sizeof(header))
        {
            throw std::runtime_error(
                "Icon data is too small to be a .ico file.");
        }
        std::memcpy(&header, icoBytes.data(), sizeof(header));
        if (header.reserved != 0
            || header.type != 1
            || header.count == 0)
        {
            throw std::runtime_error(
                "Icon data is not a valid .ico file.");
        }

        std::vector<IcoDirEntry> entries(header.count);
        const std::size_t directoryBytes =
            sizeof(IcoDirEntry) * entries.size();
        if (icoBytes.size()
            < sizeof(header) + directoryBytes)
        {
            throw std::runtime_error(
                "Icon directory is truncated.");
        }
        std::memcpy(
            entries.data(),
            icoBytes.data() + sizeof(header),
            directoryBytes);
        for (const auto& entry : entries)
        {
            const std::uint64_t end =
                static_cast<std::uint64_t>(entry.imageOffset)
                + entry.bytesInResource;
            if (entry.bytesInResource == 0
                || end > icoBytes.size())
            {
                throw std::runtime_error(
                    "Icon image data is out of range.");
            }
        }
        return entries;
    }

    // リソース言語の列挙結果をまとめる入れ物。
    struct ResourceLanguageList final
    {
        std::vector<WORD> languages;
    };

    BOOL CALLBACK CollectResourceLanguage(
        HMODULE,
        LPCWSTR,
        LPCWSTR,
        const WORD language,
        const LONG_PTR parameter)
    {
        reinterpret_cast<ResourceLanguageList*>(parameter)
            ->languages.push_back(language);
        return TRUE;
    }

    std::vector<WORD> FindResourceLanguages(
        const HMODULE module,
        const LPCWSTR type,
        const LPCWSTR name)
    {
        ResourceLanguageList list;
        EnumResourceLanguagesW(
            module,
            type,
            name,
            CollectResourceLanguage,
            reinterpret_cast<LONG_PTR>(&list));
        return std::move(list.languages);
    }

    // 既存アイコングループを読み、削除すべき（ID・言語）の一覧を
    // 集めます。グループが無ければ空を返します。
    struct ExistingIconResources final
    {
        std::vector<std::pair<WORD, WORD>> icons;
        std::vector<WORD> groupLanguages;
    };

    ExistingIconResources CollectExistingIcons(
        const std::filesystem::path& executablePath)
    {
        ExistingIconResources existing;
        const HMODULE module = LoadLibraryExW(
            executablePath.c_str(),
            nullptr,
            LOAD_LIBRARY_AS_DATAFILE
                | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (module == nullptr)
        {
            return existing;
        }

        const auto groupName =
            MAKEINTRESOURCEW(IDI_LAMAPON_ENGINE);
        existing.groupLanguages = FindResourceLanguages(
            module,
            RT_GROUP_ICON,
            groupName);
        for (const WORD language : existing.groupLanguages)
        {
            const HRSRC resource = FindResourceExW(
                module,
                RT_GROUP_ICON,
                groupName,
                language);
            if (resource == nullptr)
            {
                continue;
            }
            const HGLOBAL loaded =
                LoadResource(module, resource);
            const auto* data = loaded != nullptr
                ? static_cast<const std::byte*>(
                    LockResource(loaded))
                : nullptr;
            const DWORD size =
                SizeofResource(module, resource);
            if (data == nullptr
                || size < sizeof(IcoHeader))
            {
                continue;
            }

            IcoHeader header{};
            std::memcpy(&header, data, sizeof(header));
            const std::size_t available =
                (size - sizeof(header))
                / sizeof(GroupIconDirEntry);
            const std::size_t count = std::min<std::size_t>(
                header.count,
                available);
            for (std::size_t index = 0;
                index < count;
                ++index)
            {
                GroupIconDirEntry entry{};
                std::memcpy(
                    &entry,
                    data
                        + sizeof(header)
                        + index * sizeof(entry),
                    sizeof(entry));
                for (const WORD iconLanguage :
                    FindResourceLanguages(
                        module,
                        RT_ICON,
                        MAKEINTRESOURCEW(entry.resourceId)))
                {
                    existing.icons.emplace_back(
                        entry.resourceId,
                        iconLanguage);
                }
            }
        }
        FreeLibrary(module);
        return existing;
    }

    // WICで画像を読み込み、指定サイズのBGRAへ縮小します。
    LamaPon::IconImage DecodeScaledImage(
        IWICImagingFactory* const factory,
        IWICBitmapFrameDecode* const frame,
        const std::uint32_t size)
    {
        using Microsoft::WRL::ComPtr;

        ComPtr<IWICBitmapScaler> scaler;
        ThrowIfFailed(
            factory->CreateBitmapScaler(&scaler),
            "IWICImagingFactory::CreateBitmapScaler");
        ThrowIfFailed(
            scaler->Initialize(
                frame,
                size,
                size,
                WICBitmapInterpolationModeFant),
            "IWICBitmapScaler::Initialize");

        ComPtr<IWICFormatConverter> converter;
        ThrowIfFailed(
            factory->CreateFormatConverter(&converter),
            "IWICImagingFactory::CreateFormatConverter");
        ThrowIfFailed(
            converter->Initialize(
                scaler.Get(),
                GUID_WICPixelFormat32bppBGRA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeCustom),
            "IWICFormatConverter::Initialize");

        LamaPon::IconImage image;
        image.width = size;
        image.height = size;
        image.bgraPixels.resize(
            static_cast<std::size_t>(size) * size * 4);
        ThrowIfFailed(
            converter->CopyPixels(
                nullptr,
                size * 4,
                static_cast<UINT>(image.bgraPixels.size()),
                reinterpret_cast<BYTE*>(
                    image.bgraPixels.data())),
            "IWICFormatConverter::CopyPixels");
        return image;
    }
}

namespace LamaPon
{
    std::vector<std::byte> BuildIcoFileBytes(
        const std::vector<IconImage>& images)
    {
        if (images.empty())
        {
            throw std::invalid_argument(
                "At least one icon image is required.");
        }

        std::vector<std::vector<std::byte>> entryData;
        entryData.reserve(images.size());
        for (const auto& image : images)
        {
            if (image.width == 0
                || image.width > 256
                || image.height == 0
                || image.height > 256)
            {
                throw std::invalid_argument(
                    "Icon images must be 1 to 256 pixels.");
            }
            if (image.bgraPixels.size()
                != static_cast<std::size_t>(image.width)
                    * image.height * 4)
            {
                throw std::invalid_argument(
                    "Icon pixel data does not match its size.");
            }
            entryData.push_back(BuildDibEntry(image));
        }

        IcoHeader header{};
        header.type = 1;
        header.count =
            static_cast<std::uint16_t>(images.size());

        std::vector<std::byte> ico;
        Append(ico, &header, sizeof(header));
        std::uint32_t offset = static_cast<std::uint32_t>(
            sizeof(IcoHeader)
            + sizeof(IcoDirEntry) * images.size());
        for (std::size_t index = 0;
            index < images.size();
            ++index)
        {
            const auto& image = images[index];
            IcoDirEntry entry{};
            // 256ピクセルはICOの慣例で0と記録します。
            entry.width = static_cast<std::uint8_t>(
                image.width == 256 ? 0 : image.width);
            entry.height = static_cast<std::uint8_t>(
                image.height == 256 ? 0 : image.height);
            entry.planes = 1;
            entry.bitCount = 32;
            entry.bytesInResource =
                static_cast<std::uint32_t>(
                    entryData[index].size());
            entry.imageOffset = offset;
            offset += entry.bytesInResource;
            Append(ico, &entry, sizeof(entry));
        }
        for (const auto& data : entryData)
        {
            ico.insert(
                ico.end(),
                data.begin(),
                data.end());
        }
        return ico;
    }

    std::vector<std::byte> BuildIcoFromImageFile(
        const std::filesystem::path& imagePath)
    {
        auto extension = imagePath.extension().wstring();
        std::transform(
            extension.begin(),
            extension.end(),
            extension.begin(),
            [](const wchar_t value)
            {
                return static_cast<wchar_t>(
                    std::towlower(value));
            });
        if (extension == L".ico")
        {
            auto bytes = ReadFileBytes(imagePath);
            static_cast<void>(ParseIcoEntries(bytes));
            return bytes;
        }

        const ComScope comScope;
        using Microsoft::WRL::ComPtr;

        ComPtr<IWICImagingFactory> factory;
        ThrowIfFailed(
            CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory)),
            "CoCreateInstance(WICImagingFactory)");

        ComPtr<IWICBitmapDecoder> decoder;
        ThrowIfFailed(
            factory->CreateDecoderFromFilename(
                imagePath.c_str(),
                nullptr,
                GENERIC_READ,
                WICDecodeMetadataCacheOnLoad,
                &decoder),
            "IWICImagingFactory::CreateDecoderFromFilename");

        ComPtr<IWICBitmapFrameDecode> frame;
        ThrowIfFailed(
            decoder->GetFrame(0, &frame),
            "IWICBitmapDecoder::GetFrame");

        // Explorerの一覧・タイトルバー・Alt+Tabをカバーする標準サイズ。
        constexpr std::array<std::uint32_t, 5> sizes{
            16u, 24u, 32u, 48u, 256u
        };
        std::vector<IconImage> images;
        images.reserve(sizes.size());
        for (const auto size : sizes)
        {
            images.push_back(
                DecodeScaledImage(
                    factory.Get(),
                    frame.Get(),
                    size));
        }
        return BuildIcoFileBytes(images);
    }

    void ReplaceExecutableIcon(
        const std::filesystem::path& executablePath,
        const std::vector<std::byte>& icoBytes)
    {
        const auto entries = ParseIcoEntries(icoBytes);
        const auto existing =
            CollectExistingIcons(executablePath);

        const HANDLE update = BeginUpdateResourceW(
            executablePath.c_str(),
            FALSE);
        if (update == nullptr)
        {
            throw std::runtime_error(
                "Could not open the executable for icon update: "
                + PathToUtf8(executablePath));
        }

        bool succeeded = true;
        const WORD neutralLanguage =
            MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
        const auto groupName =
            MAKEINTRESOURCEW(IDI_LAMAPON_ENGINE);

        // 既存のアイコン本体とグループを（言語ごとに）削除します。
        for (const auto& [iconId, language] : existing.icons)
        {
            UpdateResourceW(
                update,
                RT_ICON,
                MAKEINTRESOURCEW(iconId),
                language,
                nullptr,
                0);
        }
        for (const WORD language : existing.groupLanguages)
        {
            UpdateResourceW(
                update,
                RT_GROUP_ICON,
                groupName,
                language,
                nullptr,
                0);
        }

        // 新しいアイコン本体をID 1..Nで書き込みます。
        std::vector<std::byte> group;
        IcoHeader groupHeader{};
        groupHeader.type = 1;
        groupHeader.count =
            static_cast<std::uint16_t>(entries.size());
        Append(group, &groupHeader, sizeof(groupHeader));
        for (std::size_t index = 0;
            index < entries.size();
            ++index)
        {
            const auto& entry = entries[index];
            const WORD iconId =
                static_cast<WORD>(index + 1);
            succeeded = succeeded
                && UpdateResourceW(
                    update,
                    RT_ICON,
                    MAKEINTRESOURCEW(iconId),
                    neutralLanguage,
                    const_cast<std::byte*>(
                        icoBytes.data()
                        + entry.imageOffset),
                    entry.bytesInResource)
                    != FALSE;

            GroupIconDirEntry groupEntry{};
            groupEntry.width = entry.width;
            groupEntry.height = entry.height;
            groupEntry.colorCount = entry.colorCount;
            groupEntry.planes = entry.planes;
            groupEntry.bitCount = entry.bitCount;
            groupEntry.bytesInResource =
                entry.bytesInResource;
            groupEntry.resourceId = iconId;
            Append(group, &groupEntry, sizeof(groupEntry));
        }
        succeeded = succeeded
            && UpdateResourceW(
                update,
                RT_GROUP_ICON,
                groupName,
                neutralLanguage,
                group.data(),
                static_cast<DWORD>(group.size()))
                != FALSE;

        if (!succeeded)
        {
            EndUpdateResourceW(update, TRUE);
            throw std::runtime_error(
                "Could not write the icon resources: "
                + PathToUtf8(executablePath));
        }
        if (EndUpdateResourceW(update, FALSE) == FALSE)
        {
            throw std::runtime_error(
                "Could not commit the icon update: "
                + PathToUtf8(executablePath));
        }
    }
}
