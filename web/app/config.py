import os
import tempfile
from dataclasses import dataclass
from pathlib import Path

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


@dataclass(frozen=True)
class Settings:
    engine_path: Path
    work_dir: Path
    static_dir: Path
    max_file_bytes: int
    max_duration_sec: float
    max_pending_jobs: int
    job_ttl_sec: int
    job_timeout_sec: int


def load_settings():
    work_dir = Path(os.environ.get("WORK_DIR") or Path(tempfile.gettempdir()) / "mastered_jobs")
    max_file_mb = read_number("MAX_FILE_MB", 60.0, 0.1, 2048.0, float)
    return Settings(
        engine_path=find_engine(),
        work_dir=work_dir,
        static_dir=ROOT / "static",
        max_file_bytes=int(max_file_mb * 1024 * 1024),
        max_duration_sec=read_number("MAX_DURATION_SEC", 420.0, 5.0, 7200.0, float),
        max_pending_jobs=read_number("MAX_PENDING_JOBS", 2, 1, 50, int),
        job_ttl_sec=read_number("JOB_TTL_MINUTES", 20, 1, 1440, int) * 60,
        job_timeout_sec=read_number("JOB_TIMEOUT_SEC", 1800, 30, 86400, int),
    )
