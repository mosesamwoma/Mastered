import ipaddress
import time
from collections import OrderedDict, deque

MAX_TRACKED_CLIENTS = 20000
TRUSTED_NETWORKS = []


def configure_trusted(networks):
    TRUSTED_NETWORKS[:] = list(networks)


def peer_is_trusted(scope):
    if not TRUSTED_NETWORKS:
        return True
    client = scope.get("client")
    try:
        address = ipaddress.ip_address(client[0]) if client else None
    except ValueError:
        return False
    return address is not None and any(address in network for network in TRUSTED_NETWORKS)


def client_key(scope, header, hops=1):
    headers = {name.lower(): value for name, value in scope.get("headers", [])}
    candidate = None
    if header and peer_is_trusted(scope):
        raw = headers.get(header.encode("latin-1"))
        if raw:
            text = raw.decode("latin-1").strip()
            if header == "x-forwarded-for":
                parts = [part.strip() for part in text.split(",") if part.strip()]
                candidate = parts[-hops] if 0 < hops <= len(parts) else None
            else:
                candidate = text
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

    def retry_after(self, key, now=None):
        now = time.monotonic() if now is None else now
        stamps = self.entries.get(key)
        if stamps is None:
            return 0.0
        while stamps and now - stamps[0] >= self.window:
            stamps.popleft()
        if len(stamps) >= self.limit:
            return max(1.0, self.window - (now - stamps[0]))
        return 0.0

    def clear(self, key):
        self.entries.pop(key, None)

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
    def __init__(self, app, requests_per_minute, uploads_per_hour, header, hops=1):
        self.app = app
        self.header = header
        self.hops = hops
        self.requests = SlidingWindow(requests_per_minute, 60.0)
        self.uploads = SlidingWindow(uploads_per_hour, 3600.0)

    async def __call__(self, scope, receive, send):
        path = scope.get("path", "")
        if scope["type"] != "http" or not path.startswith("/api/") or path in ("/api/health", "/api/ready"):
            await self.app(scope, receive, send)
            return
        key = client_key(scope, self.header, self.hops)
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
