import os
import re
from contextlib import asynccontextmanager
from typing import Literal

from fastapi import FastAPI, File, Form, HTTPException, Request, UploadFile
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from starlette.concurrency import run_in_threadpool

from .config import load_settings
from .jobs import JobManager, QueueFull, RateLimited, describe_duration
from .media import SOURCE_EXTENSIONS, MediaError, MediaTools
from .middleware import BodySizeLimitMiddleware, SecurityHeadersMiddleware
from .ratelimit import RateLimitMiddleware, client_key
from .wav_info import WavError, inspect_wav

VERSION = "2.0.0"
JOB_ID = re.compile(r"^[0-9a-f]{32}$")
CHUNK_BYTES = 1024 * 1024
BODY_OVERHEAD_BYTES = 1024 * 1024
BIT_DEPTHS = (0, 16, 24, 32)

settings = load_settings()
media = MediaTools(settings.ffmpeg_path, settings.ffprobe_path)
manager = JobManager(settings, media)


@asynccontextmanager
async def lifespan(_app):
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
    RateLimitMiddleware,
    requests_per_minute=settings.api_requests_per_minute,
    uploads_per_hour=settings.upload_attempts_per_hour,
    header=settings.client_ip_header,
)
app.add_middleware(SecurityHeadersMiddleware)


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
):
    arguments = [
        "--target", f"{target_lufs:.3f}",
        "--aggressive", f"{aggressiveness:.4f}",
        "--ceiling", f"{ceiling_db:.3f}",
        "--bands", str(bands),
    ]
    if bit_depth:
        arguments += ["--bitdepth", str(bit_depth)]
    if precision == "fast":
        arguments += ["--passes", "4", "--tolerance", "0.25", "--fast-peak"]
    elif precision == "high":
        arguments += ["--passes", "15", "--tolerance", "0.05"]
    if dynamics:
        arguments.append("--dynamics")
    if soft_clip:
        arguments.append("--soft-clip")
    if not dither:
        arguments.append("--no-dither")
    if noise_shaping:
        arguments.append("--noise-shaping")
    if not mid_side:
        arguments.append("--no-mid-side")
    if not match_width:
        arguments.append("--no-width")
    return arguments


async def require_job(job_id):
    row = await manager.row(job_id) if JOB_ID.match(job_id) else None
    if row is None or row["cancel"] or row["status"] == "uploading":
        raise HTTPException(404, "Job not found or expired")
    return row


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
        "preview": media.preview_codec is not None,
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
):
    if bit_depth not in BIT_DEPTHS:
        raise HTTPException(422, "bit_depth must be 0, 16, 24 or 32")
    arguments = build_engine_arguments(
        target_lufs, aggressiveness, ceiling_db, bands, bit_depth, precision,
        dynamics, soft_clip, dither, noise_shaping, mid_side, match_width,
    )
    client = client_key(request.scope, settings.client_ip_header)
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
async def job_status(job_id: str):
    return await manager.view(await require_job(job_id))


@app.get("/api/jobs/{job_id}/download")
async def job_download(job_id: str):
    row = await require_job(job_id)
    path = manager.job_directory(job_id) / "mastered.wav"
    if row["status"] != "done" or not path.is_file():
        raise HTTPException(409, "The mastered file is not ready")
    return FileResponse(path, media_type="audio/wav", filename=(await manager.view(row))["downloadName"])


@app.get("/api/jobs/{job_id}/audio/{version}")
async def job_audio(job_id: str, version: Literal["original", "mastered"]):
    row = await require_job(job_id)
    path = manager.job_directory(job_id) / f"{version}.{media.preview_extension}"
    if row["status"] != "done" or not row["preview"] or not path.is_file():
        raise HTTPException(404, "No preview is available for this job")
    return FileResponse(path, media_type=media.preview_media_type)


@app.delete("/api/jobs/{job_id}", status_code=204)
async def job_delete(job_id: str):
    await require_job(job_id)
    await manager.remove(job_id)


app.mount("/", StaticFiles(directory=settings.static_dir, html=True), name="static")
