import asyncio
import json
import os
import re
import signal
import unicodedata
from dataclasses import dataclass, field
from pathlib import Path

try:
    import resource
except ImportError:
    resource = None

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
LOSSY_CODECS = {"mp3", "aac", "vorbis", "opus", "wmav2", "ac3", "eac3", "mp2"}
TAG_FIELDS = ("title", "artist", "album")
TAG_LIMIT = 120


class MediaError(Exception):
    pass


@dataclass(frozen=True)
class MediaInfo:
    codec: str
    channels: int
    sample_rate: int
    bits: int
    duration: float
    tags: dict = field(default_factory=dict, compare=False)

    def decoded_bytes(self):
        width = 2 if 0 < self.bits <= 16 else 3
        rate = self.sample_rate if MIN_RATE <= self.sample_rate <= MAX_RATE else 44100
        channels = min(self.channels, MAX_CHANNELS) if self.channels <= MAX_CHANNELS else 2
        return int(self.duration * rate * channels * width)


def has_audio_signature(path):
    try:
        with open(path, "rb") as handle:
            head = handle.read(64)
    except OSError:
        return False
    if len(head) < 12:
        return False
    if head[:4] in (b"RIFF", b"RF64", b"BW64") and head[8:12] in (b"WAVE", b"WAV "):
        return True
    if head[:4] == b"FORM" and head[8:12] in (b"AIFF", b"AIFC"):
        return True
    if head[:4] in (b"fLaC", b"OggS", b"\x1a\x45\xdf\xa3"):
        return True
    if head[4:8] == b"ftyp":
        return True
    if head[:3] == b"ID3":
        return True
    if head[:16] == bytes.fromhex("726966662e91cf11a5d628db04c10000"):
        return True
    return head[0] == 0xFF and (head[1] & 0xE0) == 0xE0


def clean_tag(value):
    text = unicodedata.normalize("NFC", str(value))
    text = "".join(ch for ch in text if ch.isprintable())
    return text.strip()[:TAG_LIMIT]


def make_limits(memory_bytes, cpu_seconds):
    if resource is None:
        return None

    def apply():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        if cpu_seconds:
            resource.setrlimit(resource.RLIMIT_CPU, (int(cpu_seconds), int(cpu_seconds) + 5))
        if memory_bytes:
            resource.setrlimit(resource.RLIMIT_AS, (int(memory_bytes), int(memory_bytes)))

    return apply


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


async def run_process(arguments, timeout, on_start=None, limits=None):
    process = await asyncio.create_subprocess_exec(
        *arguments,
        stdin=asyncio.subprocess.DEVNULL,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
        start_new_session=True,
        preexec_fn=limits,
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
        self.encoders = frozenset()

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
        self.encoders = frozenset()
        if not self.available:
            return
        try:
            code, stdout, _ = await run_process([str(self.ffmpeg), "-hide_banner", "-encoders"], 20)
        except (MediaError, OSError):
            return
        if code != 0:
            return
        listing = stdout.decode("utf-8", "replace")
        self.encoders = frozenset(
            match.group(1) for match in re.finditer(r"^\s*A\S*\s+(\S+)", listing, re.MULTILINE)
        )
        if re.search(r"^\s*A\S*\s+libmp3lame\b", listing, re.MULTILINE):
            self.preview_codec = "mp3"
        elif re.search(r"^\s*A\S*\s+aac\b", listing, re.MULTILINE):
            self.preview_codec = "m4a"

    async def probe(self, path: Path, on_start=None):
        arguments = [
            str(self.ffprobe), "-v", "error", "-protocol_whitelist", "file",
            "-select_streams", "a:0",
            "-show_entries",
            "stream=codec_name,channels,sample_rate,bits_per_raw_sample,duration:format=format_name,duration:format_tags=title,artist,album",
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
        raw_tags = data.get("format", {}).get("tags") or {}
        lowered = {str(key).lower(): value for key, value in raw_tags.items()} if isinstance(raw_tags, dict) else {}
        tags = {name: clean_tag(lowered[name]) for name in TAG_FIELDS if lowered.get(name) and clean_tag(lowered[name])}
        return MediaInfo(str(stream.get("codec_name") or ""), channels, sample_rate, bits, duration, tags)

    async def convert_to_wav(self, source: Path, destination: Path, info: MediaInfo, max_duration, timeout, on_start=None):
        pcm = "pcm_s16le" if 0 < info.bits <= 16 else "pcm_s24le"
        arguments = [
            str(self.ffmpeg), "-nostdin", "-hide_banner", "-loglevel", "error",
            "-protocol_whitelist", "file", "-i", str(source),
            "-map", "0:a:0", "-vn", "-sn", "-dn", "-map_metadata", "-1", "-t", f"{max_duration + 1:.3f}",
        ]
        if info.channels > MAX_CHANNELS:
            arguments += ["-ac", "2"]
        if not MIN_RATE <= info.sample_rate <= MAX_RATE:
            arguments += ["-ar", "44100"]
        arguments += ["-c:a", pcm, "-f", "wav", "-y", str(destination)]
        try:
            code, _, _ = await run_process(arguments, timeout, on_start, make_limits(0, timeout * 2))
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


def describe_conversion(info):
    notes = []
    label = info.codec.upper() if info.codec else "audio"
    notes.append(f"The {label} file was decoded to WAV before mastering")
    if info.codec in LOSSY_CODECS:
        notes.append("The source is a lossy file; mastering cannot bring back detail that the encoder already removed")
    if info.channels > MAX_CHANNELS:
        notes.append(f"The source has {info.channels} channels and was mixed down to stereo")
    if not MIN_RATE <= info.sample_rate <= MAX_RATE:
        notes.append(f"The source sample rate of {info.sample_rate} Hz is outside the supported range and was resampled to 44100 Hz")
    if 0 < info.bits <= 16:
        notes.append("The decoded audio was kept at 16 bits")
    return notes
