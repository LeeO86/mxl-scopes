#!/usr/bin/env python3
"""Minimal IS-04 registration and query stand-in for integration tests."""

import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

RESOURCES = {}
LOCK = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("%s\n" % (fmt % args))

    def _read(self):
        length = int(self.headers.get("Content-Length", "0") or 0)
        if length <= 0:
            return b""
        return self.rfile.read(length)

    def _send(self, status, body=b"", content_type="application/json"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        if body:
            self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path in ("/", "/x-nmos/registration/v1.3/", "/x-nmos/query/v1.3/"):
            self._send(200, b'{"api":"ok"}')
            return
        with LOCK:
            resource = RESOURCES.get(path)
        if resource is None and "/nodes/" in path:
            node_id = path.rstrip("/").split("/")[-1]
            with LOCK:
                for item in RESOURCES.values():
                    if isinstance(item, dict) and item.get("id") == node_id and item.get("type") == "node":
                        resource = item
                        break
        if resource is None and "/senders/" in path:
            sender_id = path.rstrip("/").split("/")[-1]
            with LOCK:
                for item in RESOURCES.values():
                    if isinstance(item, dict) and item.get("id") == sender_id:
                        resource = item
                        break
        if resource is None:
            self._send(404, b'{"error":"not found"}')
            return
        self._send(200, json.dumps(resource).encode())

    def do_PUT(self):
        self.do_POST()

    def do_POST(self):
        body = self._read()
        path = self.path.split("?", 1)[0]
        if path.endswith("/resource"):
            try:
                doc = json.loads(body.decode() or "{}")
            except json.JSONDecodeError:
                self._send(400, b'{"error":"bad json"}')
                return
            data = doc.get("data") if isinstance(doc.get("data"), dict) else doc
            resource_id = data.get("id", "")
            resource_type = doc.get("type") or data.get("type") or "resource"
            key = resource_type + ":" + resource_id
            with LOCK:
                existed = key in RESOURCES
                RESOURCES[key] = data
                RESOURCES["/x-nmos/query/v1.3/" + resource_type + "s/" + resource_id] = data
            self._send(200 if existed else 201, json.dumps(data).encode())
            return
        if "/health/" in path:
            self._send(200, b"{}")
            return
        self._send(200, body or b"{}")

    def do_DELETE(self):
        path = self.path.split("?", 1)[0].rstrip("/")
        resource_id = path.split("/")[-1] if path else ""
        with LOCK:
            doomed = [key for key, value in RESOURCES.items() if key == path or key.endswith("/" + resource_id) or (isinstance(value, dict) and value.get("id") == resource_id)]
            for key in doomed:
                del RESOURCES[key]
        self._send(204, b"")


def serve(port):
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    server.serve_forever()


def main():
    reg = int(sys.argv[1]) if len(sys.argv) > 1 else 3210
    query = int(sys.argv[2]) if len(sys.argv) > 2 else reg + 1
    threading.Thread(target=serve, args=(reg,), daemon=True).start()
    serve(query)


if __name__ == "__main__":
    main()
