import asyncio
import json
import os
import re
import shutil
import signal
import time
import uuid
from dataclasses import dataclass, field
from pathlib import Path

PROGRESS_LINE = re.compile(r"^\[\s*(\d+(?:\.\d+)?)%\]\s*(.*)$")
STDERR_LIMIT = 65536
UPLOAD_STALE_SEC = 900


class QueueFull(Exception):
    pass


def describe_duration(seconds):
    if seconds < 120:
        return f"{seconds:.0f} seconds"
    return f"{seconds / 60:.0f} minutes"


def download_name(original):
    stem = Path(original.replace("\\", "/")).stem
    cleaned = re.sub(r"[^A-Za-z0-9 ._-]", "", stem).strip(" .")[:80]
    return f"{cleaned or 'track'}_mastered.wav"


@dataclass
class Job:
    id: str
    directory: Path
    track_name: str
    cli_args: list
    created: float = field(default_factory=time.time)
    status: str = "uploading"
    progress: float = 0.0
    message: str = "Waiting for upload"
    error: str = ""
    report: dict = None
    finished: float = 0.0
    process: asyncio.subprocess.Process = None

    @property
    def reference_path(self):
        return self.directory / "reference.wav"

    @property
    def track_path(self):
        return self.directory / "track.wav"

    @property
    def output_path(self):
        return self.directory / "mastered.wav"

    @property
    def report_path(self):
        return self.directory / "report.json"

    @property
    def download_name(self):
        return download_name(self.track_name)


class JobManager:
    def __init__(self, settings):
        self.settings = settings
        self.jobs = {}
        self.waiting = []
        self.queue = asyncio.Queue()
        self.worker = None
        self.janitor = None

    async def start(self):
        shutil.rmtree(self.settings.work_dir, ignore_errors=True)
        self.settings.work_dir.mkdir(parents=True, exist_ok=True)
        self.worker = asyncio.create_task(self.work())
        self.janitor = asyncio.create_task(self.sweep())

    async def stop(self):
        tasks = [task for task in (self.worker, self.janitor) if task is not None]
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        for job in list(self.jobs.values()):
            self.kill(job)
        shutil.rmtree(self.settings.work_dir, ignore_errors=True)

    def pending_count(self):
        return sum(1 for job in self.jobs.values() if job.status in ("uploading", "queued", "running"))

    def reserve(self, track_name, cli_args):
        if self.pending_count() >= self.settings.max_pending_jobs:
            raise QueueFull()
        job_id = uuid.uuid4().hex
        directory = self.settings.work_dir / job_id
        directory.mkdir(parents=True)
        job = Job(id=job_id, directory=directory, track_name=track_name, cli_args=cli_args)
        self.jobs[job_id] = job
        return job

    def enqueue(self, job):
        job.status = "queued"
        job.message = "Waiting in the queue"
        self.waiting.append(job.id)
        self.queue.put_nowait(job.id)

    def get(self, job_id):
        return self.jobs.get(job_id)

    def position(self, job_id):
        try:
            return self.waiting.index(job_id) + 1
        except ValueError:
            return 0

    def view(self, job):
        data = {
            "id": job.id,
            "status": job.status,
            "progress": round(job.progress, 1),
            "message": job.message,
        }
        if job.status == "queued":
            data["position"] = self.position(job.id)
        if job.status == "failed":
            data["error"] = job.error
        if job.status == "done":
            data["report"] = job.report
            data["downloadUrl"] = f"/api/jobs/{job.id}/download"
            data["downloadName"] = job.download_name
        return data

    async def remove(self, job_id):
        job = self.jobs.pop(job_id, None)
        if job is None:
            return False
        if job_id in self.waiting:
            self.waiting.remove(job_id)
        self.kill(job)
        await asyncio.to_thread(shutil.rmtree, job.directory, True)
        return True

    def kill(self, job):
        process = job.process
        if process is None or process.returncode is not None:
            return
        try:
            if hasattr(os, "killpg"):
                os.killpg(process.pid, signal.SIGKILL)
            else:
                process.kill()
        except (ProcessLookupError, PermissionError):
            pass

    async def work(self):
        while True:
            job_id = await self.queue.get()
            job = self.jobs.get(job_id)
            if job_id in self.waiting:
                self.waiting.remove(job_id)
            if job is None or job.status != "queued":
                continue
            try:
                await self.run(job)
            except asyncio.CancelledError:
                raise
            except Exception:
                self.fail(job, "The mastering engine failed unexpectedly")

    async def sweep(self):
        while True:
            await asyncio.sleep(30)
            now = time.time()
            for job in list(self.jobs.values()):
                expired = job.status in ("done", "failed") and now - job.finished > self.settings.job_ttl_sec
                stale = job.status == "uploading" and now - job.created > UPLOAD_STALE_SEC
                if expired or stale:
                    await self.remove(job.id)

    def fail(self, job, message):
        job.status = "failed"
        job.error = message
        job.message = "Failed"
        job.finished = time.time()

    def clean_message(self, text, job):
        text = text.replace(str(job.reference_path), "reference file")
        text = text.replace(str(job.track_path), "track file")
        text = text.replace(str(job.directory), "job")
        return text.strip()[:300]

    def engine_error(self, stderr, job):
        lines = [line.strip() for line in stderr.splitlines() if line.strip()]
        for line in reversed(lines):
            if line.startswith("Error:"):
                return self.clean_message(line[len("Error:"):], job) or "The mastering engine reported an error"
        if lines:
            return self.clean_message(lines[-1], job)
        return "The mastering engine reported an error"

    async def collect(self, stream):
        kept = b""
        while True:
            chunk = await stream.read(65536)
            if not chunk:
                break
            if len(kept) < STDERR_LIMIT:
                kept += chunk[: STDERR_LIMIT - len(kept)]
        return kept.decode("utf-8", "replace")

    async def follow(self, job, process):
        async for raw in process.stdout:
            match = PROGRESS_LINE.match(raw.decode("utf-8", "replace").strip())
            if match:
                job.progress = min(100.0, float(match.group(1)))
                job.message = match.group(2)[:120]
        await process.wait()

    async def run(self, job):
        job.status = "running"
        job.message = "Starting"
        command = [
            str(self.settings.engine_path),
            str(job.reference_path),
            str(job.track_path),
            "--output",
            str(job.output_path),
            "--json",
            str(job.report_path),
            *job.cli_args,
        ]
        try:
            process = await asyncio.create_subprocess_exec(
                *command,
                stdin=asyncio.subprocess.DEVNULL,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
                limit=1 << 20,
                start_new_session=True,
            )
        except OSError:
            self.fail(job, "The mastering engine could not be started")
            return
        job.process = process
        stderr_task = asyncio.create_task(self.collect(process.stderr))
        timed_out = False
        try:
            await asyncio.wait_for(self.follow(job, process), timeout=self.settings.job_timeout_sec)
        except asyncio.TimeoutError:
            timed_out = True
        except BaseException:
            stderr_task.cancel()
            raise
        finally:
            self.kill(job)
            await process.wait()
        stderr = await stderr_task
        job.process = None
        if self.jobs.get(job.id) is not job:
            return
        if timed_out:
            limit = describe_duration(self.settings.job_timeout_sec)
            self.fail(job, f"Mastering took longer than {limit} and was stopped")
            return
        if process.returncode != 0:
            self.fail(job, self.engine_error(stderr, job))
            return
        try:
            report = json.loads(job.report_path.read_text(encoding="utf-8"))
            if not job.output_path.is_file():
                raise FileNotFoundError
        except (OSError, ValueError):
            self.fail(job, "The mastering engine finished without producing a result")
            return
        report["input"]["path"] = job.track_name[:120]
        report["output"]["path"] = job.download_name
        job.report = report
        job.status = "done"
        job.progress = 100.0
        job.message = "Done"
        job.finished = time.time()
