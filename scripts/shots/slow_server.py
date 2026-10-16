#!/usr/bin/env python3
"""Throttled static HTTP server for testing the pack download progress bar: slow_server.py PORT DIR [KB_PER_S]."""
import http.server, os, sys, time
port, root = int(sys.argv[1]), sys.argv[2]
rate = int(sys.argv[3]) * 1024 if len(sys.argv) > 3 else 40 * 1024 * 1024
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        p = os.path.join(root, os.path.basename(self.path))
        if not os.path.isfile(p): self.send_error(404); return
        self.send_response(200); self.send_header("Content-Length", str(os.path.getsize(p))); self.end_headers()
        with open(p, "rb") as f:
            while chunk := f.read(rate // 10):
                try: self.wfile.write(chunk)
                except BrokenPipeError: return
                time.sleep(0.1)
    def log_message(self, *a): pass
http.server.ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
