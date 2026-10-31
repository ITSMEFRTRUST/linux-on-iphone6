# tiny HTTP server for net_cookie_test: ?set=<cookie> (repeatable) sets cookies, the body echoes the Cookie header,
# ?big=1 streams ~26 MB, ?sleep=s waits first. Binds 127.0.0.1 only.
import http.server, sys, urllib.parse, time
class H(http.server.BaseHTTPRequestHandler):
    def reply(self):
        q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        if 'sleep' in q: time.sleep(float(q['sleep'][0]))
        self.send_response(200)
        self.send_header('Content-Type', 'text/plain')
        for c in q.get('set', []):
            self.send_header('Set-Cookie', c if 'Max-Age' in c else c + '; Path=/; Max-Age=3600')
        self.end_headers()
        body = ('cookie=[%s]\n' % self.headers.get('Cookie', '')).encode()
        self.wfile.write(body)
        if 'big' in q:
            try:
                for i in range(400): self.wfile.write(b'x' * 65536)
            except Exception: pass
    do_GET = reply
    def do_POST(self):
        self.rfile.read(int(self.headers.get('Content-Length', 0)))
        self.reply()
    def log_message(self, *a): pass
http.server.ThreadingHTTPServer(('127.0.0.1', int(sys.argv[1])), H).serve_forever()
