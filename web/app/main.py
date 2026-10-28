import re
from contextlib import asynccontextmanager
from typing import Literal

from fastapi import FastAPI, File, Form, HTTPException, UploadFile
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from starlette.concurrency import run_in_threadpool

from .config import load_settings
from .jobs import JobManager, QueueFull, describe_duration
from .middleware import BodySizeLimitMiddleware, SecurityHeadersMiddleware
from .wav_info import WavError, inspect_wav

VERSION = "1.0.0"
JOB_ID = re.compile(r"^[0-9a-f]{32}$")
CHUNK_BYTES = 1024 * 1024
BODY_OVERHEAD_BYTES = 1024 * 1024
BIT_DEPTHS = (0, 16, 24, 32)

settings = load_settings()
manager = JobManager(settings)


@asynccontextmanager
async def lifespan(_app):
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


async def store_upload(upload, destination, label):
    try:
        size = await run_in_threadpool(copy_limited, upload.file, destination, settings.max_file_bytes)
    except OverflowError:
        limit_mb = settings.max_file_bytes / (1024 * 1024)
        raise HTTPException(413, f"The {label} file is larger than {limit_mb:g} MB") from None
    if size == 0:
        raise HTTPException(400, f"The {label} file is empty")
    try:
        info = await run_in_threadpool(inspect_wav, destination)
    except WavError as error:
        raise HTTPException(400, f"The {label} file is not a usable WAV file: {error}") from None
    if info.duration < 1.0:
        raise HTTPException(400, f"The {label} file must be at least one second long")
    if info.duration > settings.max_duration_sec:
        raise HTTPException(413, f"The {label} file is longer than {describe_duration(settings.max_duration_sec)}")


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
        arguments += ["--passes", "4", "--tolerance", "0.25"]
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


def require_job(job_id):
    job = manager.get(job_id) if JOB_ID.match(job_id) else None
    if job is None or job.status == "uploading":
        raise HTTPException(404, "Job not found or expired")
    return job


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
    }


@app.post("/api/jobs", status_code=202)
async def create_job(
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
    try:
        job = manager.reserve(track.filename or "track.wav", arguments)
    except QueueFull:
        raise HTTPException(429, "The server is busy. Please try again in a few minutes.") from None
    try:
        await store_upload(reference, job.reference_path, "reference")
        await store_upload(track, job.track_path, "track")
    except BaseException:
        await manager.remove(job.id)
        raise
    manager.enqueue(job)
    return JSONResponse(
        {"id": job.id, "status": job.status, "position": manager.position(job.id)},
        status_code=202,
    )


@app.get("/api/jobs/{job_id}")
async def job_status(job_id: str):
    return manager.view(require_job(job_id))


@app.get("/api/jobs/{job_id}/download")
async def job_download(job_id: str):
    job = require_job(job_id)
    if job.status != "done" or not job.output_path.is_file():
        raise HTTPException(409, "The mastered file is not ready")
    return FileResponse(job.output_path, media_type="audio/wav", filename=job.download_name)


@app.delete("/api/jobs/{job_id}", status_code=204)
async def job_delete(job_id: str):
    require_job(job_id)
    await manager.remove(job_id)


app.mount("/", StaticFiles(directory=settings.static_dir, html=True), name="static")
