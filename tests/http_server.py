#!/usr/bin/env python3
"""Tiny HTTP/HTTPS server for the JSONfs web-mount tests.

usage: http_server.py DIR PORTFILE [CERT KEY]

Serves DIR on 127.0.0.1 (random port, written to PORTFILE). Extra routes:
  /redirect/<name>         302 -> /<name> on the same server
  /redirect-http/<port>/<name>  302 -> http://127.0.0.1:<port>/<name>
"""
import http.server
import os
import ssl
import sys


class Handler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        parts = self.path.split("/")
        if self.path.startswith("/redirect-http/") and len(parts) >= 4:
            self.send_response(302)
            self.send_header("Location", "http://127.0.0.1:%s/%s" % (parts[2], "/".join(parts[3:])))
            self.end_headers()
            return
        if self.path.startswith("/redirect/"):
            self.send_response(302)
            self.send_header("Location", "/" + self.path[len("/redirect/"):])
            self.end_headers()
            return
        super().do_GET()


def main():
    root, portfile = sys.argv[1], os.path.abspath(sys.argv[2])
    tls = [os.path.abspath(x) for x in sys.argv[3:5]]
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
