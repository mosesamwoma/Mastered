"""Plain-text version of the mastering report, for people who want to keep or share the numbers."""


def number(value, digits=2, unit=""):
    if value is None:
        return "n/a"
    try:
        text = f"{float(value):.{digits}f}"
    except (TypeError, ValueError):
        return "n/a"
    return f"{text} {unit}".rstrip()


def measurement_row(label, m):
    return (
        f"  {label:<10} {number(m.get('integratedLufs'), 2):>8} LUFS  {number(m.get('truePeakDb'), 2):>8} dBTP  "
        f"LRA {number(m.get('loudnessRangeLu'), 1):>5} LU  PLR {number(m.get('plrDb'), 1):>5} dB"
    )


def band_lines(title, bands):
    if not bands:
        return [f"  {title}: no correction needed"]
    lines = [f"  {title}:"]
    for index, band in enumerate(bands, 1):
        lines.append(
            f"    {index}. {band.get('type', 'peak'):<10} {number(band.get('frequency'), 1):>9} Hz  "
            f"{float(band.get('gain', 0.0)):+6.2f} dB  Q {number(band.get('q'), 2)}"
        )
    return lines


def report_text(report, track_name, output_name):
    settings = report.get("settings", {})
    gain = report.get("gain", {})
    limiter = report.get("limiter", {})
    spectrum = report.get("spectrum", {})
    match = spectrum.get("match", {})
    lines = [
        f"Mastered {report.get('version', '')} report",
        "",
        f"Track:  {track_name}",
        f"Output: {output_name}",
        "",
        "Settings",
        f"  Loudness target   {number(settings.get('targetLufs'), 2)} LUFS (tolerance {number(settings.get('toleranceLu'), 3)} LU)",
        f"  True-peak ceiling {number(settings.get('ceilingDb'), 2)} dBTP",
        f"  Limiter           look-ahead {number(settings.get('lookaheadMs'), 1)} ms, release {number(settings.get('releaseMs'), 0)} ms",
        "",
        "Measurements",
        measurement_row("Input", report.get("input", {}).get("measurements", {})),
        measurement_row("Reference", report.get("references", {}).get("measurements", {})),
        measurement_row("Output", report.get("output", {}).get("measurements", {})),
        "",
        "Processing",
        f"  Makeup gain       {number(gain.get('makeupDb'), 2)} dB after {gain.get('iterations', 0)} loudness passes",
        f"  Target reached    {'yes' if gain.get('targetReached') else 'no'}",
        f"  Limiter           up to {number(limiter.get('maxReductionDb'), 1)} dB, {number(limiter.get('averageReductionDb'), 2)} dB on average",
        f"  Soft clipper      {'on' if limiter.get('softClip') else 'off'}",
    ]
    if match.get("measured"):
        lines.append(
            f"  Tonal match       {number(match.get('beforeDb'), 2)} dB -> {number(match.get('afterDb'), 2)} dB RMS difference to the reference"
        )
    lines += ["", "EQ"]
    lines += band_lines("Mid" if report.get("width", {}).get("midSide") else "Stereo", report.get("mid", {}).get("bands", []))
    if report.get("width", {}).get("midSide"):
        lines += band_lines("Side", report.get("side", {}).get("bands", []))
        lines.append(f"  Side gain {float(report.get('width', {}).get('sideGainDb', 0.0)):+.2f} dB")
    if report.get("monoBassHz"):
        lines.append(f"  Mono bass below {number(report.get('monoBassHz'), 0)} Hz")
    notes = report.get("notes") or []
    warnings = report.get("warnings") or []
    if notes:
        lines += ["", "Notes"] + [f"  {text}" for text in notes]
    if warnings:
        lines += ["", "Warnings"] + [f"  {text}" for text in warnings]
    return "\n".join(lines) + "\n"
