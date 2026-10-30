import struct
from dataclasses import dataclass
from pathlib import Path

FORMAT_PCM = 1
FORMAT_FLOAT = 3
FORMAT_EXTENSIBLE = 0xFFFE
UNKNOWN_SIZE = 0xFFFFFFFF
DS64_PAYLOAD = 28


class WavError(ValueError):
    pass


@dataclass(frozen=True)
class WavInfo:
    channels: int
    sample_rate: int
    bits: int
    frames: int

    @property
    def duration(self):
        return self.frames / self.sample_rate


def inspect_wav(path: Path) -> WavInfo:
    size = path.stat().st_size
    fmt = None
    data = None
    ds64_data_size = None
    ds64_table = {}
    with path.open("rb") as handle:
        header = handle.read(12)
        rf64 = header[:4] in (b"RF64", b"BW64")
        if len(header) < 12 or (header[:4] != b"RIFF" and not rf64) or header[8:12] != b"WAVE":
            raise WavError("not a RIFF/WAVE file")
        position = 12
        while position + 8 <= size and not (fmt is not None and data is not None):
            handle.seek(position)
            chunk = handle.read(8)
            if len(chunk) < 8:
                break
            chunk_id = chunk[:4]
            chunk_size = int.from_bytes(chunk[4:8], "little")
            payload = position + 8
            if rf64 and chunk_size == UNKNOWN_SIZE and chunk_id in ds64_table:
                chunk_size = ds64_table[chunk_id]
            if rf64 and chunk_id == b"ds64" and ds64_data_size is None:
                if chunk_size < DS64_PAYLOAD:
                    raise WavError("invalid ds64 chunk")
                body = handle.read(min(chunk_size, DS64_PAYLOAD + 12 * 32))
                if len(body) < DS64_PAYLOAD:
                    raise WavError("invalid ds64 chunk")
                ds64_data_size = struct.unpack_from("<Q", body, 8)[0]
                table_length = struct.unpack_from("<I", body, 24)[0]
                for index in range(table_length):
                    start = DS64_PAYLOAD + 12 * index
                    if start + 12 > len(body):
                        break
                    ds64_table[body[start:start + 4]] = struct.unpack_from("<Q", body, start + 4)[0]
            elif chunk_id == b"fmt " and fmt is None:
                if chunk_size < 16:
                    raise WavError("invalid fmt chunk")
                body = handle.read(min(chunk_size, 40))
                if len(body) < 16:
                    raise WavError("invalid fmt chunk")
                tag, channels, sample_rate = struct.unpack_from("<HHI", body, 0)
                block_align, bits = struct.unpack_from("<HH", body, 12)
                if tag == FORMAT_EXTENSIBLE:
                    if len(body) < 26:
                        raise WavError("invalid extensible fmt chunk")
                    tag = struct.unpack_from("<H", body, 24)[0]
                fmt = (tag, channels, sample_rate, block_align, bits)
            elif chunk_id == b"data" and data is None:
                if rf64 and ds64_data_size is not None and chunk_size == UNKNOWN_SIZE:
                    chunk_size = ds64_data_size
                data = (payload, chunk_size)
            position = payload + chunk_size + (chunk_size & 1)
    if fmt is None:
        raise WavError("no fmt chunk found")
    if data is None:
        raise WavError("no data chunk found")
    tag, channels, sample_rate, block_align, bits = fmt
    if tag == FORMAT_PCM:
        if bits not in (16, 24, 32):
            raise WavError(f"unsupported PCM bit depth {bits}")
    elif tag == FORMAT_FLOAT:
        if bits not in (32, 64):
            raise WavError(f"unsupported float bit depth {bits}")
    else:
        raise WavError(f"unsupported audio format {tag}")
    if not 1 <= channels <= 8:
        raise WavError(f"unsupported channel count {channels}")
    if not 8000 <= sample_rate <= 192000:
        raise WavError(f"unsupported sample rate {sample_rate}")
    if block_align != channels * (bits // 8):
        raise WavError("inconsistent block alignment")
    offset, declared = data
    available = max(0, size - offset)
    frames = min(declared, available) // block_align
    if frames == 0:
        raise WavError("file contains no audio")
    return WavInfo(channels=channels, sample_rate=sample_rate, bits=bits, frames=frames)
