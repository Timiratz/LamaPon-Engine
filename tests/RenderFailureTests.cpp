#include "LamaPon/LamaPon.h"

#include "LamaPon/Graphics/GraphicsDeviceD3D11Access.h"
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
#include <wrl/client.h>

// Shader失敗の再試行抑制と修復後の復帰を検証する。
// WARPを使うためD3D11非対応環境では実行できない。
namespace
{
    using D3D11Access =
        LamaPon::Detail::GraphicsDeviceD3D11Access;

    // テスト描画幅
    constexpr std::uint32_t Width = 64;
    // テスト描画高さ
    constexpr std::uint32_t Height = 64;

    // Require(condition: 成立条件, message: 失敗理由): 条件不成立を検査失敗にする。
    void Require(const bool condition, const char* message)
    {
        // 検査条件の不成立を検出する。
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // Stage(name: テスト段階): 実行中の検査段階を標準出力へ示す。
    void Stage(const char* name)
    {
        std::cout << "stage: " << name << std::endl;
    }

    // CreateHiddenWindow(): WARP描画に必要な非表示ウィンドウを作る。
    [[nodiscard]] HWND CreateHiddenWindow()
    {
        // Win32ウィンドウクラス設定
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
        // TemporaryDirectory(): shaderサブフォルダーを持つ一時領域を作る。
        TemporaryDirectory()
        {
            // 一時パス衝突の回避値
            const auto unique =
                std::chrono::steady_clock::now()
                    .time_since_epoch().count();
            m_path = std::filesystem::temp_directory_path()
                / (L"LamaPonRenderFailureTests-"
                    + std::to_wstring(unique));
            std::filesystem::create_directories(
                m_path / L"shaders");
        }

        // 一時ディレクトリと内容を削除する。
        ~TemporaryDirectory()
        {
            // 削除失敗を例外にしない受け皿
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
        }

        // TemporaryDirectory(other: 複製元): 一時領域の複製を禁止する。
        TemporaryDirectory(const TemporaryDirectory&) = delete;
        // operator=(other: 複製元): 一時領域の代入を禁止する。
        TemporaryDirectory& operator=(
            const TemporaryDirectory&) = delete;

        // Path(): テスト用一時領域の場所を返す。
        [[nodiscard]] const std::filesystem::path&
            Path() const noexcept
        {
            return m_path;
        }

    private:
        // テスト用一時ディレクトリ
        std::filesystem::path m_path;
    };

    class ThrowOnceRenderComponent final
        : public LamaPon::Component
    {
    public:
        enum class Failure
        {
            None,
            Standard,
            NonStandard
        };

        // FailNext(failure: 次回失敗種別): 次の描画時に指定例外を一度だけ発生させる。
        void FailNext(const Failure failure) noexcept
        {
            m_failure = failure;
        }

    protected:
        // OnRender3D(): 設定済みの一度限りの描画失敗を発生させる。
        void OnRender3D(
            DirectX::FXMMATRIX,
            DirectX::CXMMATRIX) override
        {
            // 次回描画に設定された失敗種別
            const auto failure = m_failure;
            m_failure = Failure::None;
            // 標準例外の描画失敗を注入する。
            if (failure == Failure::Standard)
            {
                throw std::runtime_error(
                    "Injected probe render failure.");
            }
            // 非標準例外の描画失敗を注入する。
            if (failure == Failure::NonStandard)
            {
                throw 42;
            }
        }

    private:
        // 次回描画へ注入する失敗種別
        Failure m_failure{};
    };

    struct OutputBindingSnapshot final
    {
        // 現在のカラー出力先
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> color;
        // 現在の深度出力先
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth;
        // 現在のビューポート
        D3D11_VIEWPORT viewport{};
        // ビューポート取得結果
        bool hasViewport{};
    };

    // CaptureOutputBinding(context: 描画コンテキスト): 出力先とビューポートを取得する。
    [[nodiscard]] OutputBindingSnapshot CaptureOutputBinding(
        ID3D11DeviceContext* const context)
    {
        // 取得した出力先の状態
        OutputBindingSnapshot snapshot;
        context->OMGetRenderTargets(
            1,
            snapshot.color.ReleaseAndGetAddressOf(),
            snapshot.depth.ReleaseAndGetAddressOf());
        // 読み取るビューポート数
        UINT viewportCount = 1;
        context->RSGetViewports(
            &viewportCount,
            &snapshot.viewport);
        snapshot.hasViewport = viewportCount > 0;
        return snapshot;
    }

    // RequireSameOutputBinding(expected: 期待値, actual: 実値, message: 失敗理由): 描画出力先が復元されたか確認する。
    void RequireSameOutputBinding(
        const OutputBindingSnapshot& expected,
        const OutputBindingSnapshot& actual,
        const char* const message)
    {
        // ビューポートが一致するか
        const bool sameViewport =
            expected.hasViewport == actual.hasViewport
            && (!expected.hasViewport
                || (expected.viewport.TopLeftX
                        == actual.viewport.TopLeftX
                    && expected.viewport.TopLeftY
                        == actual.viewport.TopLeftY
                    && expected.viewport.Width
                        == actual.viewport.Width
                    && expected.viewport.Height
                        == actual.viewport.Height
                    && expected.viewport.MinDepth
                        == actual.viewport.MinDepth
                    && expected.viewport.MaxDepth
                        == actual.viewport.MaxDepth));
        Require(
            expected.color.Get() == actual.color.Get()
                && expected.depth.Get() == actual.depth.Get()
                && sameViewport,
            message);
    }

    // WriteFile(path: 出力先, contents: ファイル内容): テストシェーダーを保存する。
    void WriteFile(
        const std::filesystem::path& path,
        const std::string& contents)
    {
        // 作成するシェーダーファイル
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        output << contents;
        // 書き込み失敗を検出する。
        if (!output)
        {
            throw std::runtime_error(
                "Could not write the test shader.");
        }
    }

    // FailureOf(callable: 検査処理): 例外文を返し、例外がなければ空文字列を返す。
    template <typename Callable>
    [[nodiscard]] std::string FailureOf(Callable&& callable)
    {
        // 検査処理を実行する。
        try
        {
            callable();
            return {};
        }
        // 標準例外の診断文を返す。
        catch (const std::exception& exception)
        {
            return exception.what();
        }
    }
}

// main(): Shader再試行、Scene描画復旧、出力先復元を検証する。
int main()
{
    // COM初期化結果
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // COM終了処理が必要か
    const bool uninitialize = SUCCEEDED(comResult);

    // 検査の終了状態
    int status = 0;
    // 作成した非表示ウィンドウ
    HWND window = nullptr;
    // 検査失敗を終了状態へ変換する。
    try
    {
        // shader素材を置くテスト用一時領域
        TemporaryDirectory root;
        // 壊れた入力で差し替える組み込みshader
        const auto shader =
            root.Path() / L"shaders" / L"LamaPonEnvironment.hlsl";
        // 構文エラーで未キャッシュのshader失敗を再現する。
        WriteFile(
            shader,
            "float4 VSMain() : SV_Position"
            " { return not_a_function(); }\n");

        Stage("window");
        // 作成した非表示ウィンドウ
        window = CreateHiddenWindow();
        Require(
            window != nullptr,
            "the hidden window must be created");

        // GPUのないCIでも動かすためWARPを選択する。
        LamaPon::GraphicsDevice::SetPreferWarpAdapter(true);

        // 描画検証に使うグラフィックスデバイス
        LamaPon::GraphicsDevice graphics;
        Stage("initialize");
        graphics.Initialize(window, Width, Height);
        graphics.Assets().SetAssetRoot(root.Path());
        // 描画失敗を再試行する環境描画処理
        const auto initializeEnvironment = [&graphics]
        {
            // 全軸を向く環境描画行列
            const auto identity = DirectX::XMMatrixIdentity();
            graphics.DrawSky(
                identity,
                identity,
                LamaPon::SkySettings{});
        };

        Stage("first-failure");
        LamaPon::ResetShaderCompileStatistics();
        // 最初のコンパイル失敗の診断文
        const auto first =
            FailureOf(initializeEnvironment);
        Require(
            !first.empty(),
            "a broken built-in shader must still throw");
        // 初回試行でコンパイルされたshader数
        const auto afterFirst =
            LamaPon::ShaderCompileStatistics().compiledCount;
        Require(
            afterFirst > 0,
            "the first attempt must actually compile");

        Stage("latched");
        // frame: 抑制中の再試行を確認するフレーム番号。
        for (int frame = 0; frame < 5; ++frame)
        {
            // 抑制期間中に返された失敗
            const auto repeated =
                FailureOf(initializeEnvironment);
            Require(
                repeated == first,
                "the remembered failure must be returned as-is");
        }
        Require(
            LamaPon::ShaderCompileStatistics().compiledCount
                == afterFirst,
            "a remembered failure must not recompile");

        // フォールバック描画数とplaceholder取得を照合する。
        Stage("fallback-count");
        graphics.ResetShaderFallbackDraws();
        Require(
            graphics.FrameStats().shaderFallbackDraws == 0,
            "resetting must zero the fallback count");
        // 要求したshader placeholder
        const auto* const placeholder =
            graphics.ShaderErrorPlaceholder(false);
        // placeholder未配置環境では取得数が増えない。
        Require(
            (placeholder != nullptr)
                == (graphics.FrameStats()
                        .shaderFallbackDraws
                    > 0),
            "the count must rise only when a placeholder"
            " is actually handed out");
        // placeholderを取得した時点の回数
        // placeholderがある場合だけ取得回数の増加を確認する。
        if (placeholder != nullptr)
        {
            // 1回目の取得後の回数
            const auto once =
                graphics.FrameStats().shaderFallbackDraws;
            static_cast<void>(graphics.ShaderErrorPlaceholder(false));
            Require(
                graphics.FrameStats().shaderFallbackDraws
                    == once + 1,
                "every hand-out must be counted, not just"
                " the first one");
        }
        graphics.ResetShaderFallbackDraws();

        // 本物の組み込みHLSLと依存includeを置いて復旧させる。
        Stage("recover");
        // エンジン付属shaderの格納先
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
        // 失敗抑制の再試行間隔を超えるまで待つ。
        std::this_thread::sleep_for(
            std::chrono::milliseconds(2500));
        // shader修復後の再試行結果
        const auto recovered =
            FailureOf(initializeEnvironment);
        Require(
            recovered.empty(),
            "fixing the shader must bring rendering back");

        // 例外後に出力先と再入状態を復旧する。
        // 標準例外はReflection、非標準例外はfail-soft GIで確認する。
        Stage("probe-output-recovery");
        // 描画前に設定する背景色
        constexpr float clearColor[]{
            0.01f, 0.02f, 0.03f, 1.0f
        };
        graphics.BeginFrame(clearColor);
        // フレーム開始時の出力先状態
        const auto primaryOutput =
            CaptureOutputBinding(D3D11Access::Context(graphics));

        // 例外検査用のScene
        LamaPon::Scene probeScene(graphics);
        // Reflection probeを持つSceneオブジェクト
        auto& probeObject =
            probeScene.CreateGameObject("Failure probe");
        // バイク処理対象のReflection probe
        auto& probe = probeObject.AddComponent<
            LamaPon::ReflectionProbeComponent>();
        // 一度だけ例外を投げるSceneオブジェクト
        auto& throwObject =
            probeScene.CreateGameObject("Throw once");
        // 描画時に例外を注入するコンポーネント
        auto& thrower = throwObject.AddComponent<
            ThrowOnceRenderComponent>();

        thrower.FailNext(
            ThrowOnceRenderComponent::Failure::Standard);
        // Reflection bake中に投げた例外
        const auto reflectionFailure = FailureOf(
            [&]
            {
                probeScene.RenderMainCamera(
                    graphics.AspectRatio(),
                    false);
            });
        Require(
            reflectionFailure
                    == "Injected probe render failure."
                && probe.IsBakeRequested(),
            "reflection probe failure must propagate and remain retryable");
        RequireSameOutputBinding(
            primaryOutput,
            CaptureOutputBinding(D3D11Access::Context(graphics)),
            "reflection probe failure leaked its output binding");

        // 例外後のReflection bake再試行結果
        const auto reflectionRetry = FailureOf(
            [&]
            {
                probeScene.RenderMainCamera(
                    graphics.AspectRatio(),
                    false);
            });
        Require(
            reflectionRetry.empty() && probe.IsBaked(),
            "reflection probe failure left the bake guard active");

        // GI bake設定
        auto gi = probeScene.BakedGlobalIllumination();
        gi.enabled = true;
        gi.resolutionX = 1;
        gi.resolutionY = 1;
        gi.resolutionZ = 1;
        probeScene.SetBakedGlobalIlluminationSettings(gi);
        probeScene.RequestBakedGlobalIlluminationBake();
        thrower.FailNext(
            ThrowOnceRenderComponent::Failure::NonStandard);
        // 非標準例外がScene呼び出しへ漏れたか
        bool giFailureEscaped{};
        // 非標準例外を投げたGI bakeを実行する。
        try
        {
            probeScene.RenderMainCamera(
                graphics.AspectRatio(),
                false);
        }
        // GI経路のfail-soft動作を確認する。
        catch (...)
        {
            giFailureEscaped = true;
        }
        Require(
            !giFailureEscaped
                && probeScene
                    .BakedGlobalIlluminationBakeProgress() < 0.0f,
            "GI bake must fail softly for a non-standard render error");
        RequireSameOutputBinding(
            primaryOutput,
            CaptureOutputBinding(D3D11Access::Context(graphics)),
            "GI probe failure leaked its output binding");

        probeScene.RequestBakedGlobalIlluminationBake();
        // GI bakeの再試行結果
        const auto giRetry = FailureOf(
            [&]
            {
                probeScene.RenderMainCamera(
                    graphics.AspectRatio(),
                    false);
            });
        Require(
            giRetry.empty()
                && probeScene.HasBakedGlobalIllumination(),
            "GI probe failure left the shared bake guard active");
        RequireSameOutputBinding(
            primaryOutput,
            CaptureOutputBinding(D3D11Access::Context(graphics)),
            "successful probe retry did not restore its output binding");

        // 例外後に描画先とUI基準サイズを戻す。
        // 同じSceneを再試行し、再入ガードの解除も確認する。
        Stage("render-target-output-recovery");
        // レンダーターゲット処理前のUI幅
        const auto primaryUIWidth = graphics.UIWidth();
        // レンダーターゲット処理前のUI高さ
        const auto primaryUIHeight = graphics.UIHeight();
        // レンダーターゲット検査用Scene
        LamaPon::Scene targetScene(graphics);
        // 出力テクスチャを持つカメラのSceneオブジェクト
        auto& cameraObject =
            targetScene.CreateGameObject("Target camera");
        // 独自レンダーターゲットを使うカメラ
        auto& targetCamera = cameraObject.AddComponent<
            LamaPon::CameraComponent>();
        targetCamera.SetTargetTexture("failure-target");
        targetCamera.SetTargetTextureSize(17, 19);
        // 例外を一度投げるターゲットSceneオブジェクト
        auto& targetThrowObject =
            targetScene.CreateGameObject("Target throw once");
        // 例外を注入するターゲット描画コンポーネント
        auto& targetThrower = targetThrowObject.AddComponent<
            ThrowOnceRenderComponent>();
        targetThrower.FailNext(
            ThrowOnceRenderComponent::Failure::Standard);

        // レンダーターゲット描画の失敗診断
        const auto targetFailure = FailureOf(
            [&] { targetScene.RenderTargetTextures(); });
        Require(
            targetFailure == "Injected probe render failure.",
            "render target failure must propagate");
        RequireSameOutputBinding(
            primaryOutput,
            CaptureOutputBinding(D3D11Access::Context(graphics)),
            "render target failure leaked its output binding");
        Require(
            graphics.UIWidth() == primaryUIWidth
                && graphics.UIHeight() == primaryUIHeight,
            "render target failure leaked its UI viewport size");

        // レンダーターゲット描画の再試行結果
        const auto targetRetry = FailureOf(
            [&] { targetScene.RenderTargetTextures(); });
        Require(
            targetRetry.empty(),
            "render target failure left its re-entry guard active");
        RequireSameOutputBinding(
            primaryOutput,
            CaptureOutputBinding(D3D11Access::Context(graphics)),
            "render target retry did not restore its output binding");
        Require(
            graphics.UIWidth() == primaryUIWidth
                && graphics.UIHeight() == primaryUIHeight,
            "render target retry did not restore its UI viewport size");

        // 例外後もProfiler区間を閉じ、新区間を深さ0で開始する。
        {
            // 復旧確認用のGPU計測区間
            LamaPon::GpuProfiler::SectionScope marker{
                graphics.Gpu(),
                "probe failure recovery marker"
            };
            Require(
                marker.InitialDepth() == 0,
                "probe failure leaked a nested GPU profiler section");
        }
        graphics.EndFrame();

        std::cout << "Render failure checks passed."
                  << std::endl;
    }
    // 例外(exception: 検査失敗情報)を標準エラーへ出力する。
    catch (const std::exception& exception)
    {
        std::cerr << "Render failure check failed: "
                  << exception.what() << std::endl;
        status = 1;
    }

    // 作成済みの非表示ウィンドウを破棄する。
    if (window != nullptr)
    {
        DestroyWindow(window);
    }
    // COM初期化に成功した場合だけ終了処理する。
    if (uninitialize)
    {
        CoUninitialize();
    }
    return status;
}
