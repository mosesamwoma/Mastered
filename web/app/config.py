import os
import re
import shutil
import tempfile
from dataclasses import dataclass
from pathlib import Path

from .auth import parse_users

ROOT = Path(__file__).resolve().parent.parent


class ConfigError(RuntimeError):
    pass


def read_number(name, default, minimum, maximum, cast):
    raw = os.environ.get(name)
    if raw is None or raw.strip() == "":
        return default
    try:
        value = cast(raw)
    except ValueError:
        raise ConfigError(f"{name} must be a number, got {raw!r}") from None
    if not minimum <= value <= maximum:
        raise ConfigError(f"{name} must be between {minimum} and {maximum}")
    return value


def find_engine():
    explicit = os.environ.get("MASTERED_CLI")
    if explicit:
        candidates = [Path(explicit)]
    else:
        candidates = [Path("/usr/local/bin/mastered_cli"), ROOT.parent / "build" / "mastered_cli"]
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate.resolve()
    raise ConfigError("Mastering engine not found. Build it with ./build.sh or set MASTERED_CLI")


def find_tool(variable, name):
    explicit = os.environ.get(variable)
    if explicit:
        candidate = Path(explicit)
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate.resolve()
        raise ConfigError(f"{variable} does not point to an executable file")
    found = shutil.which(name)
    return Path(found).resolve() if found else None


@dataclass(frozen=True)
class Settings:
    engine_path: Path
    ffmpeg_path: Path
    ffprobe_path: Path
    work_dir: Path
    static_dir: Path
    max_file_bytes: int
    max_duration_sec: float
    max_pending_jobs: int
    max_concurrent_jobs: int
    pending_per_client: int
    jobs_per_hour_per_client: int
    api_requests_per_minute: int
    upload_attempts_per_hour: int
    client_ip_header: str
    job_ttl_sec: int
    job_timeout_sec: int
    convert_timeout_sec: int
    auth_users: dict
    session_secret: bytes
    session_seconds: int
    cookie_secure: str
    login_attempts: int
    trusted_proxy_hops: int
    max_decoded_mb: int
    min_free_mb: int
    engine_memory_mb: int
    admin_token: str
    admin_users: frozenset
    production: bool

    @property
    def auth_enabled(self):
        return bool(self.auth_users)

    @property
    def database_path(self):
        return self.work_dir / "jobs.db"


def load_auth():
    raw = os.environ.get("AUTH_USERS") or ""
    try:
        users = parse_users(raw)
    except ValueError as error:
        raise ConfigError(f"AUTH_USERS is not valid: {error}") from None
    secret = (os.environ.get("SESSION_SECRET") or "").strip()
    if users and len(secret) < 32:
        raise ConfigError("SESSION_SECRET must be set to a random value of at least 32 characters when AUTH_USERS is used")
    mode = (os.environ.get("COOKIE_SECURE") or "auto").strip().lower()
    if mode not in ("auto", "always", "never"):
        raise ConfigError("COOKIE_SECURE must be auto, always or never")
    return users, secret.encode("utf-8"), mode


def read_flag(name, default):
    raw = (os.environ.get(name) or "").strip().lower()
    if raw == "":
        return default
    if raw in ("1", "true", "yes", "on"):
        return True
    if raw in ("0", "false", "no", "off"):
        return False
    raise ConfigError(f"{name} must be true or false")


def load_admin(users):
    token = (os.environ.get("ADMIN_TOKEN") or "").strip()
    if token and len(token) < 24:
        raise ConfigError("ADMIN_TOKEN must be at least 24 characters long")
    names = frozenset(name.strip() for name in re.split(r"[,\s]+", os.environ.get("ADMIN_USERS") or "") if name.strip())
    unknown = names - set(users)
    if unknown:
        raise ConfigError(f"ADMIN_USERS lists users that are not in AUTH_USERS: {', '.join(sorted(unknown))}")
    return token, names


def load_settings():
    users, secret, cookie_mode = load_auth()
    admin_token, admin_users = load_admin(users)
    production = (os.environ.get("APP_ENV") or "").strip().lower() == "production"
    if production:
        if not users and not read_flag("ALLOW_PUBLIC", False):
            raise ConfigError("Production mode requires sign-in: set AUTH_USERS and SESSION_SECRET, or set ALLOW_PUBLIC=true to run an open service on purpose")
        if cookie_mode == "never":
            raise ConfigError("COOKIE_SECURE=never is not allowed in production mode")
    work_dir = Path(os.environ.get("WORK_DIR") or Path(tempfile.gettempdir()) / "mastered_jobs")
    max_file_mb = read_number("MAX_FILE_MB", 60.0, 0.1, 2048.0, float)
    return Settings(
        engine_path=find_engine(),
        ffmpeg_path=find_tool("FFMPEG_PATH", "ffmpeg"),
        ffprobe_path=find_tool("FFPROBE_PATH", "ffprobe"),
        work_dir=work_dir,
        static_dir=ROOT / "static",
        max_file_bytes=int(max_file_mb * 1024 * 1024),
        max_duration_sec=read_number("MAX_DURATION_SEC", 420.0, 5.0, 7200.0, float),
        max_pending_jobs=read_number("MAX_PENDING_JOBS", 2, 1, 50, int),
        max_concurrent_jobs=read_number("MAX_CONCURRENT_JOBS", 1, 1, 8, int),
        pending_per_client=read_number("PENDING_PER_CLIENT", 1, 1, 20, int),
        jobs_per_hour_per_client=read_number("JOBS_PER_HOUR_PER_CLIENT", 6, 1, 1000, int),
        api_requests_per_minute=read_number("API_REQUESTS_PER_MINUTE", 240, 10, 100000, int),
        upload_attempts_per_hour=read_number("UPLOAD_ATTEMPTS_PER_HOUR", 30, 1, 10000, int),
        client_ip_header=(os.environ.get("CLIENT_IP_HEADER") or "").strip().lower(),
        job_ttl_sec=read_number("JOB_TTL_MINUTES", 20, 1, 1440, int) * 60,
        job_timeout_sec=read_number("JOB_TIMEOUT_SEC", 1800, 30, 86400, int),
        convert_timeout_sec=read_number("CONVERT_TIMEOUT_SEC", 300, 10, 3600, int),
        auth_users=users,
        session_secret=secret,
        session_seconds=int(read_number("SESSION_HOURS", 12.0, 0.1, 720.0, float) * 3600),
        cookie_secure=cookie_mode,
        login_attempts=read_number("LOGIN_ATTEMPTS", 8, 1, 100, int),
        trusted_proxy_hops=read_number("TRUSTED_PROXY_HOPS", 1, 1, 10, int),
        max_decoded_mb=read_number("MAX_DECODED_MB", 1024, 16, 16384, int),
        min_free_mb=read_number("MIN_FREE_MB", 512, 0, 1048576, int),
        engine_memory_mb=read_number("ENGINE_MEMORY_MB", 2048, 256, 65536, int),
        admin_token=admin_token,
        admin_users=admin_users,
        production=production,
    )
