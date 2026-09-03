#include "LamaPon/Graphics/PngWriter.h"

#include "LamaPon/Core/PathUtils.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace
{
    using Microsoft::WRL::ComPtr;

    // GUIDの比較にguiddef.hのoperator==/!=を**使わない**こと。
    // グローバルなinline演算子のCOMDATがこのオブジェクトに載ると、
    // CMakeのWINDOWS_EXPORT_ALL_SYMBOLSがexports.defへ「==」「!=」
    // という名前をそのまま書き出し、LamaPonRuntime.dllのリンクが
    // LNK2001で落ちます（2026-08-07に実際に踏みました。
    // exports.defの中身を見るまで原因が分かりません）。
    [[nodiscard]] bool SameGuid(
        const GUID& left,
        const GUID& right) noexcept
    {
        return std::memcmp(
            &left,
            &right,
            sizeof(GUID)) == 0;
    }

    void ThrowIfFailed(
        const HRESULT result,
        const char* what)
    {
        if (FAILED(result))
        {
            char buffer[16]{};
            std::snprintf(
                buffer,
                sizeof(buffer),
                "%08X",
                static_cast<unsigned>(result));
            throw std::runtime_error(
                std::string{ what }
                + " (HRESULT=0x"
                + buffer
                + ")");
        }
    }
}

namespace LamaPon
{
    void SavePng(
        const std::filesystem::path& path,
        const std::uint32_t width,
        const std::uint32_t height,
        const std::vector<std::uint8_t>& rgbaPixels)
    {
        if (rgbaPixels.size()
            < static_cast<std::size_t>(width)
                * height * 4)
        {
            throw std::runtime_error(
                "SavePng: the pixel buffer is smaller"
                " than width x height.");
        }
        // アルファを不透明へ倒します（ヘッダのコメントを参照）。
        std::vector<std::uint8_t> opaque = rgbaPixels;
        for (std::size_t offset = 3;
            offset < opaque.size();
            offset += 4)
        {
            opaque[offset] = 255;
        }

        ComPtr<IWICImagingFactory> factory;
        ThrowIfFailed(
            CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory)),
            "Could not create the WIC factory.");

        ComPtr<IWICStream> stream;
        ThrowIfFailed(
            factory->CreateStream(&stream),
            "Could not create the WIC stream.");
        ThrowIfFailed(
            stream->InitializeFromFilename(
                path.c_str(),
                GENERIC_WRITE),
            "Could not open the PNG for writing.");

        ComPtr<IWICBitmapEncoder> encoder;
        ThrowIfFailed(
            factory->CreateEncoder(
                GUID_ContainerFormatPng,
                nullptr,
                &encoder),
            "Could not create the PNG encoder.");
        ThrowIfFailed(
            encoder->Initialize(
                stream.Get(),
                WICBitmapEncoderNoCache),
            "Could not initialize the PNG encoder.");

        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> properties;
        ThrowIfFailed(
            encoder->CreateNewFrame(&frame, &properties),
            "Could not create the PNG frame.");
        ThrowIfFailed(
            frame->Initialize(properties.Get()),
            "Could not initialize the PNG frame.");
        ThrowIfFailed(
            frame->SetSize(width, height),
            "Could not set the PNG size.");

        // 32bppRGBAを頼みますが、エンコーダーは別形式へ交渉して
        // 返すことがあります（VM上のWICは32bppBGRAを返しました）。
        // BGRAならRとBを入れ替えて合わせます。それ以外の形式は、
        // 黙って書くと色が壊れるので止めます。
        WICPixelFormatGUID format =
            GUID_WICPixelFormat32bppRGBA;
        ThrowIfFailed(
            frame->SetPixelFormat(&format),
            "Could not set the PNG pixel format.");
        if (SameGuid(
                format,
                GUID_WICPixelFormat32bppBGRA))
        {
            for (std::size_t offset = 0;
                offset + 2 < opaque.size();
                offset += 4)
            {
                std::swap(
                    opaque[offset],
                    opaque[offset + 2]);
            }
        }
        else if (!SameGuid(
            format,
            GUID_WICPixelFormat32bppRGBA))
        {
            throw std::runtime_error(
                "The PNG encoder accepted neither RGBA"
                " nor BGRA. The channel order would be"
                " wrong.");
        }

        ThrowIfFailed(
            frame->WritePixels(
                height,
                width * 4,
                static_cast<UINT>(opaque.size()),
                opaque.data()),
            "Could not write the PNG pixels.");
        ThrowIfFailed(
            frame->Commit(),
            "Could not commit the PNG frame.");
        ThrowIfFailed(
            encoder->Commit(),
            "Could not commit the PNG file.");
    }
}
