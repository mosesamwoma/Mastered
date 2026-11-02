# Mastered

Reference-based audio mastering engine written in C++23. It analyses one or more reference tracks, fits a parametric EQ to your track so its tonal balance matches, optionally matches dynamics and stereo width, then drives the result to a loudness target with a look-ahead true-peak limiter and writes a dithered WAV file.

Everything runs as a streaming pipeline in fixed-size blocks, so memory use stays flat regardless of file length.

## Features

- BS.1770-4 / EBU R128 loudness: K-weighting, channel weighting, absolute and relative gating, loudness range (LRA), true-peak metering
- Reference matching on 1/6-octave bands with power-domain averaging, silence gating and robust level matching
- EQ placement by optimisation: peak, low-shelf and high-shelf biquads are fitted against the target curve using their real frequency responses
- Mid/side analysis and EQ, stereo width matching, correlation reporting
- Multi-reference averaging
- Dynamics matching: three-band compression whose ratios come from the measured level spread of each band in your track and in the reference, refined by measuring what the compressor actually achieves
- Optional soft clipper before the limiter
- Look-ahead true-peak limiter with a closed-loop check: the final output is measured with a more accurate 8x oversampled detector and the limiter ceiling is trimmed until the measured peak respects the ceiling
- Loudness loop: gain is computed after EQ and dynamics, then iterated until the measured output hits the target
- TPDF dither with optional first-order noise shaping for 16 and 24 bit output
- WAV input: 16/24/32-bit PCM, 32/64-bit float, WAVE_FORMAT_EXTENSIBLE, RF64/BW64 files over 4 GB, up to 8 channels, odd-sized chunks, streamed headers
- WAV output: 16/24-bit PCM, 32-bit float (format tag 3), extensible header above two channels, automatic RF64 beyond 4 GB
- JSON report, ReaEQ band list and Equalizer APO config (including mid/side)
- Platform presets (Spotify, YouTube, Apple Music, Amazon Music, Tidal, Deezer, SoundCloud, podcast, EBU R128, ATSC A/85, loud master) shared by the command line and the web page
- Mono bass: the side signal is high-passed (fourth order) so low frequencies fold to mono cleanly
- Extra measurements: sample peak, momentary and short-term maximum, peak-to-loudness ratio, DC offset, clipped samples, mono fold-down loss and low-band correlation
- Limiter gain-reduction report (maximum, average, share of time)
- Reference check: after rendering, the master's average spectrum is measured again and compared with the reference, so the report shows how much closer the tone got
- Waveform data for the input and the master, an average-spectrum comparison, and a streaming-service table in the web page
- C API shared library
- Web app (FastAPI + a single static page): optional sign-in page, MP3, FLAC, M4A, OGG and AAC upload, a before/after player, per-visitor rate limits, persistent job storage, Docker image and Render blueprint

## Build

```
./build.sh
```

Requires CMake 3.20+ and a compiler with C++23 support (built and tested with GCC 12 and GCC 13; Clang 16+ and MSVC 2022 should work but were not tried). Options:

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

By default the mastered file is written next to the input as `<input>_mastered.wav`, with a JSON report next to it. The command line reads WAV files only; the web app converts other formats first.

| Option | Default | Meaning |
| --- | --- | --- |
| `--reference PATH` | | Additional reference track, repeatable |
| `--output PATH` | `<input>_mastered.wav` | Mastered WAV path |
| `--json PATH` | output with `.json` | Report path |
| `--presets` | off | Write `.reaeq.txt` and `.apo.txt` EQ files |
| `--preset NAME` | | Target and ceiling for a service; `--target` and `--ceiling` still override it |
| `--list-presets` | | Print the presets as JSON and exit |
| `--mono-bass HZ` | off | Make everything below HZ mono, 20 to 500 (about -24 dB of side signal one octave below HZ) |
| `--target LUFS` | -14 | Loudness target, -60 to -1 |
| `--aggressive N` | 0.85 | EQ and dynamics strength, 0 to 1 |
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
| `--soft-clip` | | Always use the soft clipper before the limiter |
| `--no-auto-clip` | | Do not switch the soft clipper on automatically when the limiter would have to remove more than about 3 dB |
| `--bass-limiter` | | Experimental: limit the band below 140 Hz separately (off by default, see Notes) |
| `--no-auto-mono-bass` | | Do not make the bass mono automatically when the low end is out of phase (correlation below 0.3) |
| `--dual-mono` | | Measure single-channel files as dual mono (+3 LU) |
| `--measure PATH` | | Measure a WAV file with the engine's meter, print JSON and exit |
| `--fast-peak` | | Use the 4x peak detector and skip the closed-loop ceiling check (about a third faster) |
| `--lookahead MS` | 5 | Limiter look-ahead, 0.5 to 20 |
| `--release MS` | 80 | Limiter release, 5 to 1000 |
| `--fft N` | 8192 | FFT size, power of two 1024 to 65536 |
| `--passes N` | 12 | Maximum loudness passes, 1 to 30 |
| `--tolerance LU` | 0.02 | Loudness tolerance, 0.005 to 2 |
| `--verbose` | | Print EQ bands and stereo/dynamics details |

Exit codes: 0 success, 1 error, 2 usage.

### EQ export files

`--presets` writes two text files next to the output.

- `.apo.txt` is an Equalizer APO configuration. For stereo tracks that use mid/side processing it encodes the mid and side bands with `Copy:` and `Channel:` commands, written to match the Equalizer APO documentation. The sequence was checked mathematically against the engine's own mid/side processing, but it has not been run inside Equalizer APO itself.
- `.reaeq.txt` lists each band with type, frequency, gain, bandwidth in octaves and Q so the values can be entered in ReaEQ. A real ReaEQ preset file is not generated: its preset format is an undocumented binary state, and a guessed file could be silently wrong.

## Web app

A small web page lets you upload a reference and your track, watch progress, see the measurements and EQ curves, listen to the original and the master, and download the mastered file as WAV, FLAC, AIFF, MP3, OGG Vorbis, Opus or AAC. Several tracks can be mastered one after another with the same reference and settings.

```
Mastered/
  Dockerfile        builds the engine, then the Python image with ffmpeg
  render.yaml       Render blueprint (free plan)
  web/
    app/            FastAPI service
    static/         index.html, login.html, style.css, app.js, login.js
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

Install `ffmpeg` and `ffprobe` to enable MP3, FLAC, M4A, OGG and AAC upload and the listening player. Without them the app still works for WAV files and the player is hidden.

With Docker:

```
docker build -t mastered .
docker run --rm -p 10000:10000 mastered
```

### Deploy on Render (free)

1. Push this folder to a GitHub repository.
2. In Render choose New, then Blueprint, and select the repository. Render reads `render.yaml` and creates a free Docker web service. Alternatively choose New, then Web Service, pick the repository, set the runtime to Docker and the plan to Free.
3. Wait for the build to finish and open the service URL.

The free plan has a small CPU share and sleeps after 15 minutes without visitors, so the first visit can take about a minute and a 3-minute song can take several minutes to master. Conversion and preview creation add to that. Disk is temporary: results and the job database are removed on every restart. The page shows a waking-up message while the service starts and keeps polling your job if you reload.

### Sign-in

The web app can require a username and password. It needs no database: accounts come from an environment variable and sessions are signed cookies.

Sign-in is off until `AUTH_USERS` is set. When it is on, every page and API call needs a valid session except the login page and its files, the sign-in and sign-out calls, `/api/health` and `/api/session`; visitors without a session are sent to `/login`.

1. Create a password hash for each user (passwords must be at least 10 characters):

   ```
   cd web
   python3 -m app.passwd alice
   ```

   With Docker: `docker run --rm -it mastered python -m app.passwd alice`. Each run prints one `name:hash` entry.
2. Join the entries with commas or new lines and set them as `AUTH_USERS`. Hashes contain `$`, so put them in single quotes in a shell and enter them as they are in a hosting dashboard.
3. Set `SESSION_SECRET` to a random string of at least 32 characters. For example: `python3 -c "import secrets; print(secrets.token_urlsafe(48))"`. The secret signs the sessions, so every server process that shares users must use the same one.

```
AUTH_USERS='alice:scrypt$16384$8$1$...,bob:scrypt$16384$8$1$...'
SESSION_SECRET='...'
```

On Render the blueprint already creates a random `SESSION_SECRET`. Add `AUTH_USERS` yourself under the service's Environment settings. Without it the site stays public.

How it works:

- Passwords are stored as salted scrypt hashes. Login checks take the same time for unknown users and wrong passwords, and the same message is returned for both.
- A session is a signed cookie (HttpOnly, SameSite=Lax, Secure over HTTPS) that lasts `SESSION_HOURS` (12 by default). Nothing is stored on the server.
- Failed logins are limited per visitor (`LOGIN_ATTEMPTS` per 15 minutes, 8 by default). The count is kept in each process.
- Requests that change data are rejected when their `Origin` header does not match the site, which blocks cross-site requests.
- Each job belongs to the user who created it. Other users, and anyone not signed in, get a not-found response for its status, download, preview and delete.
- Job limits (one job at a time, jobs per hour) are counted per user instead of per IP address.

Limits to know about:

- Sessions are not stored, so Sign out only removes the cookie from your browser. A copy of the cookie stays valid until it expires. To cut off a user immediately, remove them from `AUTH_USERS` or change their hash; either one ends all of that user's sessions. Changing `SESSION_SECRET` ends everyone's.
- There is no sign-up, password reset or account page; you manage users by editing the environment variable.
- Use HTTPS in production. Render provides it. Set `COOKIE_SECURE=always` if your proxy does not send `X-Forwarded-Proto`.

### Formats and previews

- WAV files that the engine can read are used as they are.
- Everything else is inspected with `ffprobe` and converted with `ffmpeg` as the first step of the job. Only audio containers on an allow-list are accepted (MP3, FLAC, Ogg, MP4/M4A, AAC, WAV/AIFF, Matroska/WebM), and `ffmpeg` is run with a file-only protocol whitelist, so playlists or concat files that point elsewhere are rejected.
- Lossless sources of 16 bits or less are decoded to 16-bit, everything else to 24-bit. The default output keeps that depth.
- When the job finishes, 160 kbps MP3 (or AAC) previews of the original and the master are made for the player. For tracks up to four minutes (two minutes on touch screens) both previews are decoded in the browser and played on one audio clock, so switching between them is sample-accurate; longer tracks use two media elements that are kept in step. Match loudness lowers whichever version is louder so the comparison is fair.
- Converted inputs and temporary files are deleted as soon as they are no longer needed.

### Rate limits

Limits are applied per visitor, identified by IP address (IPv6 addresses are grouped by /64).

- Only one job per visitor at a time (`PENDING_PER_CLIENT`) and a maximum number of jobs per hour (`JOBS_PER_HOUR_PER_CLIENT`).
- A cap on API requests per minute (`API_REQUESTS_PER_MINUTE`) and on upload attempts per hour (`UPLOAD_ATTEMPTS_PER_HOUR`).
- A global cap on waiting plus running jobs (`MAX_PENDING_JOBS`).
- Rejected requests get HTTP 429 with a `Retry-After` header.

Behind a proxy the visitor's address must come from a header. Set `CLIENT_IP_HEADER` to the header your host sets (`cf-connecting-ip` on Render, which sits behind Cloudflare; the Docker image and `render.yaml` already do this). When it is empty the header is ignored and the connection address is used, which cannot be spoofed but sees every visitor as the proxy. Only set it when the header is overwritten by infrastructure you trust.

### Persistent job storage and several servers

Job state lives in a SQLite database (`jobs.db` inside `WORK_DIR`) in write-ahead mode, not in memory.

- Jobs survive a restart as long as `WORK_DIR` does. On Render's free plan the disk is temporary, so everything is lost on restart; attach a persistent disk and set `WORK_DIR` to it to keep results.
- A job that was running when a server crashed is marked failed after its heartbeat goes stale (about a minute).
- Several server processes can share the same `WORK_DIR` on one machine or on a shared volume. Any process can accept uploads, any process can run jobs, and any process can answer status, download and cancel requests. `MAX_CONCURRENT_JOBS` is a cap across all of them.
- Servers on separate machines need a shared filesystem for the job folders. SQLite should not be placed on a network filesystem that does not support file locking. A setup with separate hosts would need an external database and object storage, which are not included.
- API rate limits per minute are counted inside each process; job limits are enforced from the shared database.

### Settings

All settings are environment variables with safe defaults.

| Variable | Default | Meaning |
| --- | --- | --- |
| `MASTERED_CLI` | auto | Path to `mastered_cli` |
| `FFMPEG_PATH`, `FFPROBE_PATH` | found on `PATH` | Paths to ffmpeg tools |
| `WORK_DIR` | system temp dir | Uploads, results and `jobs.db` |
| `MAX_FILE_MB` | 60 | Largest single upload |
| `MAX_DURATION_SEC` | 420 | Longest audio file accepted |
| `MAX_PENDING_JOBS` | 2 | Jobs uploading, waiting or running before new ones get HTTP 429 |
| `MAX_CONCURRENT_JOBS` | 1 | Jobs running at the same time across all processes |
| `PENDING_PER_CLIENT` | 1 | Unfinished jobs per visitor |
| `JOBS_PER_HOUR_PER_CLIENT` | 6 | Jobs per visitor per hour |
| `API_REQUESTS_PER_MINUTE` | 240 | API requests per visitor per minute, per process |
| `UPLOAD_ATTEMPTS_PER_HOUR` | 30 | Upload attempts per visitor per hour, per process |
| `CLIENT_IP_HEADER` | empty (Docker: `cf-connecting-ip`) | Header that carries the visitor address |
| `AUTH_USERS` | empty | `name:hash` entries that enable sign-in; see Sign-in |
| `SESSION_SECRET` | empty | Signing key for sessions, at least 32 characters, required with `AUTH_USERS` |
| `SESSION_HOURS` | 12 | How long a sign-in lasts |
| `COOKIE_SECURE` | auto | `auto` marks the cookie Secure over HTTPS, `always` or `never` force it |
| `LOGIN_ATTEMPTS` | 8 | Failed sign-ins per visitor per 15 minutes, per process |
| `JOB_TTL_MINUTES` | 20 | How long finished jobs stay downloadable |
| `JOB_TIMEOUT_SEC` | 1800 | A mastering run longer than this is stopped |
| `CONVERT_TIMEOUT_SEC` | 300 | Time allowed to convert one uploaded file |
| `PORT` | 10000 | Port used by the Docker image |

### How it works

- Jobs are claimed from the database by a background worker in each process; the engine and ffmpeg run as separate processes in their own process groups, so cancelling or timing out a job kills them cleanly, including from another process.
- Progress lines printed by the engine are parsed and exposed through the status endpoint.
- Uploads are streamed to disk with size limits enforced on the whole request and on each file, and each file is checked before the job is queued.
- Files are stored under random job ids with fixed names, never under user-supplied names.
- Validation errors never echo the submitted values, so passwords do not appear in responses.
- The page sets a strict content security policy and builds all dynamic content with `textContent`.

### API

| Method and path | Purpose |
| --- | --- |
| `GET /api/health` | Health check |
| `POST /api/login` | JSON `username` and `password`; sets the session cookie |
| `POST /api/logout` | Clears the session cookie |
| `GET /api/session` | Whether sign-in is required and who is signed in |
| `GET /api/config` | Limits, accepted formats, whether previews are available and whether sign-in is required |
| `POST /api/jobs` | Multipart upload with `reference`, `track` and optional `target_lufs`, `aggressiveness`, `ceiling_db`, `bands`, `bit_depth`, `precision` (`fast`, `normal`, `high`), `dynamics`, `soft_clip`, `auto_clip`, `auto_mono_bass`, `dual_mono`, `bass_limiter`, `dither`, `noise_shaping`, `mid_side`, `match_width`, `mono_bass_hz`; returns 202 with the job id |
| `GET /api/jobs/{id}` | Status (`queued`, `running`, `done`, `failed`), progress, queue position, and when done the report, download link and preview links |
| `GET /api/jobs/{id}/download` | Mastered WAV |
| `POST /api/jobs/{id}/export` | JSON body `{"format": "mp3", "quality": "320"}`; encodes and checks the file, returns its name, URL, size and measured loudness and true peak. One export runs at a time, at most 12 files per job |
| `GET /api/jobs/{id}/export/{key}` | A file made by the export call |
| `GET /api/jobs/{id}/report?kind=txt\|json` | The mastering report |
| `GET /api/jobs/{id}/eq/{reaeq\|apo}` | EQ settings for ReaEQ or Equalizer APO |
| `GET /api/jobs/{id}/audio/original` and `/mastered` | Preview audio with range support |
| `DELETE /api/jobs/{id}` | Cancel and delete a job |

### Downloads in other formats

The mastered WAV is the master; every other format is made from it with ffmpeg when you ask for it, and cached with the job until it expires. Which formats are offered depends on the encoders in the server's ffmpeg: FLAC and AIFF (lossless), MP3 (320, V0, 192, 128), OGG Vorbis (q9 to q3), Opus (192 to 96 kbps) and AAC in M4A (256 to 128 kbps).

Lossy encoders move the true peak: an MP3, Opus or AAC file can come out above the ceiling that the master respected. Each lossy file is therefore decoded again and measured with the engine's own meter (`mastered_cli --measure`). If its true peak is above the ceiling, the level is lowered by the overshoot and the file is encoded again (up to five encodes). The result shows the loudness and peak of the file you actually download, and says so when the level had to be lowered or the ceiling could not be met.

Also available for each result: a text or JSON report and, for the EQ, ReaEQ and Equalizer APO settings.

## Processing chain

```
reader -> [mid/side split] -> EQ -> [side gain] -> [mid/side merge]
       -> multiband compressor (optional) -> temporary 32-bit float file
temporary file -> makeup gain -> soft clipper (optional, automatic for heavy limiting)
       -> true-peak limiter -> writer -> meter (on the written file)
```

1. Every file is scanned once: averaged mid and side power spectra, integrated loudness, LRA, true peak, stereo correlation and side-to-mid ratio, and per-band level statistics.
2. Band levels are level-matched using the median difference across 63 Hz to 12.5 kHz, scaled by the aggressiveness, and clamped to -12 dB for cuts, +9 dB for boosts and +6 dB for boosts below 160 Hz. Big low-frequency boosts raise the kick peaks before the limiter and make it work harder, which is what softens a beat.
3. A greedy fitter adds one band at a time, tries peak, low-shelf and high-shelf for each, and refines all bands jointly.
4. With dynamics matching, each band's compression ratio is derived from how much more spread out its 200 ms levels are in your track than in the reference. The compressor is then run on the track and the ratios are corrected up to three times until the measured spread matches the target.
5. The EQ, mid/side and dynamics stages do not depend on the makeup gain, so their output is written once to a temporary 32-bit float file next to the output (removed when the run ends, also on errors). The first measurement of that signal gives the starting gain and the peak-to-ceiling distance. Further passes read the temporary file, apply gain, clipper and limiter and adjust the makeup gain until the integrated loudness is within tolerance (0.02 LU by default, at most 12 passes per search).
6. If the makeup gain would force the limiter to remove more than about 3 dB, the soft clipper is switched on automatically (`--no-auto-clip` turns that off), so the limiter does not have to pull the whole mix down on every kick.
7. The limiter has a fast and a slow release follower and uses the smaller gain of the two: a single transient is released quickly, sustained limiting recovers over the full release time. The default look-ahead is 5 ms.
8. The finished file is measured again after rounding to the output bit depth. If its true peak is above the ceiling minus 0.02 dB, the limiter ceiling is lowered by the excess; if the limiter is working and the peak is more than 0.08 dB lower, the ceiling is raised again (at most 0.6 dB, twice). The loudness is re-converged after each change and the file is written again, up to five times. The reported loudness and true peak are those of the delivered file.

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

Options: `target_lufs`, `aggressiveness`, `ceiling_db`, `max_bands`, `max_side_bands`, `smoothing`, `perceptual_weighting`, `auto_gain`, `mid_side`, `match_width`, `dynamics`, `dynamics_match`, `dynamics_ratio`, `dynamics_threshold_offset_db`, `soft_clip`, `soft_clip_knee_db`, `accurate_peak`, `lookahead_ms`, `release_ms`, `bit_depth`, `dither`, `noise_shaping`, `tolerance_lu`, `max_iterations`, `fft_size`, `auto_clip`, `auto_mono_bass`, `dual_mono`, `bass_limiter`.

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

The JSON report contains input, reference and output measurements, the makeup gain and number of loudness passes, the fitted mid and side bands with their target and response curves on a 1/12-octave grid, the stereo width adjustment, the dynamics settings with the measured per-band level spread of your track and the reference, and any warnings. Version 2.2 adds `limiter` (gain reduction), `spectrum` (1/3-octave shapes of the input, the reference and the master, plus a `match` block with the RMS difference to the reference before and after), `waveform` (800 peak and RMS columns for the input and the master), `monoBassHz`, and the extra measurements listed above.

## Notes and limits

- The command line reads and writes WAV only. Files over 4 GB are read and written as RF64.
- Sample rates from 8 kHz to 192 kHz, 1 to 8 channels. Files must be at least one second long.
- Reference and input may have different sample rates; the analysis range is limited to the lower Nyquist.
- The tonal-match figure compares average spectra in 1/3-octave bands over 63 Hz to 12.5 kHz. It shows how close the tone is to the reference, not whether the result sounds better.
- Bands where the input has almost no content (more than 50 dB below its average level, for example above the cut-off of a lossy file) are not boosted.
- Platform targets are typical values and change over time; services also differ in whether they turn quiet tracks up.
- Mid/side EQ is used for stereo files only. For other channel counts the mid EQ is applied to every channel.
- True peak: the limiter detects with a 4x, 24-tap-per-phase oversampler. The reported output peak and the closed-loop check use an 8x, 64-tap-per-phase detector. Compared with a much longer reference filter it read within 0.04 dB on typical material and up to about 0.2 dB low on very heavily limited material. Input and reference peaks in the report use the 4x detector.
- `--fast-peak` (used by the web page's Fast precision) skips the accurate measurement and the closed-loop check, so the peak is only guaranteed to the 4x detector's accuracy.
- If the ceiling prevents the loudness target from being reached, the closest result is written and a warning is reported.
- Dynamics matching moves each band's level spread toward the reference; it is an approximation and a band is left alone when it already matches.
- `--bitdepth 32` writes IEEE float. 32-bit integer PCM input is accepted and converted.
- Bundled FFmpeg-based conversion is only as capable as the FFmpeg build installed on the server.
- Meters and loudness: the integrated loudness follows ITU-R BS.1770-4 / EBU R128 with absolute and relative gating, and agrees with ffmpeg's `ebur128` filter to within about 0.05 LU. Other meters can differ in what they show: a single-channel file is measured as one channel (use `--dual-mono` for meters that count it twice), short-term and momentary readings are not the integrated value, and true-peak meters with 4x oversampling (including ffmpeg's) read a little lower than this engine's 8x detector.
- Disk use: while a file is mastered, a temporary 32-bit float copy of the processed signal (4 bytes per sample per channel) is kept next to the output; a 5 minute stereo 44.1 kHz track needs about 106 MB for it, and it is removed when the run ends.
- `--bass-limiter` is experimental and off by default. On a synthetic kick-and-bass test it moved loudness from the bass to the highs and did not reduce modulation of the highs, so the default chain relies on the automatic soft clipper and the two-follower release instead. Judge it by ear on your own material before using it.
- The soft clipper that is switched on for heavy limiting trades limiter pumping for a little clipping distortion on the tallest peaks. If you prefer the limiter alone, use `--no-auto-clip` (the web page has the same switch under Advanced).
- Lossy exports from the web app are checked after encoding, but a player's own decoder and sample-rate conversion can still add a little peak; leave some room in the ceiling if a platform is strict.
