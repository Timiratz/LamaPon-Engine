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
    def __init__(self):
        super().__init__()
        self.attributes = {}

    def handle_starttag(self, tag, attrs):
        if tag == "body":
            self.attributes = dict(attrs)


# ページが結果を書き込む前の初期値です。--virtual-time-budgetが
# 早送りするのはタイマーだけで、Wasmのコンパイルと初期化にかかる実時間は
# 早送りされません。CIのrunnerが混んでいると初期化が終わる前にDOMを
# 吸い出してしまい、初期値のまま失敗します。この値だけは待ち不足として
# 扱い、もう一度ブラウザを起動して確かめます。
UNFINISHED = {"data-test-status": "pending", "data-lamapon-status": "loading"}
ATTEMPTS = 3


def run_browser(browser: str, url: str,
                timeout: int) -> subprocess.CompletedProcess:
    with tempfile.TemporaryDirectory(prefix="lamapon-web-test-", ignore_cleanup_errors=True) as profile:
        return subprocess.run([
            browser, "--headless", "--disable-gpu",
            "--no-first-run", "--no-default-browser-check",
            "--enable-logging=stderr",
            f"--user-data-dir={profile}", "--virtual-time-budget=10000",
            "--dump-dom", url,
        ], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--browser", required=True)
    parser.add_argument("--html", required=True, type=Path)
    parser.add_argument("--runtime", action="store_true",
                        help="Check the portable game's running status")
    args = parser.parse_args()
    html = args.html.resolve()
    handler = partial(SimpleHTTPRequestHandler, directory=str(html.parent))
    attribute, expected = (("data-lamapon-status", "running") if args.runtime
                           else ("data-test-status", "passed"))
    with ThreadingHTTPServer(("127.0.0.1", 0), handler) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            url = f"http://127.0.0.1:{server.server_port}/{html.name}"
            for attempt in range(1, ATTEMPTS + 1):
                result = run_browser(args.browser, url, timeout=60)
                status = BodyStatus()
                status.feed(result.stdout)
                actual = status.attributes.get(attribute)
                if not result.returncode and actual == expected:
                    break
                # ページが失敗を報告したのなら本物の不合格です。やり直しても
                # 結果は変わらないので、ここで止めて落ちたDOMを残します。
                unfinished = (actual == UNFINISHED[attribute]
                              or actual is None)
                if attempt == ATTEMPTS or not unfinished:
                    failure_page = html.with_suffix(".browser-failure.html")
                    failure_page.write_text(result.stdout, encoding="utf-8")
                    raise RuntimeError(f"Browser exit code: {result.returncode}\n"
                                       + f"Body: {status.attributes}\n"
                                       + f"Attempts: {attempt}/{ATTEMPTS}\n"
                                       + f"DOM saved to: {failure_page}\n"
                                       + result.stderr[-4000:])
                print(f"{html.name}: {attribute}={actual} "
                      f"（{attempt}/{ATTEMPTS}回目、初期化が終わっていないので再試行します）")
        finally:
            server.shutdown()
            thread.join()
    print("Browser runtime startup passed." if args.runtime
          else "Browser lifecycle and logging tests passed.")


if __name__ == "__main__":
    main()
