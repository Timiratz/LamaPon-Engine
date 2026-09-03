# LamaPon 開発メモ

LamaPon は C++20・DirectX 11 を使う Windows 向けゲームエンジンです。
軽さ・速さ・安定性と、C++ を学びながらゲームを作れることを大切にします。

## 開発環境

[CONTRIBUTING.md](CONTRIBUTING.md) の手順で、x64 Visual Studio 開発者プロンプトから CMake プリセットを実行します。
`windows-debug` と `windows-release` が利用できます。テストは `ctest --preset windows-release` です。
GitHub のリポジトリは `Timiratz/LamaPon-Engine` です。

## コードの構成

- `src/LamaPon/` — Runtime、Editor、Hub と公開 API。
- `tools/LamaPonCli/` — コマンドライン操作。
- `tools/ProjectGameModule/` — プロジェクトの C++ Script ビルド。
- `assets/`、`samples/`、`tests/` — 組み込みアセット、教材、回帰テスト。
- `.lamapon/project.json` — このリポジトリに含むサンプルの設定。

## 変更時の確認

- API の変更は `src`、`tests`、`samples`、`tools`、`assets/packages` の利用箇所へ反映します。
- Runtime の ABI を変える場合は Game Module API 版と再ビルドの必要性を確認します。
- 新しいソースは `CMakeLists.txt` へ登録します。
- 日本語と UTF-8 のパスを維持し、描画変更ではレンダーテストも実行します。
- ビルド・テストの失敗を確認し、生成物・ログ・個人設定をコミットしません。
- タグのプッシュでリリースが作られるため、リリース作業時だけ実行します。

使い方は [docs/index.md](docs/index.md)、配布手順は [docs/project.md](docs/project.md) を参照してください。
