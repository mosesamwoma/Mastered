import json
import logging
import re
import sys
import threading
import time
import uuid

from .ratelimit import client_key

JOB_TOKEN = re.compile(r"[0-9a-f]{32}")
LOG = logging.getLogger("mastered")


class JsonFormatter(logging.Formatter):
    def format(self, record):
        entry = {
            "time": time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(record.created)) + f".{int(record.msecs):03d}Z",
            "level": record.levelname.lower(),
            "logger": record.name,
            "message": record.getMessage(),
        }
        fields = getattr(record, "fields", None)
        if isinstance(fields, dict):
            entry.update(fields)
        if record.exc_info:
            entry["exception"] = self.formatException(record.exc_info)
        return json.dumps(entry, ensure_ascii=True, separators=(",", ":"))


def setup_logging():
    handler = logging.StreamHandler(sys.stdout)
    handler.setFormatter(JsonFormatter())
    root = logging.getLogger()
    root.handlers[:] = [handler]
    root.setLevel(logging.INFO)
    for name in ("uvicorn", "uvicorn.error", "uvicorn.access"):
        logger = logging.getLogger(name)
        logger.handlers[:] = []
        logger.propagate = True


def log_event(message, **fields):
    LOG.info(message, extra={"fields": fields})


def mask_path(path):
    return JOB_TOKEN.sub(lambda match: match.group(0)[:8] + "...", path)


class Metrics:
    def __init__(self):
        self.lock = threading.Lock()
        self.counters = {}
        self.sums = {}
        self.gauges = {}

    def count(self, name, labels=None, amount=1):
        key = (name, tuple(sorted((labels or {}).items())))
        with self.lock:
            self.counters[key] = self.counters.get(key, 0) + amount

    def observe(self, name, value, labels=None):
        key = (name, tuple(sorted((labels or {}).items())))
        with self.lock:
            total, count = self.sums.get(key, (0.0, 0))
            self.sums[key] = (total + float(value), count + 1)

    def gauge(self, name, value):
        with self.lock:
            self.gauges[name] = float(value)

    @staticmethod
    def label_text(labels):
        if not labels:
            return ""
        parts = []
        for name, value in labels:
            text = str(value).replace("\\", "\\\\").replace('"', '\\"').replace("\n", " ")
            parts.append(f'{name}="{text}"')
        return "{" + ",".join(parts) + "}"

    def render(self):
        with self.lock:
            counters = dict(self.counters)
            sums = dict(self.sums)
            gauges = dict(self.gauges)
        lines = []
        for (name, labels), value in sorted(counters.items()):
            lines.append(f"mastered_{name}_total{self.label_text(labels)} {value}")
        for (name, labels), (total, count) in sorted(sums.items()):
            lines.append(f"mastered_{name}_seconds_sum{self.label_text(labels)} {total:.3f}")
            lines.append(f"mastered_{name}_seconds_count{self.label_text(labels)} {count}")
        for name, value in sorted(gauges.items()):
            lines.append(f"mastered_{name} {value:.3f}")
        return "\n".join(lines) + "\n"


metrics = Metrics()


class RequestLogMiddleware:
    def __init__(self, app, header, hops):
        self.app = app
        self.header = header
        self.hops = hops

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http":
            await self.app(scope, receive, send)
            return
        request_id = uuid.uuid4().hex[:16]
        scope["mastered_request_id"] = request_id
        started = time.monotonic()
        status = 500

        async def send_with_id(message):
            nonlocal status
            if message["type"] == "http.response.start":
                status = message["status"]
                headers = list(message.get("headers", []))
                headers.append((b"x-request-id", request_id.encode("ascii")))
                message = {**message, "headers": headers}
            await send(message)

        try:
            await self.app(scope, receive, send_with_id)
        finally:
            path = scope.get("path", "")
            if path != "/api/health" and path != "/api/ready":
                elapsed = time.monotonic() - started
                if path.startswith("/api/"):
                    metrics.count("http_requests", {"class": f"{status // 100}xx"})
                log_event(
                    "request",
                    request_id=request_id,
                    method=scope.get("method", ""),
                    path=mask_path(path),
                    status=status,
                    duration_ms=round(elapsed * 1000.0, 1),
                    client=client_key(scope, self.header, self.hops),
                    user=scope.get("mastered_user"),
                )
