"""Run a local Wasm regression page in a Chromium-compatible browser."""
from __future__ import annotations

import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from html.parser import HTMLParser
from pathlib import Path
import subprocess
import tempfile
import threading


class BodyStatus(HTMLParser):
    # 初期化時に結果属性を空にする
    # __init__(self: HTMLパーサー)
    def __init__(self):
        super().__init__()
        # body要素から読み取った属性
        self.attributes = {}

    # body開始タグの属性を保存します。
    # handle_starttag(self: HTMLパーサー, tag: タグ名, attrs: 属性一覧)
    def handle_starttag(self, tag, attrs):
        # body開始タグだけ状態として保持
        if tag == "body":
            self.attributes = dict(attrs)


# Wasm初期化中のbody属性値
UNFINISHED = {"data-test-status": "pending", "data-lamapon-status": "loading"}
# ブラウザー検査の最大試行回数
ATTEMPTS = 3


# ブラウザーを一時プロファイルで起動し、DOM出力を返します。
# run_browser(browser: 実行ファイル, url: 対象ページ, timeout: 制限秒数)
def run_browser(browser: str, url: str,
                timeout: int) -> subprocess.CompletedProcess:
    # profile: Chromiumの一時ユーザーデータ領域
    with tempfile.TemporaryDirectory(prefix="lamapon-web-test-", ignore_cleanup_errors=True) as profile:
        return subprocess.run([
            browser, "--headless", "--disable-gpu",
            "--no-first-run", "--no-default-browser-check",
            "--enable-logging=stderr",
            f"--user-data-dir={profile}", "--virtual-time-budget=10000",
            "--dump-dom", url,
        ], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout)


def main() -> None:
    """ローカル配信したWebページの起動状態を検査します。"""
    # コマンドライン引数の定義
    parser = argparse.ArgumentParser()
    parser.add_argument("--browser", required=True)
    parser.add_argument("--html", required=True, type=Path)
    parser.add_argument("--runtime", action="store_true",
                        help="Check the portable game's running status")
    # 検査対象と実行モード
    args = parser.parse_args()
    # 配信するHTMLファイルの絶対パス
    html = args.html.resolve()
    # HTMLと同じディレクトリを公開する要求ハンドラー
    handler = partial(SimpleHTTPRequestHandler, directory=str(html.parent))
    # 確認するbody属性名と成功値
    attribute, expected = (("data-lamapon-status", "running") if args.runtime
                           else ("data-test-status", "passed"))
    # ループバックの一時HTTPサーバー
    with ThreadingHTTPServer(("127.0.0.1", 0), handler) as server:
        # サーバー処理を担当するバックグラウンドスレッド
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        # 配信とブラウザー検査を実行
        try:
            # 起動したサーバー上の対象ページ
            url = f"http://127.0.0.1:{server.server_port}/{html.name}"
            # attempt: 初期化待ちを含むブラウザー試行回数
            for attempt in range(1, ATTEMPTS + 1):
                # ブラウザーの終了状態とDOM
                result = run_browser(args.browser, url, timeout=60)
                # body属性を取得する解析器
                status = BodyStatus()
                status.feed(result.stdout)
                # 期待判定に使う実際の属性値
                actual = status.attributes.get(attribute)
                # 正常終了かつ期待値なら検査完了
                if not result.returncode and actual == expected:
                    break
                # 失敗確定のページは再試行せずDOMを保存
                # actualが初期値なら初期化待ちとして再試行
                unfinished = (actual == UNFINISHED[attribute]
                              or actual is None)
                # 最終試行または明確な失敗はDOMを保存する
                if attempt == ATTEMPTS or not unfinished:
                    # 失敗時に調査できるDOM出力先
                    failure_page = html.with_suffix(".browser-failure.html")
                    failure_page.write_text(result.stdout, encoding="utf-8")
                    raise RuntimeError(f"Browser exit code: {result.returncode}\n"
                                       + f"Body: {status.attributes}\n"
                                       + f"Attempts: {attempt}/{ATTEMPTS}\n"
                                       + f"DOM saved to: {failure_page}\n"
                                       + result.stderr[-4000:])
                print(f"{html.name}: {attribute}={actual} "
                      f"（{attempt}/{ATTEMPTS}回目、初期化が終わっていないので再試行します）")
        # 成否にかかわらずHTTPサーバーを停止
        finally:
            server.shutdown()
            thread.join()
    print("Browser runtime startup passed." if args.runtime
          else "Browser lifecycle and logging tests passed.")


# 直接実行時だけブラウザー検査を開始
if __name__ == "__main__":
    main()
