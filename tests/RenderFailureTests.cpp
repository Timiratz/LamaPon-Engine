// 組み込みシェーダーが組み立てられないときの振る舞いの検査です。
//
// ユーザーの書いたShaderが壊れていれば、マゼンタの代役を描いて
// エディターは動き続けます（GraphicsDevice::ShaderErrorPlaceholder）。
// ところがエンジン自身のShader（LamaPonLit・LamaPonEnvironment・
// LamaPonLightCulling）には代役を差し込む先が無く、例外がmainまで
// 飛んで**エディターごと終了**していました。プロジェクトが開けない
// ということは、直す手段も無いということです。
//
// 投げること自体は変えられないので、ここで確かめるのは
//
//   1. 同じ失敗のために**毎フレームコンパイルし直さない**こと
//      （大きなHLSLだと、壊れている間エディターが事実上止まります）
//   2. 直したら**そのまま戻ってくる**こと
//
// の2つです。「落ちない」側はApplicationの描画ループがtry/catchで
// 受けています（UIが生きていれば、開いたまま直せます）。
//
// WARPデバイスを使うので、D3D11が動かない環境では実行できません。

#include "LamaPon/LamaPon.h"

#include "LamaPon/Graphics/ShaderCompiler.h"

#include <Windows.h>
#include <objbase.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

namespace
{
    constexpr std::uint32_t Width = 64;
    constexpr std::uint32_t Height = 64;

    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    void Stage(const char* name)
    {
        std::cout << "stage: " << name << std::endl;
    }

    [[nodiscard]] HWND CreateHiddenWindow()
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"LamaPonRenderFailureTests";
        RegisterClassExW(&windowClass);
        return CreateWindowExW(
            0,
            windowClass.lpszClassName,
            L"LamaPonRenderFailureTests",
            WS_OVERLAPPEDWINDOW,
            0,
            0,
            static_cast<int>(Width),
            static_cast<int>(Height),
            nullptr,
            nullptr,
            windowClass.hInstance,
            nullptr);
    }

    class TemporaryDirectory final
    {
    public:
        TemporaryDirectory()
        {
            const auto unique =
                std::chrono::steady_clock::now()
                    .time_since_epoch().count();
            m_path = std::filesystem::temp_directory_path()
                / (L"LamaPonRenderFailureTests-"
                    + std::to_wstring(unique));
            std::filesystem::create_directories(
                m_path / L"shaders");
        }

        ~TemporaryDirectory()
        {
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
        }

        TemporaryDirectory(const TemporaryDirectory&) = delete;
        TemporaryDirectory& operator=(
            const TemporaryDirectory&) = delete;

        [[nodiscard]] const std::filesystem::path&
            Path() const noexcept
        {
            return m_path;
        }

    private:
        std::filesystem::path m_path;
    };

    void WriteFile(
        const std::filesystem::path& path,
        const std::string& contents)
    {
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        output << contents;
        if (!output)
        {
            throw std::runtime_error(
                "Could not write the test shader.");
        }
    }

    // 例外のメッセージを返します。投げなければ空文字列です。
    template <typename Callable>
    [[nodiscard]] std::string FailureOf(Callable&& callable)
    {
        try
        {
            callable();
            return {};
        }
        catch (const std::exception& exception)
        {
            return exception.what();
        }
    }
}

int main()
{
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(comResult);

    int status = 0;
    HWND window = nullptr;
    try
    {
        TemporaryDirectory root;
        const auto shader =
            root.Path() / L"shaders" / L"LamaPonEnvironment.hlsl";
        // 壊れた組み込みシェーダー。**構文エラーなので失敗は
        // キャッシュへ残りません**（許可リストは「入口が無い」だけ）。
        // つまり毎回コンパイルし直せてしまう＝覚えていなければ
        // 毎フレーム走る、という状況を作れます。
        WriteFile(
            shader,
            "float4 VSMain() : SV_Position"
            " { return not_a_function(); }\n");

        Stage("window");
        window = CreateHiddenWindow();
        Require(
            window != nullptr,
            "the hidden window must be created");

        // CIランナーにはGPUが無いためWARPを明示します。
        LamaPon::GraphicsDevice::SetPreferWarpAdapter(true);

        LamaPon::GraphicsDevice graphics;
        Stage("initialize");
        graphics.Initialize(window, Width, Height);
        graphics.Assets().SetAssetRoot(root.Path());

        Stage("first-failure");
        LamaPon::ResetShaderCompileStatistics();
        const auto first =
            FailureOf([&] { graphics.Environment(); });
        Require(
            !first.empty(),
            "a broken built-in shader must still throw");
        const auto afterFirst =
            LamaPon::ShaderCompileStatistics().compiledCount;
        Require(
            afterFirst > 0,
            "the first attempt must actually compile");

        // 覚えていること。ここが効いていないと、壊れている間
        // 毎フレーム大きなHLSLをコンパイルし続けます。
        Stage("latched");
        for (int frame = 0; frame < 5; ++frame)
        {
            const auto repeated =
                FailureOf([&] { graphics.Environment(); });
            Require(
                repeated == first,
                "the remembered failure must be returned as-is");
        }
        Require(
            LamaPon::ShaderCompileStatistics().compiledCount
                == afterFirst,
            "a remembered failure must not recompile");

        // 代役シェーダーの計数。撮った絵の色から推測せずに
        // 「壊れたものが描かれたか」を言い切るための事実なので、
        // 「渡したときだけ増える」ことをここで固定します。
        Stage("fallback-count");
        graphics.ResetShaderFallbackDraws();
        Require(
            graphics.FrameStats().shaderFallbackDraws == 0,
            "resetting must zero the fallback count");
        const auto* const placeholder =
            graphics.ShaderErrorPlaceholder(false);
        // 代役そのものを用意できない環境（シェーダーが配られて
        // いない）でも、数えていないことは確かめられます。
        Require(
            (placeholder != nullptr)
                == (graphics.FrameStats()
                        .shaderFallbackDraws
                    > 0),
            "the count must rise only when a placeholder"
            " is actually handed out");
        if (placeholder != nullptr)
        {
            const auto once =
                graphics.FrameStats().shaderFallbackDraws;
            graphics.ShaderErrorPlaceholder(false);
            Require(
                graphics.FrameStats().shaderFallbackDraws
                    == once + 1,
                "every hand-out must be counted, not just"
                " the first one");
        }
        graphics.ResetShaderFallbackDraws();

        // 直したら戻ってくること。組み込みシェーダーは手で書ける
        // 大きさではないので、本物を置きます（#includeする.hlsliも
        // 一緒に——これを忘れると2026-08-07と同じ状況になります）。
        Stage("recover");
        const std::filesystem::path engineShaders{
            LAMAPON_TEST_ASSET_DIR
        };
        std::filesystem::copy_file(
            engineShaders / "shaders" / "LamaPonScreenDepth.hlsli",
            root.Path() / L"shaders" / L"LamaPonScreenDepth.hlsli",
            std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy_file(
            engineShaders / "shaders" / "LamaPonEnvironment.hlsl",
            shader,
            std::filesystem::copy_options::overwrite_existing);
        // 覚えている間は投げ続けます。試し直す間隔を待ちます。
        std::this_thread::sleep_for(
            std::chrono::milliseconds(2500));
        const auto recovered =
            FailureOf([&] { graphics.Environment(); });
        Require(
            recovered.empty(),
            "fixing the shader must bring rendering back");

        std::cout << "Render failure checks passed."
                  << std::endl;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Render failure check failed: "
                  << exception.what() << std::endl;
        status = 1;
    }

    if (window != nullptr)
    {
        DestroyWindow(window);
    }
    if (uninitialize)
    {
        CoUninitialize();
    }
    return status;
}
