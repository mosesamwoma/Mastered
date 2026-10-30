import asyncio
import json
import os
import re
import signal
from dataclasses import dataclass
from pathlib import Path

ALLOWED_CONTAINERS = {
    "mp3", "flac", "ogg", "mov", "mp4", "m4a", "3gp", "3g2", "mj2",
    "aac", "wav", "w64", "aiff", "matroska", "webm",
}
SOURCE_EXTENSIONS = (".wav", ".mp3", ".flac", ".m4a", ".ogg", ".aac")
PROBE_TIMEOUT_SEC = 20
PREVIEW_TIMEOUT_SEC = 180
MIN_RATE = 8000
MAX_RATE = 192000
MAX_CHANNELS = 8


class MediaError(Exception):
    pass


@dataclass(frozen=True)
class MediaInfo:
    codec: str
    channels: int
    sample_rate: int
    bits: int
    duration: float


def kill_group(process):
    if process is None or process.returncode is not None:
        return
    try:
        if hasattr(os, "killpg"):
            os.killpg(process.pid, signal.SIGKILL)
        else:
            process.kill()
    except (ProcessLookupError, PermissionError):
        pass


async def run_process(arguments, timeout, on_start=None):
    process = await asyncio.create_subprocess_exec(
        *arguments,
        stdin=asyncio.subprocess.DEVNULL,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
        start_new_session=True,
    )
    if on_start is not None:
        on_start(process)
    try:
        stdout, stderr = await asyncio.wait_for(process.communicate(), timeout=timeout)
    except asyncio.TimeoutError:
        kill_group(process)
        await process.wait()
        raise MediaError("the operation timed out") from None
    except BaseException:
        kill_group(process)
        await process.wait()
        raise
    finally:
        if on_start is not None:
            on_start(None)
    return process.returncode, stdout, stderr.decode("utf-8", "replace")


class MediaTools:
    def __init__(self, ffmpeg, ffprobe):
        self.ffmpeg = ffmpeg
        self.ffprobe = ffprobe
        self.preview_codec = None

    @property
    def available(self):
        return self.ffmpeg is not None and self.ffprobe is not None

    @property
    def preview_media_type(self):
        return "audio/mpeg" if self.preview_codec == "mp3" else "audio/mp4"

    @property
    def preview_extension(self):
        return "mp3" if self.preview_codec == "mp3" else "m4a"

    async def detect_preview_codec(self):
        self.preview_codec = None
        if not self.available:
            return
        try:
            code, stdout, _ = await run_process([str(self.ffmpeg), "-hide_banner", "-encoders"], 20)
        except (MediaError, OSError):
            return
        if code != 0:
            return
        listing = stdout.decode("utf-8", "replace")
        if re.search(r"^\s*A\S*\s+libmp3lame\b", listing, re.MULTILINE):
            self.preview_codec = "mp3"
        elif re.search(r"^\s*A\S*\s+aac\b", listing, re.MULTILINE):
            self.preview_codec = "m4a"

    async def probe(self, path: Path, on_start=None):
        arguments = [
            str(self.ffprobe), "-v", "error", "-protocol_whitelist", "file",
            "-select_streams", "a:0",
            "-show_entries", "stream=codec_name,channels,sample_rate,bits_per_raw_sample,duration:format=format_name,duration",
            "-of", "json", str(path),
        ]
        try:
            code, stdout, _ = await run_process(arguments, PROBE_TIMEOUT_SEC, on_start)
        except OSError:
            raise MediaError("the audio tools could not be started") from None
        if code != 0:
            raise MediaError("the file could not be read as audio")
        try:
            data = json.loads(stdout.decode("utf-8", "replace"))
            stream = data["streams"][0]
        except (ValueError, KeyError, IndexError):
            raise MediaError("the file has no audio stream") from None
        containers = set(str(data.get("format", {}).get("format_name", "")).split(","))
        if not containers & ALLOWED_CONTAINERS:
            raise MediaError("this file type is not supported")
        duration = None
        for candidate in (stream.get("duration"), data.get("format", {}).get("duration")):
            try:
                value = float(candidate)
            except (TypeError, ValueError):
                continue
            if value > 0:
                duration = value
                break
        if duration is None:
            raise MediaError("the length of the audio could not be determined")
        try:
            channels = int(stream.get("channels") or 0)
            sample_rate = int(stream.get("sample_rate") or 0)
            bits = int(stream.get("bits_per_raw_sample") or 0)
        except (TypeError, ValueError):
            raise MediaError("the audio format could not be read") from None
        if channels < 1 or sample_rate < 1:
            raise MediaError("the audio format could not be read")
        return MediaInfo(str(stream.get("codec_name") or ""), channels, sample_rate, bits, duration)

    async def convert_to_wav(self, source: Path, destination: Path, info: MediaInfo, max_duration, timeout, on_start=None):
        pcm = "pcm_s16le" if 0 < info.bits <= 16 else "pcm_s24le"
        arguments = [
            str(self.ffmpeg), "-nostdin", "-hide_banner", "-loglevel", "error",
            "-protocol_whitelist", "file", "-i", str(source),
            "-map", "0:a:0", "-vn", "-sn", "-dn", "-t", f"{max_duration + 1:.3f}",
        ]
        if info.channels > MAX_CHANNELS:
            arguments += ["-ac", "2"]
        if not MIN_RATE <= info.sample_rate <= MAX_RATE:
            arguments += ["-ar", "44100"]
        arguments += ["-c:a", pcm, "-f", "wav", "-y", str(destination)]
        try:
            code, _, _ = await run_process(arguments, timeout, on_start)
        except OSError:
            raise MediaError("the audio tools could not be started") from None
        if code != 0 or not destination.is_file():
            destination.unlink(missing_ok=True)
            raise MediaError("the file could not be decoded")

    async def make_preview(self, source: Path, destination: Path, on_start=None):
        encoder = ["-c:a", "libmp3lame", "-b:a", "160k"] if self.preview_codec == "mp3" else ["-c:a", "aac", "-b:a", "160k"]
        arguments = [
            str(self.ffmpeg), "-nostdin", "-hide_banner", "-loglevel", "error",
            "-i", str(source), "-vn", "-ac", "2", "-ar", "44100", *encoder, "-y", str(destination),
        ]
        try:
            code, _, _ = await run_process(arguments, PREVIEW_TIMEOUT_SEC, on_start)
        except OSError:
            raise MediaError("the audio tools could not be started") from None
        if code != 0 or not destination.is_file():
            destination.unlink(missing_ok=True)
            raise MediaError("the preview could not be created")
