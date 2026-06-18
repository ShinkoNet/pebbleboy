#!/usr/bin/env python3
"""Serve one ROM at a local URL in a pypkjs-safe text encoding."""

from __future__ import annotations

import argparse
import base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import quote, unquote, urlparse


MARKER = b"PEBBLEBOY_ROM_BASE64\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("port", type=int)
    parser.add_argument("rom_file", type=Path)
    parser.add_argument("--host", default="127.0.0.1")
    args = parser.parse_args()

    rom_file = args.rom_file.resolve()
    rom_name = rom_file.name
    rom_path = "/" + quote(rom_name)
    payload = MARKER + base64.b64encode(rom_file.read_bytes())

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):  # noqa: N802 - BaseHTTPRequestHandler API
            path = unquote(urlparse(self.path).path)
            if path != rom_path and path != "/" + rom_name:
                self.send_response(404)
                self.end_headers()
                return

            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=us-ascii")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def log_message(self, fmt, *args):
            print("%s - %s" % (self.address_string(), fmt % args), flush=True)

    print(f"serving {rom_file} at http://{args.host}:{args.port}{rom_path}", flush=True)
    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
