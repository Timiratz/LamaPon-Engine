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

    // GUIDのバイト列が一致するか判定する(left: 左の識別子, right: 右の識別子)。
    // GUID演算子のCOMDATが自動エクスポートへ混入するとリンクが失敗するため、memcmpで比較する。
    [[nodiscard]] bool SameGuid(
        const GUID& left,
        const GUID& right) noexcept
    {
        return std::memcmp(
            &left,
            &right,
            sizeof(GUID)) == 0;
    }

    // HRESULTの失敗を十六進診断付きの例外へ変換する(result: 操作結果, what: 診断に表示する操作名)。
    void ThrowIfFailed(
        const HRESULT result,
        const char* what)
    {
        if (FAILED(result))
        {
            // HRESULTの十六進表示領域
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

        // アルファを255にした保存画素列
        std::vector<std::uint8_t> opaque = rgbaPixels;
        // 処理する画素のバイト位置
        for (std::size_t offset = 3;
            offset < opaque.size();
            offset += 4)
        {
            opaque[offset] = 255;
        }

        // WICの画像作成ファクトリー
        ComPtr<IWICImagingFactory> factory;
        ThrowIfFailed(
            CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory)),
            "Could not create the WIC factory.");

        // PNGの書込ストリーム
        ComPtr<IWICStream> stream;
        ThrowIfFailed(
            factory->CreateStream(&stream),
            "Could not create the WIC stream.");
        ThrowIfFailed(
            stream->InitializeFromFilename(
                path.c_str(),
                GENERIC_WRITE),
            "Could not open the PNG for writing.");

        // PNG形式のエンコーダー
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

        // 書き込むPNGの画像フレーム
        ComPtr<IWICBitmapFrameEncode> frame;
        // PNGフレームの作成設定
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

        // WICがBGRAを選んだ場合は赤と青を交換し、他の形式は拒否する。
        // WICが選択する画素形式
        WICPixelFormatGUID format =
            GUID_WICPixelFormat32bppRGBA;
        ThrowIfFailed(
            frame->SetPixelFormat(&format),
            "Could not set the PNG pixel format.");
        if (SameGuid(
                format,
                GUID_WICPixelFormat32bppBGRA))
        {
            // 処理する画素のバイト位置
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
