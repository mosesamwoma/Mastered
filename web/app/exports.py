
import asyncio
import json
import re
from dataclasses import dataclass
from pathlib import Path

from .media import MediaError, make_limits, run_process

ENCODE_TIMEOUT_SEC = 240
MEASURE_TIMEOUT_SEC = 120
MAX_ATTEMPTS = 6
PEAK_AIM_DB = 0.1
LOUDNESS_TOLERANCE_LU = 0.08
MAX_STEP_DB = 4.0
FILE_SIZE_LIMIT = 4 << 30
EXPORT_KEY = re.compile(r"^[a-z0-9_]{3,40}$")
MAX_EXPORTS_PER_JOB = 12
DELIVERY_RATES = (0, 48000)
PCM_BY_BITS = {16: "pcm_s16le", 24: "pcm_s24le", 32: "pcm_f32le"}


@dataclass(frozen=True)
class Quality:
    id: str
    label: str
    arguments: tuple


@dataclass(frozen=True)
class ExportFormat:
    id: str
    label: str
    extension: str
    media_type: str
    lossy: bool
    encoder: str
    description: str
    qualities: tuple
    container: tuple = ()


FORMATS = {
    "wav": ExportFormat("wav", "WAV", "wav", "audio/wav", False, "", "Uncompressed, exactly as mastered", (Quality("master", "As mastered", ()),)),
    "flac": ExportFormat(
        "flac", "FLAC", "flac", "audio/flac", False, "flac", "Lossless, about half the size of WAV",
        (Quality("lossless", "Lossless", ("-c:a", "flac", "-compression_level", "8")),),
    ),
    "aiff": ExportFormat(
        "aiff", "AIFF", "aiff", "audio/aiff", False, "pcm_s24be", "Uncompressed, for Apple and DAW workflows",
        (Quality("pcm24", "24-bit", ("-c:a", "pcm_s24be")), Quality("pcm16", "16-bit", ("-c:a", "pcm_s16be"))),
    ),
    "mp3": ExportFormat(
        "mp3", "MP3", "mp3", "audio/mpeg", True, "libmp3lame", "Plays everywhere",
        (
            Quality("320", "320 kbps", ("-c:a", "libmp3lame", "-b:a", "320k")),
            Quality("v0", "V0 (about 245 kbps, variable)", ("-c:a", "libmp3lame", "-q:a", "0")),
            Quality("192", "192 kbps", ("-c:a", "libmp3lame", "-b:a", "192k")),
            Quality("128", "128 kbps", ("-c:a", "libmp3lame", "-b:a", "128k")),
        ),
    ),
    "ogg": ExportFormat(
        "ogg", "OGG Vorbis", "ogg", "audio/ogg", True, "libvorbis", "Open format, good quality at small sizes",
        (
            Quality("q9", "Quality 9 (about 320 kbps)", ("-c:a", "libvorbis", "-q:a", "9")),
            Quality("q7", "Quality 7 (about 224 kbps)", ("-c:a", "libvorbis", "-q:a", "7")),
            Quality("q5", "Quality 5 (about 160 kbps)", ("-c:a", "libvorbis", "-q:a", "5")),
            Quality("q3", "Quality 3 (about 112 kbps)", ("-c:a", "libvorbis", "-q:a", "3")),
        ),
    ),
    "opus": ExportFormat(
        "opus", "Opus", "opus", "audio/ogg", True, "libopus", "Most efficient lossy format",
        (
            Quality("192", "192 kbps", ("-c:a", "libopus", "-b:a", "192k", "-vbr", "on")),
            Quality("160", "160 kbps", ("-c:a", "libopus", "-b:a", "160k", "-vbr", "on")),
            Quality("128", "128 kbps", ("-c:a", "libopus", "-b:a", "128k", "-vbr", "on")),
            Quality("96", "96 kbps", ("-c:a", "libopus", "-b:a", "96k", "-vbr", "on")),
        ),
        ("-f", "ogg"),
    ),
    "m4a": ExportFormat(
        "m4a", "AAC (M4A)", "m4a", "audio/mp4", True, "aac", "For Apple devices and video platforms",
        (
            Quality("256", "256 kbps", ("-c:a", "aac", "-b:a", "256k")),
            Quality("192", "192 kbps", ("-c:a", "aac", "-b:a", "192k")),
            Quality("128", "128 kbps", ("-c:a", "aac", "-b:a", "128k")),
        ),
        ("-movflags", "+faststart", "-f", "ipod"),
    ),
}
FORMAT_ORDER = ("wav", "flac", "aiff", "mp3", "ogg", "opus", "m4a")


def available_formats(encoders):
    result = []
    for key in FORMAT_ORDER:
        fmt = FORMATS[key]
        if fmt.id == "wav" or fmt.encoder in encoders:
            result.append(fmt)
    return result


def describe_formats(encoders):
    return [
        {
            "id": fmt.id,
            "label": fmt.label,
            "extension": fmt.extension,
            "lossy": fmt.lossy,
            "description": fmt.description,
            "qualities": [{"id": quality.id, "label": quality.label} for quality in fmt.qualities],
        }
        for fmt in available_formats(encoders)
    ]


def export_key(format_id, quality_id):
    return f"{format_id}_{quality_id}"


def find_quality(encoders, format_id, quality_id):
    for fmt in available_formats(encoders):
        if fmt.id == format_id:
            for quality in fmt.qualities:
                if quality.id == quality_id:
                    return fmt, quality
    return None, None


def encode_arguments(ffmpeg, source, destination, fmt, quality, gain_db, channels, sample_rate, title, rate=0, tags=None, bits=24):
    arguments = [str(ffmpeg), "-nostdin", "-hide_banner", "-loglevel", "error", "-i", str(source), "-map", "0:a:0", "-vn", "-map_metadata", "-1"]
    sixteen = fmt.id == "wav" and bits == 16 or "pcm_s16be" in quality.arguments
    filters = []
    if abs(gain_db) > 0.0005:
        filters.append(f"volume={gain_db:.4f}dB")
    if sixteen:
        filters.append("aresample=dither_method=triangular")
    if filters:
        arguments += ["-af", ",".join(filters)]
    if rate:
        arguments += ["-ar", str(rate)]
    elif fmt.id == "opus":
        arguments += ["-ar", "48000"]
    elif fmt.id in ("mp3", "m4a") and sample_rate > 48000:
        arguments += ["-ar", "48000"]
    if fmt.id in ("mp3", "m4a") and channels > 2:
        arguments += ["-ac", "2"]
    if fmt.id == "wav":
        arguments += ["-c:a", PCM_BY_BITS.get(bits, "pcm_s24le"), "-f", "wav"]
    arguments += list(quality.arguments)
    arguments += list(fmt.container)
    metadata = dict(tags or {})
    if title and "title" not in metadata:
        metadata["title"] = title
    for name in ("title", "artist", "album"):
        if metadata.get(name):
            arguments += ["-metadata", f"{name}={metadata[name]}"]
    arguments += ["-y", str(destination)]
    return arguments


async def run_checked(arguments, timeout, failure):
    try:
        code, stdout, _ = await run_process(arguments, timeout, limits=make_limits(0, timeout * 2, FILE_SIZE_LIMIT))
    except MediaError:
        raise MediaError(failure) from None
    except OSError:
        raise MediaError("the audio tools could not be started") from None
    if code != 0:
        raise MediaError(failure)
    return stdout


async def measure_file(engine, ffmpeg, path, work_directory, dual_mono, strict_peak=False):
    decoded = work_directory / f"measure_{path.stem}.wav"
    try:
        await run_checked(
            [str(ffmpeg), "-nostdin", "-hide_banner", "-loglevel", "error", "-i", str(path), "-map", "0:a:0", "-vn", "-c:a", "pcm_f32le", "-y", str(decoded)],
            MEASURE_TIMEOUT_SEC, "the exported file could not be checked",
        )
        arguments = [str(engine), "--measure", str(decoded)]
        if dual_mono:
            arguments.append("--dual-mono")
        if strict_peak:
            arguments.append("--strict-peak")
        stdout = await run_checked(arguments, MEASURE_TIMEOUT_SEC, "the exported file could not be measured")
        try:
            return json.loads(stdout.decode("utf-8", "replace"))["measurements"]
        except (ValueError, KeyError):
            raise MediaError("the exported file could not be measured") from None
    finally:
        decoded.unlink(missing_ok=True)


async def build_export(settings, directory: Path, format_id, quality_id, encoders, report, title, rate=0, tags=None):
    fmt, quality = find_quality(encoders, format_id, quality_id)
    if fmt is None:
        raise MediaError("this format is not available")
    master = directory / "mastered.wav"
    if not master.is_file():
        raise MediaError("the mastered file is no longer available")
    settings_block = report.get("settings", {})
    ceiling = float(settings_block.get("ceilingDb", -1.0))
    dual_mono = bool(settings_block.get("dualMono", False))
    strict_peak = bool(settings_block.get("strictPeak", False))
    channels = int(report.get("input", {}).get("channels", 2) or 2)
    sample_rate = int(report.get("input", {}).get("sampleRate", 44100) or 44100)
    mastered_measurements = report["output"]["measurements"]
    if fmt.id == "wav" and not rate:
        tags = None
    bits = int(report.get("output", {}).get("bitDepth", 24) or 24)
    needs_check = fmt.lossy or bool(rate) or quality.id == "pcm16"
    target_lufs = float(mastered_measurements.get("integratedLufs", -120.0))
    retarget = fmt.lossy and target_lufs > -100.0
    key = export_key(fmt.id, quality.id) + ("_48k" if rate else "") + ("_tags" if tags else "")

    def describe(path, measured, gain_db, attempts):
        return {
            "key": key,
            "format": fmt.id,
            "quality": quality.id,
            "label": (f"{fmt.label} {quality.label}" if len(fmt.qualities) > 1 else fmt.label) + (" 48 kHz" if rate else ""),
            "rate": rate,
            "extension": fmt.extension,
            "mediaType": fmt.media_type,
            "lossy": fmt.lossy,
            "bytes": path.stat().st_size,
            "gainDb": round(gain_db, 3),
            "attempts": attempts,
            "ceilingDb": ceiling,
            "overCeiling": float(measured["truePeakDb"]) > ceiling + 0.02,
            "measured": {
                "integratedLufs": measured["integratedLufs"],
                "truePeakDb": measured["truePeakDb"],
                "loudnessRangeLu": measured["loudnessRangeLu"],
            },
            "loudnessErrorLu": round(float(measured["integratedLufs"]) - target_lufs, 3) if retarget else 0.0,
        }

    if fmt.id == "wav" and not rate:
        return describe(master, mastered_measurements, 0.0, 0)

    destination = directory / f"export_{key}.{fmt.extension}"
    sidecar = directory / f"export_{key}.json"
    if destination.is_file() and sidecar.is_file():
        try:
            return json.loads(sidecar.read_text("utf-8"))
        except (OSError, ValueError):
            destination.unlink(missing_ok=True)
            sidecar.unlink(missing_ok=True)

    measured = mastered_measurements
    gain_db = 0.0
    applied_db = 0.0
    attempts = 0
    history = []
    ok_gain = None
    bad_gain = None

    async def encode_and_measure(gain):
        arguments = encode_arguments(settings.ffmpeg_path, master, destination, fmt, quality, gain, channels, sample_rate, title, rate, tags, bits)
        await run_checked(arguments, ENCODE_TIMEOUT_SEC, "the file could not be encoded")
        return await measure_file(settings.engine_path, settings.ffmpeg_path, destination, directory, dual_mono, strict_peak)

    try:
        while attempts < MAX_ATTEMPTS:
            attempts += 1
            applied_db = gain_db
            if not needs_check:
                await run_checked(
                    encode_arguments(settings.ffmpeg_path, master, destination, fmt, quality, gain_db, channels, sample_rate, title, rate, tags, bits),
                    ENCODE_TIMEOUT_SEC, "the file could not be encoded",
                )
                break
            measured = await encode_and_measure(gain_db)
            peak = float(measured["truePeakDb"])
            valid = peak <= ceiling
            room = ceiling - PEAK_AIM_DB - peak - 0.01
            if valid:
                ok_gain = applied_db if ok_gain is None else max(ok_gain, applied_db)
            else:
                bad_gain = applied_db if bad_gain is None else min(bad_gain, applied_db)
            if retarget:
                loud_error = target_lufs - float(measured["integratedLufs"])
                if valid and (abs(loud_error) <= LOUDNESS_TOLERANCE_LU or (loud_error > 0 and room <= LOUDNESS_TOLERANCE_LU)):
                    break
                change = min(loud_error, room)
                if valid and 0 <= change < 0.01:
                    break
            else:
                if valid:
                    break
                change = room
            if not valid:
                history.append((gain_db, peak))
                if len(history) >= 2:
                    lowered = history[-2][0] - history[-1][0]
                    moved = history[-2][1] - history[-1][1]
                    if lowered > 0 and moved < 0.5 * lowered:
                        change *= 1.5
                change = max(change, -MAX_STEP_DB)
            candidate = gain_db + change
            if bad_gain is not None and candidate >= bad_gain - 0.005:
                if ok_gain is None:
                    candidate = bad_gain - max(0.25, -change)
                else:
                    if bad_gain - ok_gain < 0.03:
                        break
                    candidate = 0.5 * (ok_gain + bad_gain)
            gain_db = candidate
        if needs_check and float(measured["truePeakDb"]) > ceiling and ok_gain is not None:
            applied_db = ok_gain
            attempts += 1
            measured = await encode_and_measure(ok_gain)
    except BaseException:
        destination.unlink(missing_ok=True)
        raise
    info = describe(destination, measured, applied_db, attempts)
    sidecar.write_text(json.dumps(info), "utf-8")
    return info
