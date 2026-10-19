#!/usr/bin/env python3
"""Serve the browser build locally with the isolation required by WASM threads."""
import argparse
import functools
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class IsolatedHandler(SimpleHTTPRequestHandler):
    extensions_map = {**SimpleHTTPRequestHandler.extensions_map, '.wasm': 'application/wasm'}

    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=ROOT / 'build/web/engine')
    parser.add_argument('--bind', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8000)
    args = parser.parse_args()
    directory = args.directory.resolve()
    if not (directory / 'index.html').is_file():
        parser.error(f'{directory}/index.html missing; run make web-warcraft3-engine first')
    handler = functools.partial(IsolatedHandler, directory=str(directory))
    with ThreadingHTTPServer((args.bind, args.port), handler) as server:
        print(f'OpenRealm: http://{args.bind}:{server.server_port}/ ({directory})', flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == '__main__':
    main()
