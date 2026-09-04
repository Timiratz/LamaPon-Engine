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


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--browser", required=True)
    parser.add_argument("--html", required=True, type=Path)
    parser.add_argument("--runtime", action="store_true",
                        help="Check the portable game's running status")
    args = parser.parse_args()
    html = args.html.resolve()
    handler = partial(SimpleHTTPRequestHandler, directory=str(html.parent))
    with ThreadingHTTPServer(("127.0.0.1", 0), handler) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory(prefix="lamapon-web-test-", ignore_cleanup_errors=True) as profile:
                result = subprocess.run([
                    args.browser, "--headless", "--disable-gpu",
                    "--no-first-run", "--no-default-browser-check",
                    "--enable-logging=stderr",
                    f"--user-data-dir={profile}", "--virtual-time-budget=10000",
                    "--dump-dom", f"http://127.0.0.1:{server.server_port}/{html.name}",
                ], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)
                status = BodyStatus()
                status.feed(result.stdout)
                attribute, expected = (("data-lamapon-status", "running") if args.runtime
                                       else ("data-test-status", "passed"))
                if result.returncode or status.attributes.get(attribute) != expected:
                    failure_page = html.with_suffix(".browser-failure.html")
                    failure_page.write_text(result.stdout, encoding="utf-8")
                    raise RuntimeError(f"Browser exit code: {result.returncode}\n"
                                       + f"Body: {status.attributes}\nDOM saved to: {failure_page}\n"
                                       + result.stderr[-4000:])
        finally:
            server.shutdown()
            thread.join()
    print("Browser runtime startup passed." if args.runtime
          else "Browser lifecycle and logging tests passed.")


if __name__ == "__main__":
    main()
