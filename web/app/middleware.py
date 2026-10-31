from urllib.parse import urlsplit

from .auth import session_token

SECURITY_HEADERS = [
    (b"x-content-type-options", b"nosniff"),
    (b"x-frame-options", b"DENY"),
    (b"referrer-policy", b"no-referrer"),
    (
        b"content-security-policy",
        b"default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
        b"connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'",
    ),
]


class SecurityHeadersMiddleware:
    def __init__(self, app):
        self.app = app

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http":
            await self.app(scope, receive, send)
            return

        async def send_with_headers(message):
            if message["type"] == "http.response.start":
                existing = {name.lower() for name, _ in message.get("headers", [])}
                headers = list(message.get("headers", []))
                for name, value in SECURITY_HEADERS:
                    if name not in existing:
                        headers.append((name, value))
                message = {**message, "headers": headers}
            await send(message)

        await self.app(scope, receive, send_with_headers)


SAFE_METHODS = ("GET", "HEAD", "OPTIONS")
PUBLIC_PATHS = {
    "/login", "/login.js", "/style.css", "/api/health", "/api/login", "/api/logout", "/api/session",
}


class BodyTooLarge(Exception):
    pass


class BodySizeLimitMiddleware:
    def __init__(self, app, max_bytes, path, method="POST", message=None):
        self.app = app
        self.max_bytes = max_bytes
        self.path = path
        self.method = method
        self.message = message

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http" or scope["path"] != self.path or scope["method"] != self.method:
            await self.app(scope, receive, send)
            return

        headers = {name.lower(): value for name, value in scope["headers"]}
        declared = headers.get(b"content-length")
        if declared is not None:
            try:
                too_large = int(declared) > self.max_bytes
            except ValueError:
                await self._reject(send, 400, b'{"detail":"Invalid Content-Length header"}')
                return
            if too_large:
                await self._reject(send, 413, self._message())
                return

        received = 0
        exceeded = False
        responded = False

        async def limited_receive():
            nonlocal received, exceeded
            message = await receive()
            if message["type"] == "http.request":
                received += len(message.get("body", b""))
                if received > self.max_bytes:
                    exceeded = True
                    raise BodyTooLarge()
            return message

        async def guarded_send(message):
            nonlocal responded
            if exceeded:
                if not responded:
                    responded = True
                    await self._reject(send, 413, self._message())
                return
            if message["type"] == "http.response.start":
                responded = True
            await send(message)

        try:
            await self.app(scope, limited_receive, guarded_send)
        except BodyTooLarge:
            pass
        if exceeded and not responded:
            await self._reject(send, 413, self._message())

    def _message(self):
        if self.message:
            return ('{"detail":"%s"}' % self.message).encode()
        limit_mb = self.max_bytes / (1024 * 1024)
        return ('{"detail":"Upload is too large. The combined limit is %g MB."}' % round(limit_mb, 1)).encode()

    async def _reject(self, send, status, body):
        await send(
            {
                "type": "http.response.start",
                "status": status,
                "headers": [
                    (b"content-type", b"application/json"),
                    (b"content-length", str(len(body)).encode()),
                    (b"connection", b"close"),
                ],
            }
        )
        await send({"type": "http.response.body", "body": body})


class AuthMiddleware:
    def __init__(self, app, signer, users):
        self.app = app
        self.signer = signer
        self.users = users

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http":
            await self.app(scope, receive, send)
            return
        headers = scope.get("headers", [])
        scope["mastered_user"] = self.signer.read(session_token(headers), self.users)
        path = scope["path"]
        if scope["method"] not in SAFE_METHODS and not self.same_origin(headers):
            await self.reply(send, 403, b'{"detail":"Cross-site request blocked."}')
            return
        if path in PUBLIC_PATHS or scope["mastered_user"] is not None:
            await self.app(scope, receive, send)
            return
        if path.startswith("/api/") or scope["method"] not in SAFE_METHODS:
            await self.reply(send, 401, b'{"detail":"Please sign in."}')
            return
        await self.reply(send, 302, b"", [(b"location", b"/login")])

    def same_origin(self, headers):
        values = {name: value for name, value in headers}
        origin = values.get(b"origin")
        if origin is None:
            return True
        host = values.get(b"host", b"").decode("latin-1").lower()
        return urlsplit(origin.decode("latin-1")).netloc.lower() == host and host != ""

    async def reply(self, send, status, body, extra=None):
        await send(
            {
                "type": "http.response.start",
                "status": status,
                "headers": [
                    (b"content-type", b"application/json"),
                    (b"content-length", str(len(body)).encode()),
                    (b"cache-control", b"no-store"),
                    *(extra or []),
                ],
            }
        )
        await send({"type": "http.response.body", "body": body})
