import asyncio
import json
import logging
import math
import re
import shutil
import time
import uuid
from pathlib import Path

from .media import MediaError, MediaInfo, kill_group
from .store import JobStore
from .wav_info import WavError, inspect_wav

LOG = logging.getLogger("uvicorn.error")
PROGRESS_LINE = re.compile(r"^\[\s*(\d+(?:\.\d+)?)%\]\s*(.*)$")
JOB_DIRECTORY = re.compile(r"^[0-9a-f]{32}$")
STDERR_LIMIT = 65536
UPLOAD_STALE_SEC = 900
HEARTBEAT_TIMEOUT_SEC = 60
HEARTBEAT_EVERY_TICKS = 5
SWEEP_INTERVAL_SEC = 30
ORPHAN_DIRECTORY_SEC = 1800
CONVERT_PROGRESS = 8.0
ENGINE_PROGRESS_END = 96.0


class QueueFull(Exception):
    pass


class RateLimited(Exception):
    def __init__(self, message, retry_after):
        super().__init__(message)
        self.message = message
        self.retry_after = max(1, int(math.ceil(retry_after)))


def describe_duration(seconds):
    if seconds < 120:
        return f"{seconds:.0f} seconds"
    if seconds < 7200:
        return f"{seconds / 60:.0f} minutes"
    return f"{seconds / 3600:.0f} hours"


def download_name(original):
    stem = Path(original.replace("\\", "/")).stem
    cleaned = re.sub(r"[^A-Za-z0-9 ._-]", "", stem).strip(" .")[:80]
    return f"{cleaned or 'track'}_mastered.wav"


class JobManager:
    def __init__(self, settings, media):
        self.settings = settings
        self.media = media
        self.store = None
        self.worker_id = uuid.uuid4().hex[:8]
        self.processes = {}
        self.cancelled = set()
        self.last_progress = {}
        self.wake = asyncio.Event()
        self.tasks = []

    def job_directory(self, job_id):
        return self.settings.work_dir / job_id

    async def database(self, function, *arguments, **keywords):
        return await asyncio.to_thread(function, *arguments, **keywords)

    async def start(self):
        self.settings.work_dir.mkdir(parents=True, exist_ok=True)
        self.store = await asyncio.to_thread(JobStore, self.settings.database_path)
        self.tasks = [asyncio.create_task(self.work()), asyncio.create_task(self.sweep())]

    async def stop(self):
        for task in self.tasks:
            task.cancel()
        await asyncio.gather(*self.tasks, return_exceptions=True)
        for process in list(self.processes.values()):
            kill_group(process)
        if self.store is not None:
            for job_id in self.store.running_for_worker(self.worker_id):
                self.store.mark_failed(job_id, "The server restarted while this job was running", only_status="running")
            self.store.close()

    async def reserve(self, track_name, cli_args, client):
        job_id = uuid.uuid4().hex
        settings = self.settings
        status, retry = await self.database(
            self.store.reserve, job_id, track_name, cli_args, client, time.time(),
            settings.max_pending_jobs, settings.pending_per_client, settings.jobs_per_hour_per_client,
        )
        if status == "busy":
            raise QueueFull()
        if status == "client_busy":
            raise RateLimited("You already have a job in progress. Please wait for it to finish.", 30)
        if status == "rate":
            raise RateLimited(
                f"You have reached the limit of {settings.jobs_per_hour_per_client} jobs per hour. "
                f"Please try again in {describe_duration(retry)}.",
                retry,
            )
        try:
            self.job_directory(job_id).mkdir(parents=True)
        except OSError:
            await self.database(self.store.delete, job_id)
            raise
        return job_id

    async def set_sources(self, job_id, sources):
        await self.database(self.store.update, job_id, sources=sources)

    async def enqueue(self, job_id):
        await self.database(self.store.update, job_id, status="queued", message="Waiting in the queue")
        self.wake.set()

    async def row(self, job_id):
        return await self.database(self.store.get, job_id)

    async def view(self, row):
        data = {
            "id": row["id"],
            "status": row["status"],
            "progress": round(row["progress"], 1),
            "message": row["message"],
        }
        if row["status"] == "queued":
            data["position"] = await self.database(self.store.queue_position, row["id"])
        if row["status"] == "failed":
            data["error"] = row["error"]
        if row["status"] == "done":
            data["report"] = row["report"]
            data["downloadUrl"] = f"/api/jobs/{row['id']}/download"
            data["downloadName"] = download_name(row["track_name"])
            data["preview"] = None
            if row["preview"]:
                data["preview"] = {
                    "original": f"/api/jobs/{row['id']}/audio/original",
                    "mastered": f"/api/jobs/{row['id']}/audio/mastered",
                }
        return data

    async def remove(self, job_id):
        row = await self.row(job_id)
        if row is None:
            return False
        if row["status"] == "running":
            await self.database(self.store.update, job_id, cancel=True)
            self.kill_local(job_id)
            return True
        await self.discard(job_id)
        return True

    async def discard(self, job_id):
        await self.database(self.store.delete, job_id)
        await asyncio.to_thread(shutil.rmtree, self.job_directory(job_id), True)

    def kill_local(self, job_id):
        process = self.processes.get(job_id)
        if process is not None:
            self.cancelled.add(job_id)
            kill_group(process)

    def register(self, job_id):
        def callback(process):
            if process is None:
                self.processes.pop(job_id, None)
            else:
                self.processes[job_id] = process
                if job_id in self.cancelled:
                    kill_group(process)

        return callback

    async def work(self):
        while True:
            row = await self.database(self.store.claim, self.worker_id, self.settings.max_concurrent_jobs, time.time())
            if row is None:
                try:
                    await asyncio.wait_for(self.wake.wait(), timeout=1.0)
                except asyncio.TimeoutError:
                    pass
                self.wake.clear()
                continue
            try:
                await self.run(row)
            except asyncio.CancelledError:
                raise
            except Exception:
                LOG.exception("Job %s failed unexpectedly", row["id"])
                await self.fail(row["id"], "The mastering engine failed unexpectedly")

    async def sweep(self):
        while True:
            await asyncio.sleep(SWEEP_INTERVAL_SEC)
            try:
                await self.sweep_once()
            except Exception:
                LOG.exception("Cleanup sweep failed")

    async def sweep_once(self):
        now = time.time()
        for job_id in await self.database(self.store.expired_ids, now - self.settings.job_ttl_sec, now - UPLOAD_STALE_SEC):
            await self.discard(job_id)
        for row in await self.database(self.store.stale_running, now - HEARTBEAT_TIMEOUT_SEC):
            if row["cancel"]:
                await self.discard(row["id"])
            elif await self.database(self.store.mark_failed, row["id"], "The worker stopped unexpectedly", "running"):
                await self.clear_files(row["id"])
        await asyncio.to_thread(self.remove_orphan_directories)

    def remove_orphan_directories(self):
        known = self.store.all_ids()
        now = time.time()
        for child in self.settings.work_dir.iterdir():
            if not child.is_dir() or not JOB_DIRECTORY.match(child.name) or child.name in known:
                continue
            try:
                if now - child.stat().st_mtime > ORPHAN_DIRECTORY_SEC:
                    shutil.rmtree(child, ignore_errors=True)
            except OSError:
                continue

    async def clear_files(self, job_id):
        def remove():
            directory = self.job_directory(job_id)
            if directory.is_dir():
                for entry in directory.iterdir():
                    if entry.is_file():
                        entry.unlink(missing_ok=True)

        await asyncio.to_thread(remove)

    async def fail(self, job_id, message):
        await self.database(self.store.mark_failed, job_id, message)
        await self.clear_files(job_id)

    async def progress(self, job_id, value, message, force=False):
        now = time.monotonic()
        previous = self.last_progress.get(job_id)
        if not force and previous is not None:
            last_value, last_message, last_time = previous
            if message == last_message and value - last_value < 1.0 and now - last_time < 2.0:
                return
        self.last_progress[job_id] = (value, message, now)
        await self.database(self.store.update, job_id, progress=min(100.0, value), message=message, heartbeat=time.time())

    async def monitor(self, job_id):
        ticks = 0
        while True:
            await asyncio.sleep(1.0)
            ticks += 1
            row = await self.database(self.store.get, job_id)
            if row is None or row["cancel"]:
                self.cancelled.add(job_id)
                kill_group(self.processes.get(job_id))
                return
            if ticks % HEARTBEAT_EVERY_TICKS == 0:
                await self.database(self.store.update, job_id, heartbeat=time.time())

    async def run(self, row):
        job_id = row["id"]
        monitor = asyncio.create_task(self.monitor(job_id))
        try:
            await self.execute(row)
        finally:
            monitor.cancel()
            await asyncio.gather(monitor, return_exceptions=True)
            self.processes.pop(job_id, None)
            self.last_progress.pop(job_id, None)
            if job_id in self.cancelled:
                self.cancelled.discard(job_id)
                await self.discard(job_id)

    def is_cancelled(self, job_id):
        return job_id in self.cancelled

    async def convert_sources(self, row):
        job_id = row["id"]
        directory = self.job_directory(job_id)
        converted = False
        for index, label in enumerate(("reference", "track")):
            source = row["sources"].get(label, {})
            if not source.get("convert"):
                continue
            await self.progress(job_id, 1.0 + 3.0 * index, f"Converting the {label} file", force=True)
            info = MediaInfo(source["codec"], source["channels"], source["sample_rate"], source["bits"], source["duration"])
            raw = directory / f"{label}.src"
            wav = directory / f"{label}.wav"
            try:
                await self.media.convert_to_wav(
                    raw, wav, info, self.settings.max_duration_sec, self.settings.convert_timeout_sec, self.register(job_id)
                )
                converted_info = await asyncio.to_thread(inspect_wav, wav)
                if converted_info.duration > self.settings.max_duration_sec + 1.0:
                    raise MediaError("the audio is longer than the allowed length")
            except (MediaError, WavError) as error:
                if self.is_cancelled(job_id):
                    return None
                await self.fail(job_id, f"The {label} file could not be converted: {error}")
                return None
            raw.unlink(missing_ok=True)
            converted = True
        return converted

    async def run_engine(self, row, base):
        job_id = row["id"]
        directory = self.job_directory(job_id)
        command = [
            str(self.settings.engine_path),
            str(directory / "reference.wav"),
            str(directory / "track.wav"),
            "--output", str(directory / "mastered.wav"),
            "--json", str(directory / "report.json"),
            *row["cli_args"],
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
            return "The mastering engine could not be started"
        self.register(job_id)(process)
        stderr_task = asyncio.create_task(self.collect(process.stderr))
        timed_out = False
        try:
            await asyncio.wait_for(self.follow(job_id, process, base), timeout=self.settings.job_timeout_sec)
        except asyncio.TimeoutError:
            timed_out = True
        except BaseException:
            stderr_task.cancel()
            raise
        finally:
            kill_group(process)
            await process.wait()
            self.register(job_id)(None)
        stderr = await stderr_task
        if self.is_cancelled(job_id):
            return None
        if timed_out:
            return f"Mastering took longer than {describe_duration(self.settings.job_timeout_sec)} and was stopped"
        if process.returncode != 0:
            return self.engine_error(stderr, job_id)
        return ""

    async def collect(self, stream):
        kept = b""
        while True:
            chunk = await stream.read(65536)
            if not chunk:
                break
            if len(kept) < STDERR_LIMIT:
                kept += chunk[: STDERR_LIMIT - len(kept)]
        return kept.decode("utf-8", "replace")

    async def follow(self, job_id, process, base):
        async for raw in process.stdout:
            match = PROGRESS_LINE.match(raw.decode("utf-8", "replace").strip())
            if match:
                percent = min(100.0, float(match.group(1)))
                await self.progress(job_id, base + percent * (ENGINE_PROGRESS_END - base) / 100.0, match.group(2)[:120])
        await process.wait()

    def clean_message(self, text, job_id):
        directory = self.job_directory(job_id)
        for name, label in (("reference.wav", "reference file"), ("track.wav", "track file")):
            text = text.replace(str(directory / name), label)
        return text.replace(str(directory), "job").strip()[:300]

    def engine_error(self, stderr, job_id):
        lines = [line.strip() for line in stderr.splitlines() if line.strip()]
        for line in reversed(lines):
            if line.startswith("Error:"):
                return self.clean_message(line[len("Error:"):], job_id) or "The mastering engine reported an error"
        if lines:
            return self.clean_message(lines[-1], job_id)
        return "The mastering engine reported an error"

    async def make_previews(self, job_id):
        if self.media.preview_codec is None:
            return False
        directory = self.job_directory(job_id)
        extension = self.media.preview_extension
        try:
            await self.media.make_preview(directory / "track.wav", directory / f"original.{extension}", self.register(job_id))
            await self.media.make_preview(directory / "mastered.wav", directory / f"mastered.{extension}", self.register(job_id))
        except (MediaError, OSError):
            return False
        return True

    async def execute(self, row):
        job_id = row["id"]
        converted = await self.convert_sources(row)
        if converted is None:
            return
        base = CONVERT_PROGRESS if converted else 0.0
        failure = await self.run_engine(row, base)
        if failure is None:
            return
        if failure:
            await self.fail(job_id, failure)
            return
        directory = self.job_directory(job_id)
        try:
            report = json.loads(await asyncio.to_thread((directory / "report.json").read_text, "utf-8"))
            if not (directory / "mastered.wav").is_file():
                raise FileNotFoundError
        except (OSError, ValueError):
            await self.fail(job_id, "The mastering engine finished without producing a result")
            return
        report["input"]["path"] = row["track_name"][:120]
        report["output"]["path"] = download_name(row["track_name"])
        await self.progress(job_id, 97.0, "Preparing the preview", force=True)
        preview = await self.make_previews(job_id)
        if self.is_cancelled(job_id):
            return
        for name in ("reference.wav", "track.wav", "report.json"):
            (directory / name).unlink(missing_ok=True)
        await self.database(
            self.store.update, job_id, status="done", progress=100.0, message="Done",
            finished=time.time(), report=report, preview=preview,
        )
