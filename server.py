#!/usr/bin/env python3
import http.server
import os

PORT = int(os.environ.get("PORT", "10000"))
REPORT = "/app/report.txt"

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        try:
            with open(REPORT, "r") as f:
                content = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.end_headers()
            self.wfile.write(content.encode())
        except Exception as e:
            self.send_response(500)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(f"Error: {e}\n".encode())

    def log_message(self, fmt, *args):
        pass

if __name__ == "__main__":
    server = http.server.HTTPServer(("0.0.0.0", PORT), Handler)
    print(f"Serving report on port {PORT}")
    server.serve_forever()
