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
        // 予約領域・0
        std::uint16_t reserved;
        // 形式識別・ICOは1
        std::uint16_t type;
        // 格納する画像の16bit枚数
        std::uint16_t count;
    };

    struct IcoDirEntry final
    {
        // 画像の幅・0は256ピクセル
        std::uint8_t width;
        // 画像の高さ・0は256ピクセル
        std::uint8_t height;
        // paletteの色数・true colorは0
        std::uint8_t colorCount;
        // 予約領域・0
        std::uint8_t reserved;
        // 色planesの数
        std::uint16_t planes;
        // 1ピクセルの色のbit数
        std::uint16_t bitCount;
        // 画像データのバイト数
        std::uint32_t bytesInResource;
        // ファイル先頭からの画像位置
        std::uint32_t imageOffset;
    };

    struct GroupIconDirEntry final
    {
        // 画像の幅・0は256ピクセル
        std::uint8_t width;
        // 画像の高さ・0は256ピクセル
        std::uint8_t height;
        // paletteの色数・true colorは0
        std::uint8_t colorCount;
        // 予約領域・0
        std::uint8_t reserved;
        // 色planesの数
        std::uint16_t planes;
        // 1ピクセルの色のbit数
        std::uint16_t bitCount;
        // 画像データのバイト数
        std::uint32_t bytesInResource;
        // 画像本体のresource ID
        std::uint16_t resourceId;
    };
#pragma pack(pop)

    // 呼び出しスレッドのCOMを初期化するRAII。
    struct ComScope final
    {
        // COM初期化に成功し終了処理が必要か
        bool uninitialize{};

        // 呼出し元のCOMをMTAとして初期化し成功時だけ終了処理を所有する。
        ComScope()
        {
            // COM初期化のHRESULT
            const HRESULT result = CoInitializeEx(
                nullptr,
                COINIT_MULTITHREADED);
            uninitialize = SUCCEEDED(result);
        }

        // このscopeで成功したCOM初期化を終了する。
        ~ComScope()
        {
            if (uninitialize)
            {
                CoUninitialize();
            }
        }
    };

    // 失敗したHRESULTなら診断名と数値を含む例外を出す(result: 判定するHRESULT, message: 失敗したAPI等の診断名)。
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

    // 指定範囲のバイト列を末尾へ複製する(destination: 追記するバイト列, data: 複製する領域の借用, size: 複製するバイト数)。
    void Append(
        std::vector<std::byte>& destination,
        const void* data,
        const std::size_t size)
    {
        // 読取・複製するバイト列
        const auto* bytes =
            static_cast<const std::byte*>(data);
        destination.insert(
            destination.end(),
            bytes,
            bytes + size);
    }

    // 上から下のBGRA画像を逆順の32bit DIBと全0のANDマスクへ変換する(image: 寸法を検証済みのBGRA画像)。
    std::vector<std::byte> BuildDibEntry(
        const LamaPon::IconImage& image)
    {
        // BGRA画像の1行のバイト数
        const std::size_t rowBytes =
            static_cast<std::size_t>(image.width) * 4;
        // 32bit境界に揃えたマスク行の長さ
        const std::size_t maskRowBytes =
            ((image.width + 31) / 32) * 4;

        // ICOまたはDIBの形式ヘッダー
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

        // 画像の格納内容または位置情報
        std::vector<std::byte> entry;
        entry.reserve(
            sizeof(header) + header.biSizeImage);
        Append(entry, &header, sizeof(header));
        // 下から複製する画像の1始まり行
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
        // アルファ付き32bit画像では、ANDマスクの全ビットを0にします。
        entry.resize(
            entry.size()
                + maskRowBytes * image.height,
            std::byte{});
        return entry;
    }

    // 画像文書を全バイト読み不完全な読込なら例外を出す(path: 読み取る画像文書のパス)。
    std::vector<std::byte> ReadFileBytes(
        const std::filesystem::path& path)
    {
        // 画像文書を読むバイナリstream
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error(
                "Could not open the icon image: "
                + LamaPon::PathToUtf8(path));
        }
        // 文書サイズに合わせた読込バッファ
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

    // ICOの形式・一覧長・各画像範囲を検証して位置情報を返す(icoBytes: ICO文書の全バイト)。
    std::vector<IcoDirEntry> ParseIcoEntries(
        const std::vector<std::byte>& icoBytes)
    {
        // ICOまたはDIBの形式ヘッダー
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

        // 画像枚数に合わせた位置情報の配列
        std::vector<IcoDirEntry> entries(header.count);
        // ICO画像位置一覧のバイト数
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
        // 画像の格納内容または位置情報
        for (const auto& entry : entries)
        {
            // 64bitで計算した画像の末尾位置
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
        // 列挙したresource言語の一覧
        std::vector<WORD> languages;
    };

    // Windows列挙callbackからresource言語を収集する(language: 検出したresource言語, parameter: 言語一覧の借用アドレス)。
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

    // 指定resourceに存在する全言語を列挙する(module: 読込済みexeの資源, type: 列挙するresourceの種別, name: 列挙するresourceのID)。
    std::vector<WORD> FindResourceLanguages(
        const HMODULE module,
        const LPCWSTR type,
        const LPCWSTR name)
    {
        // resource言語の列挙結果
        ResourceLanguageList list;
        EnumResourceLanguagesW(
            module,
            type,
            name,
            CollectResourceLanguage,
            reinterpret_cast<LONG_PTR>(&list));
        return std::move(list.languages);
    }

    // 固定グループが参照する画像IDと言語の削除対象。
    struct ExistingIconResources final
    {
        // 削除する画像IDと言語の組
        std::vector<std::pair<WORD, WORD>> icons;
        // 削除するアイコングループの言語
        std::vector<WORD> groupLanguages;
    };

    // 固定グループが参照する画像IDと言語を削除対象として集める(executablePath: 元のexeのパス)。
    ExistingIconResources CollectExistingIcons(
        const std::filesystem::path& executablePath)
    {
        // 元アイコンの削除対象一覧
        ExistingIconResources existing;
        // 実行せず読み込んだexe資源のhandle
        const HMODULE module = LoadLibraryExW(
            executablePath.c_str(),
            nullptr,
            LOAD_LIBRARY_AS_DATAFILE
                | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (module == nullptr)
        {
            return existing;
        }

        // 置換する固定アイコングループID
        const auto groupName =
            MAKEINTRESOURCEW(IDI_LAMAPON_ENGINE);
        existing.groupLanguages = FindResourceLanguages(
            module,
            RT_GROUP_ICON,
            groupName);
        // 列挙または削除するresource言語
        for (const WORD language : existing.groupLanguages)
        {
            // 検出したグループresourceの借用
            const HRSRC resource = FindResourceExW(
                module,
                RT_GROUP_ICON,
                groupName,
                language);
            if (resource == nullptr)
            {
                continue;
            }
            // 読み込んだresourceデータのhandle
            const HGLOBAL loaded =
                LoadResource(module, resource);
            // DIBまたはresourceの画像バイト
            const auto* data = loaded != nullptr
                ? static_cast<const std::byte*>(
                    LockResource(loaded))
                : nullptr;
            // resourceサイズまたは作る画像寸法
            const DWORD size =
                SizeofResource(module, resource);
            if (data == nullptr
                || size < sizeof(IcoHeader))
            {
                continue;
            }

            // ICOまたはDIBの形式ヘッダー
            IcoHeader header{};
            std::memcpy(&header, data, sizeof(header));
            // 取得できたグループ画像の最大数
            const std::size_t available =
                (size - sizeof(header))
                / sizeof(GroupIconDirEntry);
            // 範囲内で走査する画像数
            const std::size_t count = std::min<std::size_t>(
                header.count,
                available);
            // 列挙または出力する画像の番号
            for (std::size_t index = 0;
                // 範囲内で走査する画像数
                index < count;
                ++index)
            {
                // 画像の格納内容または位置情報
                GroupIconDirEntry entry{};
                std::memcpy(
                    &entry,
                    data
                        + sizeof(header)
                        + index * sizeof(entry),
                    sizeof(entry));
                // 画像本体に付いたresource言語
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

    // WICで先頭画像を指定の正方形サイズのBGRAへ縮小する(factory: WIC factoryの借用, frame: デコード済み画像の借用, size: 出力画像の辺のピクセル数)。
    LamaPon::IconImage DecodeScaledImage(
        IWICImagingFactory* const factory,
        IWICBitmapFrameDecode* const frame,
        const std::uint32_t size)
    {
        using Microsoft::WRL::ComPtr;

        // 指定寸法へ縮小するWIC処理の所有先
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

        // BGRAへ変換するWIC処理の所有先
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

        // 変換・出力するBGRA画像
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

        // 画像ごとに組み立てたDIBバイト列
        std::vector<std::vector<std::byte>> entryData;
        entryData.reserve(images.size());
        // 変換・出力するBGRA画像
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

        // ICOまたはDIBの形式ヘッダー
        IcoHeader header{};
        header.type = 1;
        header.count =
            static_cast<std::uint16_t>(images.size());

        // 返却するICO文書のバイト列
        std::vector<std::byte> ico;
        Append(ico, &header, sizeof(header));
        // 次の画像を格納するファイル位置
        std::uint32_t offset = static_cast<std::uint32_t>(
            sizeof(IcoHeader)
            + sizeof(IcoDirEntry) * images.size());
        // 列挙または出力する画像の番号
        for (std::size_t index = 0;
            index < images.size();
            ++index)
        {
            // 変換・出力するBGRA画像
            const auto& image = images[index];
            // 画像の格納内容または位置情報
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
        // DIBまたはresourceの画像バイト
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
        // 画像形式の判定用の小文字拡張子
        auto extension = imagePath.extension().wstring();
        // 拡張子を大小文字によらず比較できる表記へ変換する(value: 拡張子の文字)。
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
            // 読取・複製するバイト列
            auto bytes = ReadFileBytes(imagePath);
            static_cast<void>(ParseIcoEntries(bytes));
            return bytes;
        }

        // 必要なCOM初期化と終了処理の番人
        const ComScope comScope;
        using Microsoft::WRL::ComPtr;

        // WIC画像処理factoryの所有先
        ComPtr<IWICImagingFactory> factory;
        ThrowIfFailed(
            CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory)),
            "CoCreateInstance(WICImagingFactory)");

        // 入力画像のWIC decoderの所有先
        ComPtr<IWICBitmapDecoder> decoder;
        ThrowIfFailed(
            factory->CreateDecoderFromFilename(
                imagePath.c_str(),
                nullptr,
                GENERIC_READ,
                WICDecodeMetadataCacheOnLoad,
                &decoder),
            "IWICImagingFactory::CreateDecoderFromFilename");

        // 入力画像の先頭フレームの所有先
        ComPtr<IWICBitmapFrameDecode> frame;
        ThrowIfFailed(
            decoder->GetFrame(0, &frame),
            "IWICBitmapDecoder::GetFrame");

        // 一覧・title・Alt+Tab用の標準寸法
        constexpr std::array<std::uint32_t, 5> sizes{
            16u, 24u, 32u, 48u, 256u
        };
        // 寸法ごとに縮小したBGRA画像一覧
        std::vector<IconImage> images;
        images.reserve(sizes.size());
        // resourceサイズまたは作る画像寸法
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
        // 検証済みの画像位置情報の一覧
        const auto entries = ParseIcoEntries(icoBytes);
        // 元アイコンの削除対象一覧
        const auto existing =
            CollectExistingIcons(executablePath);

        // exe resourceの更新transaction
        const HANDLE update = BeginUpdateResourceW(
            executablePath.c_str(),
            FALSE);
        if (update == nullptr)
        {
            throw std::runtime_error(
                "Could not open the executable for icon update: "
                + PathToUtf8(executablePath));
        }

        // 新しい画像とグループを書けたか
        bool succeeded = true;
        // 新画像とグループに使う中立言語
        const WORD neutralLanguage =
            MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
        // 置換する固定アイコングループID
        const auto groupName =
            MAKEINTRESOURCEW(IDI_LAMAPON_ENGINE);

        // iconId: 削除する元の画像ID、language: 元の画像resourceの言語
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
        // 列挙または削除するresource言語
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

        // 新しいアイコングループのバイト列
        std::vector<std::byte> group;
        // 新しいグループの形式と画像数
        IcoHeader groupHeader{};
        groupHeader.type = 1;
        groupHeader.count =
            static_cast<std::uint16_t>(entries.size());
        Append(group, &groupHeader, sizeof(groupHeader));
        // 列挙または出力する画像の番号
        for (std::size_t index = 0;
            index < entries.size();
            ++index)
        {
            // 画像の格納内容または位置情報
            const auto& entry = entries[index];
            // 新しい画像の1始まりresource ID
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

            // 新しいグループ内の画像の参照
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

        // 新画像またはグループの書込失敗なら更新をcommitせず破棄する。
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
