import asyncio
import json
import os
import re
import secrets
from contextlib import asynccontextmanager
from typing import Literal

from fastapi import FastAPI, File, Form, HTTPException, Request, Response, UploadFile
from fastapi.exceptions import RequestValidationError
from fastapi.responses import FileResponse, JSONResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field
from starlette.concurrency import run_in_threadpool

from .auth import COOKIE_NAME, MAX_PASSWORD_BYTES, SessionSigner, hash_password, verify_password
from .config import load_settings
from .exports import EXPORT_KEY, MAX_EXPORTS_PER_JOB, build_export, describe_formats, find_quality
from .jobs import JobManager, QueueFull, RateLimited, describe_duration, download_name, track_stem
from .media import SOURCE_EXTENSIONS, MediaError, MediaTools
from .reports import report_text
from .middleware import AuthMiddleware, BodySizeLimitMiddleware, SecurityHeadersMiddleware
from .ratelimit import RateLimitMiddleware, SlidingWindow, client_key
from .wav_info import WavError, inspect_wav

VERSION = "2.3.0"
JOB_ID = re.compile(r"^[0-9a-f]{32}$")
CHUNK_BYTES = 1024 * 1024
BODY_OVERHEAD_BYTES = 1024 * 1024
BIT_DEPTHS = (0, 16, 24, 32)
LOGIN_WINDOW_SEC = 900.0
LOGIN_BODY_BYTES = 4096

settings = load_settings()
media = MediaTools(settings.ffmpeg_path, settings.ffprobe_path)
manager = JobManager(settings, media)
signer = SessionSigner(settings.session_secret, settings.session_seconds) if settings.auth_enabled else None
login_limiter = SlidingWindow(settings.login_attempts, LOGIN_WINDOW_SEC)
presets = []
placeholder_hash = hash_password(secrets.token_urlsafe(16)) if settings.auth_enabled else ""


class LoginRequest(BaseModel):
    username: str = Field(min_length=1, max_length=64)
    password: str = Field(min_length=1, max_length=MAX_PASSWORD_BYTES)


async def load_presets():
    """Reads the platform presets from the engine so that the web page and the engine share one list."""
    try:
        process = await asyncio.create_subprocess_exec(
            str(settings.engine_path), "--list-presets",
            stdin=asyncio.subprocess.DEVNULL, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL,
        )
        stdout, _ = await asyncio.wait_for(process.communicate(), timeout=10)
        data = json.loads(stdout.decode("utf-8", "replace"))
    except (OSError, ValueError, asyncio.TimeoutError):
        return []
    return data if isinstance(data, list) else []


@asynccontextmanager
async def lifespan(_app):
    presets[:] = await load_presets()
    await media.detect_preview_codec()
    await manager.start()
    try:
        yield
    finally:
        await manager.stop()


app = FastAPI(title="Mastered", version=VERSION, lifespan=lifespan, docs_url=None, redoc_url=None, openapi_url=None)
app.add_middleware(
    BodySizeLimitMiddleware,
    max_bytes=settings.max_file_bytes * 2 + BODY_OVERHEAD_BYTES,
    path="/api/jobs",
)
app.add_middleware(
    BodySizeLimitMiddleware,
    max_bytes=LOGIN_BODY_BYTES,
    path="/api/login",
    message="The request is too large.",
)
if signer is not None:
    app.add_middleware(AuthMiddleware, signer=signer, users=settings.auth_users)
app.add_middleware(
    RateLimitMiddleware,
    requests_per_minute=settings.api_requests_per_minute,
    uploads_per_hour=settings.upload_attempts_per_hour,
    header=settings.client_ip_header,
)
app.add_middleware(SecurityHeadersMiddleware)


@app.exception_handler(RequestValidationError)
async def validation_failed(_request, error):
    details = [{"loc": list(item["loc"]), "msg": item["msg"], "type": item["type"]} for item in error.errors()]
    return JSONResponse({"detail": details}, status_code=422)


def copy_limited(source, destination, limit):
    written = 0
    with destination.open("wb") as handle:
        while True:
            chunk = source.read(CHUNK_BYTES)
            if not chunk:
                break
            written += len(chunk)
            if written > limit:
                raise OverflowError
            handle.write(chunk)
    return written


def check_duration(seconds, label):
    if seconds < 1.0:
        raise HTTPException(400, f"The {label} file must be at least one second long")
    if seconds > settings.max_duration_sec:
        raise HTTPException(413, f"The {label} file is longer than {describe_duration(settings.max_duration_sec)}")


async def classify_upload(raw, wav, label):
    try:
        info = await run_in_threadpool(inspect_wav, raw)
    except WavError as error:
        wav_problem = str(error)
    else:
        check_duration(info.duration, label)
        await run_in_threadpool(os.replace, raw, wav)
        return {"convert": False}
    if not media.available:
        raise HTTPException(
            400,
            f"The {label} file is not a usable WAV file ({wav_problem}). Only WAV files are supported on this server.",
        )
    try:
        probe = await media.probe(raw)
    except MediaError as error:
        raise HTTPException(400, f"The {label} file is not a supported audio file: {error}.") from None
    check_duration(probe.duration, label)
    return {
        "convert": True,
        "codec": probe.codec,
        "channels": probe.channels,
        "sample_rate": probe.sample_rate,
        "bits": probe.bits,
        "duration": probe.duration,
    }


async def store_upload(upload, directory, label):
    raw = directory / f"{label}.src"
    try:
        size = await run_in_threadpool(copy_limited, upload.file, raw, settings.max_file_bytes)
    except OverflowError:
        limit_mb = settings.max_file_bytes / (1024 * 1024)
        raise HTTPException(413, f"The {label} file is larger than {limit_mb:g} MB") from None
    if size == 0:
        raise HTTPException(400, f"The {label} file is empty")
    return await classify_upload(raw, directory / f"{label}.wav", label)


def build_engine_arguments(
    target_lufs,
    aggressiveness,
    ceiling_db,
    bands,
    bit_depth,
    precision,
    dynamics,
    soft_clip,
    dither,
    noise_shaping,
    mid_side,
    match_width,
    mono_bass_hz=0.0,
    auto_clip=True,
    auto_mono_bass=True,
    dual_mono=False,
    bass_limiter=False,
):
    arguments = [
        "--target", f"{target_lufs:.3f}",
        "--aggressive", f"{aggressiveness:.4f}",
        "--ceiling", f"{ceiling_db:.3f}",
        "--bands", str(bands),
        "--presets",
    ]
    if bit_depth:
        arguments += ["--bitdepth", str(bit_depth)]
    if precision == "fast":
        arguments += ["--passes", "6", "--tolerance", "0.1", "--fast-peak"]
    elif precision == "high":
        arguments += ["--passes", "20", "--tolerance", "0.01"]
    if dynamics:
        arguments.append("--dynamics")
    if soft_clip:
        arguments.append("--soft-clip")
    if not auto_clip:
        arguments.append("--no-auto-clip")
    if not auto_mono_bass:
        arguments.append("--no-auto-mono-bass")
    if dual_mono:
        arguments.append("--dual-mono")
    if bass_limiter:
        arguments.append("--bass-limiter")
    if not dither:
        arguments.append("--no-dither")
    if noise_shaping:
        arguments.append("--noise-shaping")
    if not mid_side:
        arguments.append("--no-mid-side")
    if not match_width:
        arguments.append("--no-width")
    if mono_bass_hz > 0:
        arguments += ["--mono-bass", f"{mono_bass_hz:.1f}"]
    return arguments


def identity(request):
    user = request.scope.get("mastered_user")
    return f"user:{user}" if user else client_key(request.scope, settings.client_ip_header)


def secure_cookie(request):
    if settings.cookie_secure != "auto":
        return settings.cookie_secure == "always"
    forwarded = request.headers.get("x-forwarded-proto", "").split(",")[0].strip().lower()
    return request.url.scheme == "https" or forwarded == "https"


def cookie_options(request):
    return {"httponly": True, "samesite": "lax", "secure": secure_cookie(request), "path": "/"}


async def require_job(job_id, request):
    row = await manager.row(job_id) if JOB_ID.match(job_id) else None
    if row is None or row["cancel"] or row["status"] == "uploading":
        raise HTTPException(404, "Job not found or expired")
    if signer is not None and row["client"] != identity(request):
        raise HTTPException(404, "Job not found or expired")
    return row


@app.post("/api/login")
async def login(request: Request, body: LoginRequest):
    if signer is None:
        raise HTTPException(404, "Sign-in is not enabled on this server.")
    key = client_key(request.scope, settings.client_ip_header)
    wait = login_limiter.retry_after(key)
    if wait > 0:
        raise HTTPException(
            429,
            f"Too many failed attempts. Please try again in {describe_duration(wait)}.",
            headers={"Retry-After": str(int(wait) + 1)},
        )
    stored = settings.auth_users.get(body.username)
    valid = await asyncio.to_thread(verify_password, body.password, stored or placeholder_hash)
    if stored is None or not valid:
        login_limiter.hit(key)
        raise HTTPException(401, "Incorrect username or password.")
    login_limiter.clear(key)
    response = JSONResponse({"user": body.username}, headers={"Cache-Control": "no-store"})
    response.set_cookie(
        COOKIE_NAME, signer.issue(body.username, stored), max_age=settings.session_seconds, **cookie_options(request)
    )
    return response


@app.post("/api/logout", status_code=204)
async def logout(request: Request):
    response = Response(status_code=204)
    response.set_cookie(COOKIE_NAME, "", max_age=0, **cookie_options(request))
    return response


@app.get("/api/session")
async def session(request: Request):
    return JSONResponse(
        {"authRequired": signer is not None, "user": request.scope.get("mastered_user")},
        headers={"Cache-Control": "no-store"},
    )


@app.get("/login", include_in_schema=False)
async def login_page(request: Request):
    if signer is None or request.scope.get("mastered_user"):
        return RedirectResponse("/", status_code=302)
    return FileResponse(settings.static_dir / "login.html", media_type="text/html", headers={"Cache-Control": "no-store"})


@app.get("/api/health")
async def health():
    return {"status": "ok"}


@app.get("/api/config")
async def public_config():
    return {
        "version": VERSION,
        "maxFileMb": round(settings.max_file_bytes / (1024 * 1024), 1),
        "maxDurationSec": settings.max_duration_sec,
        "maxPendingJobs": settings.max_pending_jobs,
        "resultMinutes": settings.job_ttl_sec // 60,
        "jobsPerHour": settings.jobs_per_hour_per_client,
        "formats": list(SOURCE_EXTENSIONS) if media.available else [".wav"],
        "exports": describe_formats(media.encoders),
        "preview": media.preview_codec is not None,
        "authRequired": signer is not None,
        "presets": presets,
    }


@app.post("/api/jobs", status_code=202)
async def create_job(
    request: Request,
    reference: UploadFile = File(...),
    track: UploadFile = File(...),
    target_lufs: float = Form(-14.0, ge=-60.0, le=-1.0),
    aggressiveness: float = Form(0.85, ge=0.0, le=1.0),
    ceiling_db: float = Form(-1.0, ge=-12.0, le=0.0),
    bands: int = Form(8, ge=0, le=32),
    bit_depth: int = Form(0),
    precision: Literal["fast", "normal", "high"] = Form("normal"),
    dynamics: bool = Form(False),
    soft_clip: bool = Form(False),
    dither: bool = Form(True),
    noise_shaping: bool = Form(False),
    mid_side: bool = Form(True),
    match_width: bool = Form(True),
    mono_bass_hz: float = Form(0.0, ge=0.0, le=500.0),
    auto_clip: bool = Form(True),
    auto_mono_bass: bool = Form(True),
    dual_mono: bool = Form(False),
    bass_limiter: bool = Form(False),
):
    if bit_depth not in BIT_DEPTHS:
        raise HTTPException(422, "bit_depth must be 0, 16, 24 or 32")
    if 0.0 < mono_bass_hz < 20.0:
        raise HTTPException(422, "mono_bass_hz must be 0 (off) or between 20 and 500")
    arguments = build_engine_arguments(
        target_lufs, aggressiveness, ceiling_db, bands, bit_depth, precision,
        dynamics, soft_clip, dither, noise_shaping, mid_side, match_width, mono_bass_hz,
        auto_clip, auto_mono_bass, dual_mono, bass_limiter,
    )
    client = identity(request)
    try:
        job_id = await manager.reserve(track.filename or "track.wav", arguments, client)
    except QueueFull:
        raise HTTPException(
            429, "The server is busy. Please try again in a few minutes.", headers={"Retry-After": "60"}
        ) from None
    except RateLimited as error:
        raise HTTPException(429, error.message, headers={"Retry-After": str(error.retry_after)}) from None
    directory = manager.job_directory(job_id)
    try:
        sources = {
            "reference": await store_upload(reference, directory, "reference"),
            "track": await store_upload(track, directory, "track"),
        }
        await manager.set_sources(job_id, sources)
    except BaseException:
        await manager.discard(job_id)
        raise
    await manager.enqueue(job_id)
    view = await manager.view(await manager.row(job_id))
    return JSONResponse({"id": job_id, "status": view["status"], "position": view.get("position", 0)}, status_code=202)


@app.get("/api/jobs/{job_id}")
async def job_status(job_id: str, request: Request):
    return await manager.view(await require_job(job_id, request))


@app.get("/api/jobs/{job_id}/download")
async def job_download(job_id: str, request: Request):
    row = await require_job(job_id, request)
    path = manager.job_directory(job_id) / "mastered.wav"
    if row["status"] != "done" or not path.is_file():
        raise HTTPException(409, "The mastered file is not ready")
    return FileResponse(path, media_type="audio/wav", filename=(await manager.view(row))["downloadName"])


@app.get("/api/jobs/{job_id}/audio/{version}")
async def job_audio(job_id: str, version: Literal["original", "mastered"], request: Request):
    row = await require_job(job_id, request)
    path = manager.job_directory(job_id) / f"{version}.{media.preview_extension}"
    if row["status"] != "done" or not row["preview"] or not path.is_file():
        raise HTTPException(404, "No preview is available for this job")
    return FileResponse(path, media_type=media.preview_media_type)


class ExportRequest(BaseModel):
    format: str = Field(min_length=2, max_length=12, pattern=r"^[a-z0-9]+$")
    quality: str = Field(min_length=2, max_length=16, pattern=r"^[a-z0-9]+$")


def count_exports(directory):
    return sum(1 for entry in directory.glob("export_*.json"))


@app.post("/api/jobs/{job_id}/export")
async def job_export(job_id: str, body: ExportRequest, request: Request):
    row = await require_job(job_id, request)
    directory = manager.job_directory(job_id)
    if row["status"] != "done" or not (directory / "mastered.wav").is_file() or not row["report"]:
        raise HTTPException(409, "The mastered file is not ready")
    if find_quality(media.encoders, body.format, body.quality)[0] is None:
        raise HTTPException(400, "This format or quality is not available on this server")
    async with manager.export_gate:
        # The job may have been removed or expired while this request waited for its turn.
        row = await manager.row(job_id)
        if row is None or row["cancel"] or not (directory / "mastered.wav").is_file():
            raise HTTPException(404, "Job not found or expired")
        if body.format != "wav" and not (directory / f"export_{body.format}_{body.quality}.json").is_file():
            if await run_in_threadpool(count_exports, directory) >= MAX_EXPORTS_PER_JOB:
                raise HTTPException(429, "This result already has the maximum number of exported files. Download one of them or master again.")
        try:
            info = await build_export(settings, directory, body.format, body.quality, media.encoders, row["report"], track_stem(row["track_name"]))
        except MediaError as error:
            raise HTTPException(500, f"Export failed: {error}.") from None
        except OSError:
            raise HTTPException(500, "Export failed: the file could not be written.") from None
    name = download_name(row["track_name"], info["extension"])
    url = f"/api/jobs/{job_id}/download" if info["format"] == "wav" else f"/api/jobs/{job_id}/export/{info['key']}"
    return {**info, "name": name, "url": url}


@app.get("/api/jobs/{job_id}/export/{key}")
async def job_export_download(job_id: str, key: str, request: Request):
    row = await require_job(job_id, request)
    if not EXPORT_KEY.match(key) or row["status"] != "done":
        raise HTTPException(404, "Export not found")
    directory = manager.job_directory(job_id)
    sidecar = directory / f"export_{key}.json"
    try:
        info = json.loads(await run_in_threadpool(sidecar.read_text, "utf-8"))
    except (OSError, ValueError):
        raise HTTPException(404, "Export not found") from None
    path = directory / f"export_{key}.{info['extension']}"
    if not path.is_file() or info.get("key") != key:
        raise HTTPException(404, "Export not found")
    return FileResponse(path, media_type=info["mediaType"], filename=download_name(row["track_name"], info["extension"]))


@app.get("/api/jobs/{job_id}/report")
async def job_report(job_id: str, request: Request, kind: Literal["txt", "json"] = "txt"):
    row = await require_job(job_id, request)
    if row["status"] != "done" or not row["report"]:
        raise HTTPException(409, "The report is not ready")
    stem = track_stem(row["track_name"])
    if kind == "json":
        body = json.dumps(row["report"], indent=2)
        return Response(body, media_type="application/json", headers={"Content-Disposition": f'attachment; filename="{stem}_mastered_report.json"'})
    body = report_text(row["report"], row["track_name"][:120], download_name(row["track_name"]))
    return Response(body, media_type="text/plain; charset=utf-8", headers={"Content-Disposition": f'attachment; filename="{stem}_mastered_report.txt"'})


@app.get("/api/jobs/{job_id}/eq/{kind}")
async def job_eq(job_id: str, kind: Literal["reaeq", "apo"], request: Request):
    row = await require_job(job_id, request)
    path = manager.job_directory(job_id) / f"mastered.{kind}.txt"
    if row["status"] != "done" or not path.is_file():
        raise HTTPException(404, "No EQ settings are available for this job")
    suffix = "reaeq" if kind == "reaeq" else "equalizer_apo"
    return FileResponse(path, media_type="text/plain; charset=utf-8", filename=f"{track_stem(row['track_name'])}_{suffix}.txt")


@app.delete("/api/jobs/{job_id}", status_code=204)
async def job_delete(job_id: str, request: Request):
    await require_job(job_id, request)
    await manager.remove(job_id)


app.mount("/", StaticFiles(directory=settings.static_dir, html=True), name="static")
