#pragma once

#include <d3dcommon.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace LamaPon
{
    class AssetManager;

    // HLSLをコンパイルします。同じ内容を前に一度コンパイルしていれば、
    // ディスクへ保存したバイトコードを読むだけで済ませます。
    //
    // なぜキャッシュが要るか: このエンジンはシェーダーをHLSLのまま
    // 配布し、実行時にコンパイルします（だからホットリロードができ、
    // 自作Shaderをユーザーが書けます）。ただし素直に書くと「起動の
    // たびに全部コンパイルし直す」ことになり、LamaPonLit.hlslのように
    // Forward+・影・SSR・TAAを全部含む大きなシェーダーではそれが
    // 起動時間としてまるごと乗ります。
    //
    // キャッシュのキーはHLSL本体と#includeした全ファイルの中身の
    // ハッシュなので、1文字でも直せば自動的に作り直されます。
    // ホットリロードはこれまでどおり動きます。
    //
    // 失敗したときはstd::runtime_errorを投げます（コンパイラの
    // メッセージ入り）。キャッシュの読み書きに失敗しても投げません。
    // その場合は普通にコンパイルするだけです。キャッシュはあくまで
    // 速さのための仕組みで、無くても動くのが正しい姿です。
    // definesはバリアントのキーワードです（`FOG_ON`のように渡すと
    // `#define FOG_ON 1` になります）。キャッシュのキーにも入るので、
    // 組み合わせごとに別々に保存されます。
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob>
        CompileShaderCached(
            AssetManager& assets,
            const std::filesystem::path& path,
            const char* entryPoint,
            const char* target,
            const std::vector<std::string>& defines = {});

    // バイトコードをディスクキャッシュへ用意するだけの入口です。
    // GPUオブジェクト（ID3D11VertexShader等）は作らないので、
    // **別スレッドから呼べます**。エディターの非同期コンパイルは
    // これをワーカーで回し、出来上がってからメインスレッドで
    // LitEffectを組み立てます（そのときはキャッシュに当たるので
    // 一瞬で終わります）。
    //
    // 例外は投げません。入口が無いシェーダーは失敗もキャッシュへ
    // 残るので、あとで試し直されることはありません。
    //
    // 注意: AssetManagerからの読み取りがスレッド安全なのは、
    // 素のファイル（エディター）を読むときだけです。アーカイブ
    // （書き出したゲーム）では呼ばないでください。
    void WarmShaderCache(
        AssetManager& assets,
        const std::filesystem::path& path,
        const std::vector<std::string>& keywords);

    // 事前コンパイルの索引を書き出します。
    //
    // ふだんのキャッシュのキーは「HLSLソースの中身のハッシュ」です。
    // ソースが手元にあれば1文字の違いも見分けられて都合が良いのですが、
    // **配布物からHLSLを外すとキーそのものが計算できなくなります**。
    // そこで書き出し時に「パス＋入口＋ターゲット＋キーワード」から
    // キーを引ける索引も一緒に置きます。実行時はまずソースから
    // キーを作り、ソースが無ければこの索引を見ます。
    void WriteShaderCacheIndex(
        const std::filesystem::path& directory);

    // キャッシュの置き場所
    // （%LOCALAPPDATA%\LamaPon\shader-cache）。
    // プロジェクトごとに分けていないのは、キーが中身のハッシュなので
    // 分ける必要がないためです。同じエンジンのシェーダーを使う
    // プロジェクト同士でキャッシュを共有できます。
    [[nodiscard]] std::filesystem::path ShaderCacheDirectory();

    // 読み取り専用のキャッシュを足します。書き出したゲームへ同梱した
    // 事前コンパイル済みバイトコード（exeの隣のshader-cache）を
    // 読むための口です。ここへは書き込みません。書き込みは常に
    // ShaderCacheDirectory()の側だけで、配布フォルダーが書き込み
    // 不可の場所（Program Files配下など）にあっても困らないように
    // しています。
    void AddShaderCacheSearchDirectory(
        std::filesystem::path directory);
    void ClearShaderCacheSearchDirectories();

    // 事前コンパイルで試す入口の一覧。エンジンが「あれば使う」方式で
    // 探すものを全部含みます。書き出し時にこれを総当たりし、成功も
    // 失敗もキャッシュへ残しておくと、プレイヤーの初回起動でも
    // コンパイルが1本も走りません。
    struct ShaderEntryPoint final
    {
        const char* entryPoint;
        const char* target;
    };

    [[nodiscard]] const std::vector<ShaderEntryPoint>&
        KnownShaderEntryPoints();

    // pathのシェーダーについて、既知の入口を総当たりでコンパイルし、
    // 結果をdestinationDirectoryへ書きます。戻り値は成功した本数。
    // 失敗は例外にしません（入口が無いのは正常なため）。
    // usedKeywordsを渡すと、shader_featureのバリアントを
    // 「実際に使われているもの」だけへ絞ります。nullptrなら全組み合わせを
    // コンパイルします。
    std::uint32_t PrecompileShader(
        AssetManager& assets,
        const std::filesystem::path& path,
        const std::filesystem::path& destinationDirectory,
        const std::vector<std::string>& defines = {},
        const std::vector<std::string>* usedKeywords = nullptr);

    // 起動からのコンパイル状況。計測とテスト用です。
    struct ShaderCompileStats final
    {
        // 実際にD3DCompileを呼んだ回数。
        std::uint32_t compiledCount{};
        // キャッシュから読めた回数。
        std::uint32_t cacheHitCount{};
        // D3DCompileに費やした合計ミリ秒。
        double compileMilliseconds{};
        // キャッシュの読み込みに費やした合計ミリ秒。
        double cacheReadMilliseconds{};
    };

    [[nodiscard]] ShaderCompileStats ShaderCompileStatistics() noexcept;
    void ResetShaderCompileStatistics() noexcept;

    // キャッシュを消します。テストと、疑わしいときの手動リセット用。
    void ClearShaderCache();
    // 覚えている「失敗」だけを消します（バイトコードは残します）。
    // 戻り値は消した件数。
    //
    // 失敗を覚えてよい条件は絞ってありますが（入口が無いときだけ）、
    // それでも「キャッシュのせいで開けない」を疑う場面のために、
    // 捨てる手段を1つ用意しておきます。セーフモードで起動したときに
    // 呼ばれます——前回落ちた原因が分からない状態なので、**起動を
    // 妨げ得るものは全部捨てて**から開くのが安全側です。
    // バイトコードを残すのは、成功した結果が起動を妨げることは無く、
    // 捨てると次の起動が全部コンパイルからになるためです。
    std::size_t ClearShaderCacheFailures();
    // 切るとキャッシュを読みも書きもしません（前後比較の計測用）。
    void SetShaderCacheEnabled(bool enabled) noexcept;
    [[nodiscard]] bool IsShaderCacheEnabled() noexcept;
}
