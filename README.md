# Mastered

Reference-based audio mastering engine written in C++17. It analyses one or more reference tracks, fits a parametric EQ to your track so its tonal balance matches, optionally matches dynamics and stereo width, then drives the result to a loudness target with a look-ahead true-peak limiter and writes a dithered WAV file.

Everything runs as a streaming pipeline in fixed-size blocks, so memory use stays flat regardless of file length.

## Features

- BS.1770-4 / EBU R128 loudness: K-weighting, channel weighting, absolute and relative gating, loudness range (LRA), true-peak metering
- Reference matching on 1/6-octave bands with power-domain averaging, silence gating and robust level matching
- EQ placement by optimisation: peak, low-shelf and high-shelf biquads are fitted against the target curve using their real frequency responses
- Mid/side analysis and EQ, stereo width matching, correlation reporting
- Multi-reference averaging
- Optional multiband compression (3 bands, Linkwitz-Riley crossovers) matched to the reference crest factor and loudness range
- Optional soft clipper before the limiter
- Look-ahead true-peak limiter with 4x oversampled detection
- Loudness loop: gain is computed after EQ and dynamics, then iterated until the measured output hits the target
- TPDF dither with optional first-order noise shaping for 16 and 24 bit output
- WAV input: 16/24/32-bit PCM, 32/64-bit float, WAVE_FORMAT_EXTENSIBLE, up to 8 channels, odd-sized chunks, streamed headers
- WAV output: 16/24-bit PCM, 32-bit float (format tag 3), extensible header above two channels
- JSON report, ReaEQ and Equalizer APO band lists
- C API shared library
- Web app (FastAPI + a single static page) with a Docker image and a Render blueprint

## Build

```
./build.sh
```

Requires CMake 3.16+ and a C++17 compiler. Options:

| Option | Default | Meaning |
| --- | --- | --- |
| `MASTERED_NATIVE` | OFF | Optimise for the build machine |
| `MASTERED_SANITIZE` | OFF | Address and undefined behaviour sanitizers |
| `MASTERED_BUILD_SHARED` | ON | Build the C API library |

Example: `./build.sh -DMASTERED_SANITIZE=ON`

Outputs: `build/mastered_cli`, `build/libmastered.so` (or the platform equivalent), `build/libmastered_engine.a`.

## Usage

```
./build/mastered_cli <reference.wav> <input.wav> [options]
```

By default the mastered file is written next to the input as `<input>_mastered.wav`, with a JSON report next to it.

| Option | Default | Meaning |
| --- | --- | --- |
| `--reference PATH` | | Additional reference track, repeatable |
| `--output PATH` | `<input>_mastered.wav` | Mastered WAV path |
| `--json PATH` | output with `.json` | Report path |
| `--presets` | off | Write `.reaeq.txt` and `.apo.txt` band lists |
| `--target LUFS` | -14 | Loudness target, -60 to -1 |
| `--aggressive N` | 0.85 | EQ strength, 0 to 1 |
| `--ceiling DB` | -1 | True-peak ceiling, -12 to 0 |
| `--bands N` | 8 | Maximum mid EQ bands, 0 to 32 |
| `--side-bands N` | 4 | Maximum side EQ bands, 0 to 32 |
| `--bitdepth N` | source | 16, 24 or 32 (32 writes float) |
| `--no-dither` | | Disable TPDF dither |
| `--noise-shaping` | | First-order noise shaping |
| `--no-mid-side` | | Disable mid/side EQ |
| `--no-width` | | Disable stereo width matching |
| `--no-auto-gain` | | Skip the loudness loop |
| `--no-smoothing` | | 1/6-octave analysis instead of 1/3-octave |
| `--no-perceptual` | | Disable perceptual error weighting in the EQ fit |
| `--dynamics` | | Multiband compression matched to the reference |
| `--dynamics-ratio N` | | Fixed ratio, 1 to 20 (implies `--dynamics`) |
| `--soft-clip` | | Soft clipper before the limiter |
| `--lookahead MS` | 3 | Limiter look-ahead, 0.5 to 20 |
| `--release MS` | 80 | Limiter release, 5 to 1000 |
| `--fft N` | 8192 | FFT size, power of two 1024 to 65536 |
| `--passes N` | 10 | Maximum loudness passes, 1 to 30 |
| `--tolerance LU` | 0.1 | Loudness tolerance, 0.01 to 2 |
| `--verbose` | | Print EQ bands and stereo/dynamics details |

Exit codes: 0 success, 1 error, 2 usage.

## Web app

A small web page lets you upload a reference and your track, watch progress, see the measurements and EQ curves, and download the mastered WAV.

```
Mastered/
  Dockerfile        builds the engine, then the Python image
  render.yaml       Render blueprint (free plan)
  web/
    app/            FastAPI service
    static/         index.html, style.css, app.js
```

### Run it locally

```
./build.sh
cd web
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
uvicorn app.main:app --port 8000
```

Open http://localhost:8000. The service looks for the engine at `/usr/local/bin/mastered_cli` and then at `build/mastered_cli`; set `MASTERED_CLI` to use another path.

With Docker:

```
docker build -t mastered .
docker run --rm -p 10000:10000 mastered
```

With Docker Compose:

```
docker compose up --build
```

Open http://localhost:10000. Stop the service with `Ctrl+C`, or run
`docker compose down` from another terminal.

### Deploy on Render (free)

1. Push this folder to a GitHub repository.
2. In Render choose New, then Blueprint, and select the repository. Render reads `render.yaml` and creates a free Docker web service. Alternatively choose New, then Web Service, pick the repository, set the runtime to Docker and the plan to Free.
3. Wait for the build to finish and open the service URL.

The free plan has a small CPU share and sleeps after 15 minutes without visitors, so the first visit can take about a minute and a 3-minute song can take several minutes to master. Disk is temporary: results are removed after `JOB_TTL_MINUTES` and on every restart. The page shows a waking-up message while the service starts and keeps polling your job if you reload.

### Settings

All settings are environment variables with safe defaults.

| Variable | Default | Meaning |
| --- | --- | --- |
| `MASTERED_CLI` | auto | Path to `mastered_cli` |
| `WORK_DIR` | system temp dir | Where uploads and results live |
| `MAX_FILE_MB` | 60 | Largest single upload |
| `MAX_DURATION_SEC` | 420 | Longest audio file accepted |
| `MAX_PENDING_JOBS` | 2 | Jobs waiting or running before new ones get HTTP 429 |
| `JOB_TTL_MINUTES` | 20 | How long finished jobs stay downloadable |
| `JOB_TIMEOUT_SEC` | 1800 | A job running longer than this is stopped |
| `PORT` | 10000 | Port used by the Docker image |

### How it works

- One job runs at a time in a background queue; the engine is started as a separate process in its own process group, so cancelling or timing out a job kills it cleanly.
- Progress lines printed by the engine are parsed and exposed through the status endpoint.
- Uploads are streamed to disk with size limits enforced both on the whole request and on each file, and each file's WAV header is checked before the job is queued.
- Files are stored under random job ids with fixed names, never under user-supplied names.
- The page sets a strict content security policy and builds all dynamic content with `textContent`.
- Run a single worker process only. Job state is kept in memory.

### API

| Method and path | Purpose |
| --- | --- |
| `GET /api/health` | Health check |
| `GET /api/config` | Limits shown on the page |
| `POST /api/jobs` | Multipart upload with `reference`, `track` and optional `target_lufs`, `aggressiveness`, `ceiling_db`, `bands`, `bit_depth`, `precision` (`fast`, `normal`, `high`), `dynamics`, `soft_clip`, `dither`, `noise_shaping`, `mid_side`, `match_width`; returns 202 with the job id |
| `GET /api/jobs/{id}` | Status (`queued`, `running`, `done`, `failed`), progress, queue position, and the report when done |
| `GET /api/jobs/{id}/download` | Mastered WAV |
| `DELETE /api/jobs/{id}` | Cancel and delete a job |

## Processing chain

```
reader -> [mid/side split] -> EQ -> [side gain] -> [mid/side merge]
       -> multiband compressor (optional) -> makeup gain
       -> soft clipper (optional) -> true-peak limiter -> meter -> writer
```

1. Every file is scanned once: averaged mid and side power spectra, integrated loudness, LRA, true peak, stereo correlation and side-to-mid ratio, and three-band RMS levels.
2. Band levels are level-matched using the median difference across 63 Hz to 12.5 kHz, scaled by the aggressiveness, and clamped to +/-12 dB.
3. A greedy fitter adds one band at a time, tries peak, low-shelf and high-shelf for each, and refines all bands jointly.
4. The first loudness pass measures the post-EQ, post-dynamics signal. Further passes run the full chain including the limiter and adjust makeup gain until the integrated loudness is within tolerance (0.1 LU by default, at most 10 passes).
5. The final pass writes the file.

## C API

Header: `include/mastered_c.h`.

```c
mastered_engine* e = mastered_create();
mastered_set_option(e, "target_lufs", -12.0);
mastered_add_reference(e, "reference.wav");
if (mastered_run(e, "input.wav", "output.wav") != 0) {
    fprintf(stderr, "%s\n", mastered_last_error(e));
}
double lufs;
mastered_get_metric(e, "output_lufs", &lufs);
const char* json = mastered_result_json(e);
mastered_destroy(e);
```

Options: `target_lufs`, `aggressiveness`, `ceiling_db`, `max_bands`, `max_side_bands`, `smoothing`, `perceptual_weighting`, `auto_gain`, `mid_side`, `match_width`, `dynamics`, `dynamics_match`, `dynamics_ratio`, `dynamics_threshold_offset_db`, `soft_clip`, `soft_clip_knee_db`, `lookahead_ms`, `release_ms`, `bit_depth`, `dither`, `noise_shaping`, `tolerance_lu`, `max_iterations`, `fft_size`.

Metrics: `input_lufs`, `input_true_peak_db`, `input_lra`, `reference_lufs`, `output_lufs`, `output_true_peak_db`, `output_lra`, `output_correlation`, `makeup_gain_db`, `iterations`, `target_reached`, `mid_band_count`, `side_band_count`.

Return codes: 0 success, 1 invalid argument or no result, 2 unknown key, 3 processing error (see `mastered_last_error`). Returned strings stay valid until the next call on the same handle.

## C++ API

```cpp
mastered::MasteringConfig config;
config.targetLufs = -14.0;
mastered::MasteringEngine engine(config);
mastered::MasteringResult result = engine.run({"reference.wav"}, "input.wav", "output.wav");
std::string json = mastered::MasteringEngine::exportJSON(result);
```

`analyze` and `render` can be called separately. All failures throw `mastered::MasteredError`.

## Report

The JSON report contains input, reference and output measurements, the makeup gain and number of loudness passes, the fitted mid and side bands with their target and response curves on a 1/12-octave grid, the stereo width adjustment, the dynamics settings and any warnings.

## Notes and limits

- Input and output are WAV only. Output is limited to 4 GB.
- Sample rates from 8 kHz to 192 kHz, 1 to 8 channels. Files must be at least one second long.
- Reference and input may have different sample rates; the analysis range is limited to the lower Nyquist.
- Mid/side EQ is used for stereo files only. For other channel counts the mid EQ is applied to every channel. The Equalizer APO export contains the mid bands only.
- True peak is measured and limited with a 4x oversampling detector (96-tap polyphase) and a 0.3 dB safety margin below the ceiling. With extreme loudness targets on very dynamic material, reconstruction by an ideal band-limited filter can still read slightly above the ceiling.
- `--bitdepth 32` writes IEEE float. 32-bit integer PCM input is accepted and converted.
- If the target cannot be reached within tolerance, the closest result is written and a warning is reported.