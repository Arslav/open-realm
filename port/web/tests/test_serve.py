"""Verify the real development server's browser isolation and runtime responses."""
import functools
import http.client
import importlib.util
from pathlib import Path
import tempfile
import threading
import unittest
from http.server import ThreadingHTTPServer

SCRIPT = Path(__file__).resolve().parents[1] / 'serve.py'
spec = importlib.util.spec_from_file_location('web_serve', SCRIPT)
server = importlib.util.module_from_spec(spec)
spec.loader.exec_module(server)


class ServeTest(unittest.TestCase):
    def test_runtime_and_errors_receive_isolation_headers(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'index.html').write_text('<title>OpenRealm</title>')
            (root / 'index.wasm').write_bytes(b'wasm fixture')
            handler = functools.partial(server.IsolatedHandler, directory=folder)
            with ThreadingHTTPServer(('127.0.0.1', 0), handler) as httpd:
                thread = threading.Thread(target=httpd.serve_forever, daemon=True)
                thread.start()
                try:
                    for path, expected in (('/', 200), ('/index.wasm', 200), ('/missing.js', 404)):
                        connection = http.client.HTTPConnection(*httpd.server_address)
                        connection.request('GET', path)
                        response = connection.getresponse()
                        self.assertEqual(response.status, expected)
                        self.assertEqual(response.getheader('Cross-Origin-Opener-Policy'), 'same-origin')
                        self.assertEqual(response.getheader('Cross-Origin-Embedder-Policy'), 'require-corp')
                        self.assertEqual(response.getheader('Cache-Control'), 'no-store')
                        if path.endswith('.wasm'):
                            self.assertEqual(response.getheader('Content-Type'), 'application/wasm')
                            self.assertEqual(response.read(), b'wasm fixture')
                        else:
                            response.read()
                        connection.close()
                finally:
                    httpd.shutdown()
                    thread.join()


if __name__ == '__main__':
    unittest.main()
