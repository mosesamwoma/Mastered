import ipaddress
import time
from collections import OrderedDict, deque

MAX_TRACKED_CLIENTS = 20000


def client_key(scope, header):
    headers = {name.lower(): value for name, value in scope.get("headers", [])}
    candidate = None
    if header:
        raw = headers.get(header.encode("latin-1"))
        if raw:
            text = raw.decode("latin-1").strip()
            candidate = text.split(",")[0].strip() if header == "x-forwarded-for" else text
    if not candidate:
        client = scope.get("client")
        candidate = client[0] if client else "unknown"
    try:
        address = ipaddress.ip_address(candidate)
    except ValueError:
        return "unknown"
    if address.version == 6:
        return str(ipaddress.ip_network(f"{address}/64", strict=False).network_address)
    return str(address)


class SlidingWindow:
    def __init__(self, limit, window_seconds, max_keys=MAX_TRACKED_CLIENTS):
        self.limit = limit
        self.window = window_seconds
        self.max_keys = max_keys
        self.entries = OrderedDict()

    def hit(self, key, now=None):
        now = time.monotonic() if now is None else now
        stamps = self.entries.get(key)
        if stamps is None:
            stamps = deque()
            self.entries[key] = stamps
        else:
            self.entries.move_to_end(key)
        while stamps and now - stamps[0] >= self.window:
            stamps.popleft()
        if len(stamps) >= self.limit:
            return False, max(1.0, self.window - (now - stamps[0]))
        stamps.append(now)
        while len(self.entries) > self.max_keys:
            self.entries.popitem(last=False)
        return True, 0.0


class RateLimitMiddleware:
    def __init__(self, app, requests_per_minute, uploads_per_hour, header):
        self.app = app
        self.header = header
        self.requests = SlidingWindow(requests_per_minute, 60.0)
        self.uploads = SlidingWindow(uploads_per_hour, 3600.0)

    async def __call__(self, scope, receive, send):
        path = scope.get("path", "")
        if scope["type"] != "http" or not path.startswith("/api/") or path == "/api/health":
            await self.app(scope, receive, send)
            return
        key = client_key(scope, self.header)
        allowed, retry = self.requests.hit(key)
        if allowed and scope["method"] == "POST" and path == "/api/jobs":
            allowed, retry = self.uploads.hit(key)
        if allowed:
            await self.app(scope, receive, send)
            return
        seconds = int(retry) + 1
        body = ('{"detail":"Too many requests. Please try again in %d seconds."}' % seconds).encode()
        await send(
            {
                "type": "http.response.start",
                "status": 429,
                "headers": [
                    (b"content-type", b"application/json"),
                    (b"content-length", str(len(body)).encode()),
                    (b"retry-after", str(seconds).encode()),
                ],
            }
        )
        await send({"type": "http.response.body", "body": body})
