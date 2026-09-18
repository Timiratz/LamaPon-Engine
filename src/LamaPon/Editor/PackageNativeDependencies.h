#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    // 1つのパッケージが持ち込むネイティブ依存です。
    // package.jsonの"native"に、パッケージフォルダーからの相対パスで
    // 書きます。ここで保持するのは検証済みの絶対パスです。
    //
    //   {
    //     "name": "my-sdk",
    //     "native": {
    //       "includeDirectories": [ "sdk/include" ],
    //       "libraries":          [ "sdk/lib/my_sdk.lib" ],
    //       "runtimeFiles":       [ "sdk/bin/my_sdk.dll" ],
    //       "defines":            [ "MY_SDK_ENABLED" ]
    //     }
    //   }
    //
    // コンパイラーやリンカーへ渡す任意のフラグは受け付けません。
    // ダウンロードしたパッケージがビルドコマンドを書き換えられると、
    // ソースを読んだだけでは分からない挙動を持ち込めるためです。
    struct PackageNativeDependency final
    {
        std::string packageName;
        std::filesystem::path packageDirectory;
        // Game Moduleのインクルードパスへ追加します。
        std::vector<std::filesystem::path> includeDirectories;
        // Game Moduleへリンクする.libです。
        std::vector<std::filesystem::path> libraries;
        // 書き出したゲームと編集中のプレイへ持っていく.dllです。
        std::vector<std::filesystem::path> runtimeFiles;
        // Game Moduleのコンパイル時マクロです。
        std::vector<std::string> defines;

        [[nodiscard]] bool Empty() const noexcept
        {
            return includeDirectories.empty()
                && libraries.empty()
                && runtimeFiles.empty()
                && defines.empty();
        }
    };

    // 1つのpackage.jsonに書ける件数の上限です。これを超える指定は
    // 設定ミスとして拒否します。
    inline constexpr std::size_t PackageNativeMaxEntries = 32u;
    inline constexpr std::size_t PackageNativePathMaxBytes = 256u;

    // package.jsonの"native"を検証して読みます。"native"が無ければ
    // 空を返します。危険または不正な指定は std::invalid_argument
    // です（絶対パス、"..", パッケージ外への脱出、拡張子違い、
    // 未知のキー、件数超過、不正なマクロ名）。
    //
    // ファイルの実在は確認しません。SDKを後からダウンロードして
    // 置く運用があるためで、実在の確認はビルドと書き出しの直前に
    // 行います。
    [[nodiscard]] PackageNativeDependency
        ParsePackageNativeDependency(
            std::string_view manifestJson,
            const std::filesystem::path& packageDirectory,
            std::string_view packageName);

    struct PackageNativeScan final
    {
        std::vector<PackageNativeDependency> packages;
        // 読めなかったpackage.jsonの理由です。1件壊れていても
        // 残りのパッケージは使えるようにします。
        std::vector<std::string> errors;
    };

    // assets/packages/* を走査して、ネイティブ依存を持つものだけを
    // 集めます。assets/packagesが無ければ空を返します。
    [[nodiscard]] PackageNativeScan ScanPackageNativeDependencies(
        const std::filesystem::path& assetRoot);

    // 宣言されたファイルが実際に置かれているかを確認します。
    // 足りなければ、どのパッケージの何が無いかを説明する
    // std::runtime_error を投げます（SDKの配置忘れが、リンカーの
    // 読みにくいエラーになる前に分かるようにします）。
    void RequirePackageNativeFiles(
        const std::vector<PackageNativeDependency>& packages);

    struct PackageNativeSelection final
    {
        // 宣言したファイルがすべて置かれているパッケージです。
        std::vector<PackageNativeDependency> available;
        // native設定を外したパッケージごとの、足りないファイルの
        // 説明です。呼び出し側が警告として表示します。
        std::vector<std::string> missing;
    };

    // SDK本体を利用者が後から置くパッケージのために、宣言したファイルが
    // 1つでも足りないパッケージはnative設定（definesを含む）ごと外します。
    // SDKの有無をマクロで分けたアダプターは、SDKなしのままビルドと
    // 書き出しができます。
    [[nodiscard]] PackageNativeSelection
        SelectAvailablePackageNativeDependencies(
            std::vector<PackageNativeDependency> packages);

    // 書き出したゲームへ持っていくDLLを、コピー先の名前つきで
    // 返します。エンジン自身のDLLと同じ名前は上書き事故になるため
    // 拒否します（std::runtime_error）。
    struct PackageRuntimeFile final
    {
        std::filesystem::path source;
        std::wstring fileName;
        std::string packageName;
    };

    [[nodiscard]] std::vector<PackageRuntimeFile>
        CollectPackageRuntimeFiles(
            const std::vector<PackageNativeDependency>& packages);

    // 編集中のプレイでGame Moduleが依存DLLを見つけられるよう、
    // runtimeFilesが置かれているフォルダーを重複なく返します。
    // GameModuleHost::SetNativeSearchDirectories へ渡します。
    [[nodiscard]] std::vector<std::filesystem::path>
        PackageNativeSearchDirectories(
            const std::vector<PackageNativeDependency>& packages);

    // Game ModuleのCMakeが読み込む設定ファイルを書き出します。
    // コマンドラインでリストを渡すとcmd.exeの引用符と衝突するため、
    // 生成したファイルをinclude()させます。ネイティブ依存が無い
    // ときも空の内容で書き出します（CMakeの再configure判定が
    // ファイルの有無で揺れないようにするためです）。
    void WritePackageNativeCMakeFile(
        const std::filesystem::path& outputPath,
        const std::vector<PackageNativeDependency>& packages);
}
