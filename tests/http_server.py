#!/usr/bin/env python3
"""Tiny HTTP/HTTPS server for the JSONfs web-mount tests.

usage: http_server.py DIR PORTFILE [CERT KEY] [--no-validators] [--no-range]

Serves DIR on 127.0.0.1 (random port, written to PORTFILE) and logs every
request as "METHOD PATH STATUS BODY_BYTES" to PORTFILE.log.

  * ETag (size + mtime) and Last-Modified with If-None-Match /
    If-Modified-Since -> 304 (disabled by --no-validators)
  * Range: bytes=A-B -> 206 (ignored with --no-range)
  * /redirect/<name>               302 -> /<name>
  * /redirect-http/<port>/<name>   302 -> http://127.0.0.1:<port>/<name>
"""
import email.utils
import http.server
import os
import ssl
import sys

OPTS = {"validators": True, "range": True, "log": None}


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def _log(self, status, nbytes):
        with open(OPTS["log"], "a") as f:
            f.write("%s %s %d %d\n" % (self.command, self.path, status, nbytes))

    def _send(self, status, body=b"", headers=None):
        self.send_response(status)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)
        self._log(status, len(body))

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        parts = self.path.split("/")
        if self.path.startswith("/redirect-http/") and len(parts) >= 4:
            return self._send(302, headers={"Location": "http://127.0.0.1:%s/%s" % (parts[2], "/".join(parts[3:]))})
        if self.path.startswith("/redirect/"):
            return self._send(302, headers={"Location": "/" + self.path[len("/redirect/"):]})
        name = self.path.lstrip("/").split("?")[0]
        if not name or ".." in name or not os.path.isfile(name):
            return self._send(404, b"not found")
        st = os.stat(name)
        with open(name, "rb") as f:
            data = f.read()
        headers = {"Content-Type": "application/json"}
        if OPTS["validators"]:
            etag = '"%x-%x"' % (st.st_size, st.st_mtime_ns)
            headers["ETag"] = etag
            headers["Last-Modified"] = email.utils.formatdate(st.st_mtime, usegmt=True)
            inm = self.headers.get("If-None-Match")
            if inm is not None and etag in [t.strip() for t in inm.split(",")]:
                return self._send(304, headers={"ETag": etag})
        rng = self.headers.get("Range")
        if OPTS["range"] and rng and rng.startswith("bytes="):
            a, _, b = rng[6:].partition("-")
            a = int(a or 0)
            b = min(int(b) if b else len(data) - 1, len(data) - 1)
            headers["Content-Range"] = "bytes %d-%d/%d" % (a, b, len(data))
            return self._send(206, data[a : b + 1], headers)
        return self._send(200, data, headers)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = [a for a in sys.argv[1:] if a.startswith("--")]
    OPTS["validators"] = "--no-validators" not in flags
    OPTS["range"] = "--no-range" not in flags
    root, portfile = args[0], os.path.abspath(args[1])
    tls = [os.path.abspath(x) for x in args[2:4]]
    OPTS["log"] = portfile + ".log"
    open(OPTS["log"], "w").close()
    os.chdir(root)
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    if len(tls) == 2:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(tls[0], tls[1])
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    with open(portfile + ".tmp", "w") as f:
        f.write(str(srv.server_address[1]))
    os.rename(portfile + ".tmp", portfile)
    srv.serve_forever()


if __name__ == "__main__":
    main()
