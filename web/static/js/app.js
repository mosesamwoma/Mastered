import { delay, describeError, ignore, parseJson, readStorage, redirectToLogin, request, writeStorage } from './api.js';
import { buildBandTable, buildChart, buildSpectrumChart } from './charts.js';
import { conversionNotes, describeChanges } from './summary.js';
import { cell, describeWait, formatNumber, formatSize, formatTime, signed } from './format.js';

const POLL_INTERVAL_MS = 1500;
const CONFIG_RETRY_MS = 3000;
const CONFIG_MAX_ATTEMPTS = 40;
const MAX_POLL_FAILURES = 6;
const SETTINGS_KEY = 'masteredSettings';
const MAX_SAVED_SETTINGS = 12;
const MAX_BATCH_TRACKS = 5;
const SEEK_STEP_SECONDS = 5;
const PREVIEW_LOAD_TIMEOUT_MS = 30000;
const BUFFER_PLAYER_MAX_SECONDS = 240;
const BUFFER_PLAYER_MAX_SECONDS_TOUCH = 120;
const ETA_MIN_ELAPSED_MS = 12000;
const ETA_MIN_PROGRESS = 8;
const TONE_REGIONS = [
  ['Bass (below 250 Hz)', 0, 250],
  ['Low mids (250 Hz to 2 kHz)', 250, 2000],
  ['Highs (above 2 kHz)', 2000, Infinity],
];

const STYLES = {
  gentle: { strength: 60, dynamics: false, softClip: false, note: 'A light touch: the tone follows the reference only partly and dynamics are left alone.' },
  balanced: { strength: 85, dynamics: false, softClip: false, note: 'The default: tone follows the reference closely, dynamics are left alone.' },
  transparent: { strength: 70, dynamics: false, softClip: false, transparent: true, note: 'Keeps the sound as it is: small EQ moves, no compression and no change to the stereo width.' },
  punchy: { strength: 90, dynamics: true, softClip: true, note: 'Denser and louder sounding: also matches dynamics and rounds peaks before the limiter.' },
};

const PROGRESS_STEPS = ['Upload', 'Analyse', 'Match tone', 'Master', 'Preview'];

const byId = (id) => document.getElementById(id);
const form = byId('job-form');
const referenceInput = byId('reference');
const trackInput = byId('track');
const dropZones = [
  [byId('drop-reference'), referenceInput, byId('reference-name'), 'Drop a finished track here or click to choose'],
  [byId('drop-track'), trackInput, byId('track-name'), 'Drop your mix here or click to choose'],
];
const readyHint = byId('ready-hint');
const targetInput = byId('target');
const ceilingInput = byId('ceiling');
const presetChips = byId('preset-chips');
const presetNote = byId('preset-note');
const styleButtons = Array.from(document.querySelectorAll('[data-style]'));
const styleNote = byId('style-note');
const aggressivenessInput = byId('aggressiveness');
const aggressivenessValue = byId('aggressiveness-value');
const bandsInput = byId('bands');
const bitDepthSelect = byId('bit-depth');
const precisionSelect = byId('precision');
const monoBassCheck = byId('mono-bass');
const monoBassHz = byId('mono-bass-hz');
const flagInputs = [
  ['mid_side', byId('mid-side')],
  ['match_width', byId('match-width')],
  ['dynamics', byId('dynamics')],
  ['soft_clip', byId('soft-clip')],
  ['dither', byId('dither')],
  ['noise_shaping', byId('noise-shaping')],
  ['auto_clip', byId('auto-clip')],
  ['auto_mono_bass', byId('auto-mono-bass')],
  ['dual_mono', byId('dual-mono')],
  ['bass_limiter', byId('bass-limiter')],
  ['transparent', byId('transparent')],
];
const planSummary = byId('plan-summary');
const savedSelect = byId('saved-select');
const savedName = byId('saved-name');
const savedSave = byId('saved-save');
const savedDelete = byId('saved-delete');
const savedNote = byId('saved-note');
const submitButton = byId('submit');
const formMessage = byId('form-message');
const serverStatus = byId('server-status');
const limitsText = byId('limits');
const progressPanel = byId('progress-panel');
const progressTitle = byId('progress-title');
const progressSteps = byId('progress-steps');
const progressBar = byId('progress-bar');
const progressMessage = byId('progress-message');
const progressEta = byId('progress-eta');
const batchStatus = byId('batch-status');
const cancelButton = byId('cancel');
const errorPanel = byId('error-panel');
const errorMessage = byId('error-message');
const retryButton = byId('retry');
const resultPanel = byId('result-panel');
const batchBar = byId('batch-bar');
const batchList = byId('batch-list');
const downloadLink = byId('download');
const heroSummary = byId('hero-summary');
const statsBox = byId('stats');
const notesList = byId('notes');
const warningsList = byId('warnings');
const exportFormat = byId('export-format');
const exportQuality = byId('export-quality');
const exportDescription = byId('export-description');
const exportButton = byId('export-button');
const export48k = byId('export-48k');
const exportTags = byId('export-tags');
const changesList = byId('changes');
const exportStatus = byId('export-status');
const exportList = byId('export-list');
const reportTxt = byId('report-txt');
const reportJson = byId('report-json');
const eqReaeq = byId('eq-reaeq');
const eqApo = byId('eq-apo');
const measurementsBody = byId('measurements');
const platformsBody = byId('platforms');
const gainSummary = byId('gain-summary');
const limiterSummary = byId('limiter-summary');
const spectrumBox = byId('spectrum');
const toneWrap = byId('tone-table-wrap');
const toneBody = byId('tone-regions');
const chartsContainer = byId('charts');
const againButton = byId('again');
const accountBox = byId('account');
const accountName = byId('account-name');
const logoutButton = byId('logout');
const formatsHint = byId('formats-hint');
const playerSection = byId('player');
const playerMode = byId('player-mode');
const playerMedia = byId('player-media');
const waveCanvas = byId('wave');
const playButton = byId('play');
const seekInput = byId('seek');
const timeLabel = byId('time');
const hearOriginal = byId('hear-original');
const hearMastered = byId('hear-mastered');
const matchLevel = byId('match-level');
const playerNote = byId('player-note');
const panels = [form, progressPanel, errorPanel, resultPanel];

let limits = null;
let jobId = null;
let activeUpload = null;
let busy = false;
let cancelRequested = false;
let results = [];
let shownIndex = -1;
let player = null;
let playerToken = 0;
let waveData = null;
let frameRequest = null;
let exportBusy = false;
let progressStartedAt = 0;

function show(panel) {
  panels.forEach((item) => {
    item.hidden = item !== panel;
  });
}

function setStatus(text) {
  serverStatus.textContent = text;
  serverStatus.hidden = text === '';
}

function setFormMessage(text) {
  formMessage.textContent = text;
  formMessage.hidden = text === '';
}

function stepIndexFor(message, percent) {
  const text = String(message || '');
  if (/preparing the preview/i.test(text)) {
    return 4;
  }
  if (/preparing the tone|measuring|matching loudness|writing|adjusting|checking/i.test(text)) {
    return 3;
  }
  if (/fitting|refining|analysis complete/i.test(text)) {
    return 2;
  }
  if (/analysing|converting/i.test(text)) {
    return 1;
  }
  return percent !== null && percent > 0 ? 1 : 0;
}

function paintSteps(index) {
  PROGRESS_STEPS.forEach((name, position) => {
    let item = progressSteps.children[position];
    if (!item) {
      item = document.createElement('li');
      item.textContent = name;
      progressSteps.appendChild(item);
    }
    item.className = position < index ? 'done' : position === index ? 'current' : '';
  });
}

function updateEta(value) {
  if (value === null || progressStartedAt === 0) {
    progressEta.textContent = '';
    return;
  }
  const elapsed = Date.now() - progressStartedAt;
  if (elapsed < ETA_MIN_ELAPSED_MS || value < ETA_MIN_PROGRESS || value >= 99) {
    progressEta.textContent = '';
    return;
  }
  const remaining = (elapsed / 1000) * ((100 - value) / value);
  progressEta.textContent = 'Roughly ' + describeWait(remaining) + ' left for this track.';
}

function setProgress(value, title, message, step) {
  if (value === null) {
    progressBar.removeAttribute('value');
  } else {
    progressBar.value = value;
  }
  progressTitle.textContent = title;
  progressMessage.textContent = message;
  paintSteps(typeof step === 'number' ? step : stepIndexFor(message, value));
  updateEta(typeof step === 'number' && step === 0 ? null : value);
}

async function loadConfig() {
  for (let attempt = 1; attempt <= CONFIG_MAX_ATTEMPTS; attempt += 1) {
    const response = await request('/api/config', { cache: 'no-store' });
    if (response !== null && response.ok) {
      limits = await response.json();
      return true;
    }
    if (response !== null && response.status === 401) {
      return false;
    }
    setStatus('The server is waking up. This can take up to a minute…');
    await delay(CONFIG_RETRY_MS);
  }
  return false;
}

function allowedFormats() {
  return limits && Array.isArray(limits.formats) && limits.formats.length > 0 ? limits.formats : ['.wav'];
}

function hasAllowedExtension(name) {
  const lower = name.toLowerCase();
  return allowedFormats().some((extension) => lower.endsWith(extension));
}

function refreshDropZone(entry) {
  const [zone, input, nameNode, placeholder] = entry;
  const files = input.files ? Array.from(input.files) : [];
  if (files.length === 0) {
    nameNode.textContent = placeholder;
  } else if (files.length === 1) {
    nameNode.textContent = files[0].name + ' (' + formatSize(files[0].size) + ')';
  } else {
    nameNode.textContent = files.length + ' tracks: ' + files.map((file) => file.name).join(', ');
  }
  zone.classList.toggle('filled', files.length > 0);
  refreshReadyHint();
  updatePlan();
}

function setupDropZones() {
  dropZones.forEach((entry) => {
    const [zone, input] = entry;
    input.addEventListener('change', () => refreshDropZone(entry));
    ['dragenter', 'dragover'].forEach((name) =>
      zone.addEventListener(name, (event) => {
        event.preventDefault();
        zone.classList.add('drag');
      })
    );
    ['dragleave', 'dragend'].forEach((name) => zone.addEventListener(name, () => zone.classList.remove('drag')));
    zone.addEventListener('drop', (event) => {
      event.preventDefault();
      zone.classList.remove('drag');
      const files = event.dataTransfer && event.dataTransfer.files;
      if (files && files.length > 0) {
        try {
          input.files = files;
        } catch (error) {
          return;
        }
        refreshDropZone(entry);
      }
    });
  });
}

function trackFiles() {
  return trackInput.files ? Array.from(trackInput.files) : [];
}

function refreshReadyHint() {
  const hasReference = Boolean(referenceInput.files && referenceInput.files[0]);
  const tracks = trackFiles().length;
  if (!hasReference && tracks === 0) {
    readyHint.textContent = 'Start by choosing a reference song and your track.';
  } else if (!hasReference) {
    readyHint.textContent = 'Now choose the reference song whose sound you want to match.';
  } else if (tracks === 0) {
    readyHint.textContent = 'Now choose the track you want to master.';
  } else if (tracks > MAX_BATCH_TRACKS) {
    readyHint.textContent = 'You can master up to ' + MAX_BATCH_TRACKS + ' tracks at a time. Remove some and choose again.';
  } else if (tracks > 1) {
    readyHint.textContent = tracks + ' tracks will be mastered one after another with the same settings.';
  } else {
    readyHint.textContent = 'Ready. Check the settings below and press Master my track.';
  }
}

function applyStyle(name) {
  const style = STYLES[name];
  if (!style) {
    return;
  }
  aggressivenessInput.value = String(style.strength);
  aggressivenessValue.textContent = style.strength + '%';
  byId('dynamics').checked = style.dynamics;
  byId('soft-clip').checked = style.softClip;
  byId('transparent').checked = style.transparent === true;
  styleNote.textContent = style.note;
  styleButtons.forEach((button) => button.setAttribute('aria-pressed', String(button.dataset.style === name)));
  updatePlan();
}

function clearStyleSelection() {
  styleButtons.forEach((button) => button.setAttribute('aria-pressed', 'false'));
  styleNote.textContent = 'Custom settings.';
}

function numberOr(input, fallback) {
  const value = Number(input.value);
  return input.value.trim() !== '' && Number.isFinite(value) ? value : fallback;
}

function updatePlan() {
  const target = numberOr(targetInput, null);
  const ceiling = numberOr(ceilingInput, null);
  const parts = [];
  const strength = Number(aggressivenessInput.value);
  parts.push(
    strength === 0
      ? 'The tone is left as it is.'
      : 'The tone of your track is moved ' + strength + '% of the way towards the reference' + (byId('mid-side').checked ? ', in mid and side separately' : '') + '.'
  );
  if (byId('match-width').checked && byId('mid-side').checked) {
    parts.push('The stereo width is matched.');
  }
  if (byId('dynamics').checked) {
    parts.push('Dynamics are matched with a three-band compressor.');
  }
  if (monoBassCheck.checked) {
    parts.push('Everything below ' + monoBassHz.value + ' Hz is made mono.');
  }
  if (target !== null && ceiling !== null) {
    parts.push('The result is set to ' + target + ' LUFS with peaks kept under ' + ceiling + ' dBTP.');
  }
  if (byId('soft-clip').checked) {
    parts.push('A soft clipper rounds the peaks before the limiter.');
  } else if (byId('auto-clip').checked) {
    parts.push('If the limiter would have to work hard, a soft clipper is switched on for you.');
  }
  const depth = bitDepthSelect.value === '0' ? 'the bit depth of your track' : bitDepthSelect.value === '32' ? '32-bit float' : bitDepthSelect.value + '-bit';
  parts.push('You get a WAV at ' + depth + ', and can download MP3, OGG, FLAC and other formats afterwards.');
  const tracks = trackFiles().length;
  if (tracks > 1) {
    parts.unshift(tracks + ' tracks, one after another.');
  }
  planSummary.textContent = 'What will happen: ' + parts.join(' ');
}

function checkInputs() {
  const reference = referenceInput.files && referenceInput.files[0];
  const tracks = trackFiles();
  const files = [['The reference song', reference]];
  tracks.forEach((file, index) => files.push([tracks.length > 1 ? 'Track ' + (index + 1) : 'Your track', file]));
  if (tracks.length === 0) {
    files.push(['Your track', null]);
  }
  if (tracks.length > MAX_BATCH_TRACKS) {
    return 'You can master up to ' + MAX_BATCH_TRACKS + ' tracks at a time.';
  }
  for (const [label, file] of files) {
    if (!file) {
      return label + ' is missing. Choose an audio file.';
    }
    if (!hasAllowedExtension(file.name)) {
      return label + ' must be one of: ' + allowedFormats().map((item) => item.slice(1).toUpperCase()).join(', ') + '.';
    }
    if (file.size === 0) {
      return label + ' is empty.';
    }
    if (limits && file.size > limits.maxFileMb * 1024 * 1024) {
      return label + ' is larger than ' + limits.maxFileMb + ' MB. A FLAC or MP3 of the same song is much smaller.';
    }
  }
  const numbers = [
    ['Loudness target', targetInput],
    ['Peak ceiling', ceilingInput],
    ['Maximum EQ bands', bandsInput],
  ];
  if (monoBassCheck.checked) {
    numbers.push(['Mono bass frequency', monoBassHz]);
  }
  for (const [label, input] of numbers) {
    if (input.value.trim() === '' || !input.checkValidity()) {
      return label + ': ' + (input.validationMessage || 'enter a valid number.');
    }
  }
  return '';
}

function buildFormData(trackFile) {
  const data = new FormData();
  data.append('reference', referenceInput.files[0]);
  data.append('track', trackFile);
  data.append('target_lufs', targetInput.value);
  data.append('aggressiveness', String(Number(aggressivenessInput.value) / 100));
  data.append('ceiling_db', ceilingInput.value);
  data.append('bands', bandsInput.value);
  data.append('bit_depth', bitDepthSelect.value);
  data.append('precision', precisionSelect.value);
  data.append('mono_bass_hz', monoBassCheck.checked ? monoBassHz.value : '0');
  flagInputs.forEach(([name, input]) => {
    data.append(name, input.checked ? 'true' : 'false');
  });
  return data;
}

function readSaved() {
  try {
    const parsed = JSON.parse(window.localStorage.getItem(SETTINGS_KEY) || '[]');
    return Array.isArray(parsed) ? parsed.filter((item) => item && typeof item.name === 'string' && item.values && typeof item.values === 'object') : [];
  } catch (error) {
    return [];
  }
}

function writeSaved(items) {
  try {
    window.localStorage.setItem(SETTINGS_KEY, JSON.stringify(items));
    return true;
  } catch (error) {
    return false;
  }
}

function collectSettings() {
  const values = {
    target: targetInput.value,
    ceiling: ceilingInput.value,
    strength: aggressivenessInput.value,
    bands: bandsInput.value,
    bitDepth: bitDepthSelect.value,
    precision: precisionSelect.value,
    monoBass: monoBassCheck.checked,
    monoBassHz: monoBassHz.value,
    flags: {},
  };
  flagInputs.forEach(([name, input]) => {
    values.flags[name] = input.checked;
  });
  return values;
}

function applySettings(values) {
  const setValue = (input, value) => {
    if (typeof value === 'string' || typeof value === 'number') {
      input.value = String(value);
    }
  };
  setValue(targetInput, values.target);
  setValue(ceilingInput, values.ceiling);
  setValue(aggressivenessInput, values.strength);
  aggressivenessValue.textContent = aggressivenessInput.value + '%';
  setValue(bandsInput, values.bands);
  if (['0', '16', '24', '32'].includes(String(values.bitDepth))) {
    bitDepthSelect.value = String(values.bitDepth);
  }
  if (['fast', 'normal', 'high'].includes(values.precision)) {
    precisionSelect.value = values.precision;
  }
  monoBassCheck.checked = values.monoBass === true;
  setValue(monoBassHz, values.monoBassHz);
  flagInputs.forEach(([name, input]) => {
    if (values.flags && typeof values.flags[name] === 'boolean') {
      input.checked = values.flags[name];
    }
  });
  clearStyleSelection();
  refreshPresetChips();
  updatePlan();
}

function refreshSavedSelect(selectedName) {
  const items = readSaved();
  savedSelect.replaceChildren();
  const first = document.createElement('option');
  first.value = '';
  first.textContent = items.length === 0 ? 'Nothing saved yet' : 'Choose a saved setting…';
  savedSelect.appendChild(first);
  items.forEach((item) => {
    const option = document.createElement('option');
    option.value = item.name;
    option.textContent = item.name;
    savedSelect.appendChild(option);
  });
  savedSelect.value = items.some((item) => item.name === selectedName) ? selectedName : '';
}

function onSavedChange() {
  const item = readSaved().find((entry) => entry.name === savedSelect.value);
  if (item) {
    applySettings(item.values);
    savedName.value = item.name;
    savedNote.textContent = 'Applied "' + item.name + '".';
  }
}

function onSavedSave() {
  const name = savedName.value.trim().slice(0, 40);
  if (name === '') {
    savedNote.textContent = 'Type a name first.';
    return;
  }
  const items = readSaved().filter((entry) => entry.name !== name);
  if (items.length >= MAX_SAVED_SETTINGS) {
    savedNote.textContent = 'You can keep up to ' + MAX_SAVED_SETTINGS + ' saved settings. Delete one first.';
    return;
  }
  items.push({ name: name, values: collectSettings() });
  if (!writeSaved(items)) {
    savedNote.textContent = 'This browser would not let the page save settings.';
    return;
  }
  refreshSavedSelect(name);
  savedNote.textContent = 'Saved "' + name + '".';
}

function onSavedDelete() {
  const name = savedSelect.value || savedName.value.trim();
  const items = readSaved();
  const kept = items.filter((entry) => entry.name !== name);
  if (kept.length === items.length) {
    savedNote.textContent = 'Choose a saved setting to delete.';
    return;
  }
  writeSaved(kept);
  refreshSavedSelect('');
  savedName.value = '';
  savedNote.textContent = 'Deleted "' + name + '".';
}

function matchingPreset() {
  if (!limits || !Array.isArray(limits.presets)) {
    return null;
  }
  const target = Number(targetInput.value);
  const ceiling = Number(ceilingInput.value);
  return (
    limits.presets.find((preset) => Math.abs(preset.targetLufs - target) < 0.001 && Math.abs(preset.ceilingDb - ceiling) < 0.001) || null
  );
}

function refreshPresetChips() {
  const active = matchingPreset();
  Array.from(presetChips.children).forEach((chip) => {
    chip.setAttribute('aria-pressed', String(active !== null && chip.dataset.preset === active.id));
  });
  presetNote.textContent = active ? active.note : 'Custom values. Pick a service above to use its usual target.';
  updatePlan();
}

function buildPresetChips() {
  presetChips.replaceChildren();
  const presets = limits && Array.isArray(limits.presets) ? limits.presets : [];
  presets.forEach((preset) => {
    const chip = document.createElement('button');
    chip.type = 'button';
    chip.className = 'chip';
    chip.dataset.preset = preset.id;
    chip.textContent = preset.name;
    chip.addEventListener('click', () => {
      targetInput.value = String(preset.targetLufs);
      ceilingInput.value = String(preset.ceilingDb);
      refreshPresetChips();
    });
    presetChips.appendChild(chip);
  });
  presetChips.hidden = presets.length === 0;
  refreshPresetChips();
}

function startUpload(data) {
  return new Promise((resolve) => {
    const xhr = new XMLHttpRequest();
    activeUpload = xhr;
    xhr.open('POST', '/api/jobs');
    xhr.upload.addEventListener('progress', (event) => {
      if (event.lengthComputable && event.total > 0) {
        const percent = Math.min(100, (event.loaded / event.total) * 100);
        setProgress(percent, 'Uploading', 'Uploading your files… ' + Math.round(percent) + '%', 0);
      }
    });
    xhr.addEventListener('load', () => {
      if (xhr.status === 401) {
        redirectToLogin();
      }
      resolve({ status: xhr.status, body: parseJson(xhr.responseText) });
    });
    xhr.addEventListener('error', () => resolve({ status: 0, body: null }));
    xhr.addEventListener('abort', () => resolve({ status: -1, body: null }));
    xhr.send(data);
  });
}

async function discardJob(id) {
  if (id) {
    await request('/api/jobs/' + id, { method: 'DELETE' });
  }
}

async function discardResults(items) {
  await Promise.all(items.filter((item) => item.job).map((item) => discardJob(item.job.id)));
}

function backToForm() {
  teardownPlayer();
  busy = false;
  jobId = null;
  cancelRequested = false;
  results = [];
  shownIndex = -1;
  writeStorage(null);
  setFormMessage('');
  progressEta.textContent = '';
  submitButton.disabled = limits === null;
  show(form);
}

function showError(message) {
  teardownPlayer();
  busy = false;
  errorMessage.textContent = message;
  show(errorPanel);
}

async function followJob(id) {
  let failures = 0;
  for (;;) {
    if (cancelRequested) {
      return { cancelled: true };
    }
    const response = await request('/api/jobs/' + id, { cache: 'no-store' });
    if (cancelRequested) {
      return { cancelled: true };
    }
    if (response === null || response.status >= 500) {
      failures += 1;
      if (failures >= MAX_POLL_FAILURES) {
        return { error: 'Lost connection to the server. If it restarted, your job was lost. Please try again.', fatal: true };
      }
      progressMessage.textContent = 'Reconnecting to the server…';
      await delay(POLL_INTERVAL_MS);
      continue;
    }
    if (response.status === 404) {
      return { error: 'This job has expired or the server restarted. Please upload your files again.' };
    }
    const job = response.ok ? parseJson(await response.text()) : null;
    if (job === null) {
      return { error: 'The server sent an unexpected response (HTTP ' + response.status + ').' };
    }
    failures = 0;
    if (job.status === 'done') {
      return { job: job };
    }
    if (job.status === 'failed') {
      return { error: job.error || 'Mastering failed.' };
    }
    if (job.status === 'queued') {
      const position = job.position > 0 ? 'Your place in the queue: ' + job.position : 'Waiting to start';
      setProgress(null, 'Waiting in the queue', position, 0);
    } else {
      setProgress(job.progress, 'Mastering', job.message || 'Working');
    }
    await delay(POLL_INTERVAL_MS);
  }
}

async function processOne(file, position, total) {
  progressStartedAt = Date.now();
  batchStatus.hidden = total < 2;
  batchStatus.textContent = total < 2 ? '' : 'Track ' + (position + 1) + ' of ' + total + ': ' + file.name;
  setProgress(0, 'Uploading', 'Uploading your files…', 0);
  const upload = await startUpload(buildFormData(file));
  activeUpload = null;
  if (cancelRequested || upload.status === -1) {
    return { cancelled: true };
  }
  if (upload.status !== 202 || !upload.body || typeof upload.body.id !== 'string') {
    const fallback =
      upload.status === 0 ? 'Could not reach the server. Check your connection and try again.' : 'The upload failed (HTTP ' + upload.status + ').';
    return { error: describeError(upload.body, fallback), fatal: upload.status === 0 || upload.status === 429 };
  }
  jobId = upload.body.id;
  if (total === 1) {
    writeStorage(jobId);
  }
  progressStartedAt = Date.now();
  setProgress(null, 'Waiting in the queue', 'Upload complete', 0);
  const outcome = await followJob(jobId);
  jobId = null;
  if (total === 1) {
    writeStorage(null);
  }
  return outcome;
}

function summariseFailures() {
  if (results.length === 1) {
    return results[0].error || 'Mastering failed.';
  }
  return results.map((item) => item.name + ': ' + (item.error || 'failed')).join(' · ');
}

function finishBatch() {
  busy = false;
  progressEta.textContent = '';
  const firstDone = results.findIndex((item) => item.job);
  if (firstDone < 0) {
    showError(summariseFailures());
    return;
  }
  showResult(firstDone);
}

async function runBatch(files) {
  busy = true;
  cancelRequested = false;
  results = [];
  shownIndex = -1;
  submitButton.disabled = true;
  setProgress(0, 'Uploading', 'Uploading your files…', 0);
  show(progressPanel);
  for (let index = 0; index < files.length; index += 1) {
    const outcome = await processOne(files[index], index, files.length);
    if (outcome.cancelled) {
      const finished = results;
      backToForm();
      await discardResults(finished);
      return;
    }
    results.push({ name: files[index].name, job: outcome.job || null, error: outcome.error || '' });
    if (outcome.fatal) {
      for (let rest = index + 1; rest < files.length; rest += 1) {
        results.push({ name: files[rest].name, job: null, error: 'Not started.' });
      }
      break;
    }
  }
  finishBatch();
}

async function resumeJob(id) {
  busy = true;
  cancelRequested = false;
  jobId = id;
  results = [];
  progressStartedAt = Date.now();
  setProgress(null, 'Reconnecting', 'Checking on your earlier job…', 0);
  batchStatus.hidden = true;
  show(progressPanel);
  const outcome = await followJob(id);
  jobId = null;
  writeStorage(null);
  if (outcome.cancelled) {
    return;
  }
  results = [{ name: outcome.job ? outcome.job.downloadName : 'Track', job: outcome.job || null, error: outcome.error || '' }];
  finishBatch();
}

async function onSubmit(event) {
  event.preventDefault();
  if (busy || limits === null) {
    return;
  }
  const problem = checkInputs();
  setFormMessage(problem);
  if (problem) {
    return;
  }
  await runBatch(trackFiles());
}

async function onCancel() {
  cancelRequested = true;
  if (activeUpload !== null) {
    activeUpload.abort();
    return;
  }
  const id = jobId;
  if (id) {
    await discardJob(id);
  }
}

async function onLeaveResult() {
  const finished = results;
  backToForm();
  await discardResults(finished);
}

function addChart(title, fit) {
  const block = document.createElement('div');
  block.className = 'chart-block';
  block.appendChild(cell('h4', title));
  if (!fit || !Array.isArray(fit.bands) || fit.bands.length === 0 || !fit.curve || fit.curve.frequencies.length < 2) {
    block.appendChild(cell('p', 'No correction was needed.'));
    chartsContainer.appendChild(block);
    return;
  }
  const legend = document.createElement('p');
  legend.className = 'legend';
  legend.appendChild(cell('span', 'Dashed: what the reference asks for'));
  legend.appendChild(cell('span', 'Solid: what the EQ does'));
  block.appendChild(buildChart(fit.curve));
  block.appendChild(legend);
  block.appendChild(buildBandTable(fit.bands));
  chartsContainer.appendChild(block);
}

function measurementRow(label, values, hasMono) {
  const row = document.createElement('tr');
  const header = cell('th', label);
  header.scope = 'row';
  row.appendChild(header);
  const peakToLoudness = typeof values.plrDb === 'number' && values.plrDb > 0 ? values.plrDb : null;
  row.appendChild(cell('td', formatNumber(values.integratedLufs, 2, 'LUFS')));
  row.appendChild(cell('td', formatNumber(values.truePeakDb, 2, 'dBTP')));
  row.appendChild(cell('td', formatNumber(values.loudnessRangeLu, 1, 'LU')));
  row.appendChild(cell('td', peakToLoudness === null ? '–' : peakToLoudness.toFixed(1) + ' dB'));
  row.appendChild(cell('td', formatNumber(values.stereoCorrelation, 2, '')));
  row.appendChild(cell('td', hasMono && typeof values.monoLossDb === 'number' ? signed(values.monoLossDb, 1, 'dB') : '–'));
  return row;
}

function regionMean(frequencies, values, low, high) {
  let sum = 0;
  let count = 0;
  frequencies.forEach((frequency, index) => {
    if (frequency >= low && frequency < high && typeof values[index] === 'number' && Number.isFinite(values[index])) {
      sum += values[index];
      count += 1;
    }
  });
  return count > 0 ? sum / count : null;
}

function buildToneRegions(spectrum) {
  toneBody.replaceChildren();
  const usable =
    spectrum &&
    Array.isArray(spectrum.frequencies) &&
    Array.isArray(spectrum.input) &&
    Array.isArray(spectrum.reference) &&
    Array.isArray(spectrum.output) &&
    spectrum.output.length === spectrum.frequencies.length &&
    spectrum.input.length === spectrum.frequencies.length &&
    spectrum.reference.length === spectrum.frequencies.length;
  toneWrap.hidden = !usable;
  if (!usable) {
    return;
  }
  TONE_REGIONS.forEach(([label, low, high]) => {
    const reference = regionMean(spectrum.frequencies, spectrum.reference, low, high);
    const before = regionMean(spectrum.frequencies, spectrum.input, low, high);
    const after = regionMean(spectrum.frequencies, spectrum.output, low, high);
    if (reference === null || before === null || after === null) {
      return;
    }
    const row = document.createElement('tr');
    const header = cell('th', label);
    header.scope = 'row';
    row.appendChild(header);
    row.appendChild(cell('td', signed(before - reference, 1, 'dB')));
    const closer = Math.abs(after - reference) < Math.abs(before - reference) - 0.05;
    row.appendChild(cell('td', signed(after - reference, 1, 'dB') + (closer ? ' (closer)' : '')));
    toneBody.appendChild(row);
  });
  toneWrap.hidden = toneBody.children.length === 0;
}

function addSpectrum(report) {
  spectrumBox.replaceChildren();
  const spectrum = report.spectrum;
  if (!spectrum || !Array.isArray(spectrum.frequencies) || spectrum.frequencies.length < 2 || !Array.isArray(spectrum.input)) {
    spectrumBox.appendChild(cell('p', 'No spectrum is available for this job.'));
    toneWrap.hidden = true;
    return;
  }
  const block = document.createElement('div');
  block.className = 'chart-block';
  block.appendChild(buildSpectrumChart(spectrum));
  const legend = document.createElement('p');
  legend.className = 'legend';
  legend.appendChild(cell('span', 'Grey: your track'));
  legend.appendChild(cell('span', 'Orange dashed: reference'));
  legend.appendChild(cell('span', 'Blue: master'));
  block.appendChild(legend);
  spectrumBox.appendChild(block);
  buildToneRegions(spectrum);
}

function statCard(label, value, sub, good) {
  const card = document.createElement('div');
  card.className = 'stat' + (good ? ' good' : '');
  card.appendChild(cell('div', label)).className = 'label';
  card.appendChild(cell('div', value)).className = 'value';
  if (sub) {
    card.appendChild(cell('div', sub)).className = 'sub';
  }
  return card;
}

function buildStats(report) {
  const input = report.input.measurements;
  const output = report.output.measurements;
  const settings = report.settings || {};
  const cards = [];
  const off = typeof settings.targetLufs === 'number' ? output.integratedLufs - settings.targetLufs : null;
  cards.push(
    statCard(
      'Loudness',
      formatNumber(output.integratedLufs, 2, 'LUFS'),
      off === null ? 'was ' + formatNumber(input.integratedLufs, 2, 'LUFS') : 'target ' + settings.targetLufs.toFixed(2) + ', off by ' + Math.abs(off).toFixed(2) + ' LU',
      report.gain.targetReached
    )
  );
  const ceiling = typeof settings.ceilingDb === 'number' ? settings.ceilingDb : null;
  cards.push(
    statCard(
      'True peak',
      formatNumber(output.truePeakDb, 2, 'dBTP'),
      ceiling === null ? 'was ' + formatNumber(input.truePeakDb, 2, 'dBTP') : 'ceiling ' + ceiling.toFixed(2) + ' dBTP',
      ceiling !== null && output.truePeakDb <= ceiling
    )
  );
  const match = report.spectrum && report.spectrum.match;
  if (match && match.measured && match.beforeDb > 0) {
    const gained = Math.round(((match.beforeDb - match.afterDb) / match.beforeDb) * 100);
    cards.push(
      statCard('Tonal match', gained + '% closer', match.beforeDb.toFixed(1) + ' dB to ' + match.afterDb.toFixed(1) + ' dB from the reference', gained > 0)
    );
  }
  if (typeof output.monoLossDb === 'number') {
    cards.push(statCard('Mono fold', signed(output.monoLossDb, 1, 'dB'), 'bass correlation ' + formatNumber(output.lowCorrelation, 2, ''), output.monoLossDb > -2));
  }
  statsBox.replaceChildren(...cards);
}

function fillList(list, items) {
  list.replaceChildren();
  (items || []).forEach((text) => list.appendChild(cell('li', text)));
  list.hidden = list.children.length === 0;
}

function buildBatchBar() {
  batchBar.hidden = results.length < 2;
  batchList.replaceChildren();
  results.forEach((item, index) => {
    const chip = document.createElement('button');
    chip.type = 'button';
    chip.className = 'chip';
    chip.textContent = index + 1 + '. ' + item.name + (item.job ? '' : ' ✗');
    chip.disabled = !item.job;
    if (!item.job) {
      chip.title = item.error || 'This track failed.';
    }
    chip.setAttribute('aria-pressed', String(index === shownIndex));
    chip.addEventListener('click', () => {
      if (index !== shownIndex && item.job) {
        showResult(index);
      }
    });
    batchList.appendChild(chip);
  });
}

function exportFormats() {
  return limits && Array.isArray(limits.exports) ? limits.exports : [];
}

function selectedFormat() {
  return exportFormats().find((item) => item.id === exportFormat.value) || null;
}

function fillQualities() {
  const format = selectedFormat();
  exportQuality.replaceChildren();
  if (!format) {
    exportDescription.textContent = '';
    return;
  }
  format.qualities.forEach((quality) => {
    const option = document.createElement('option');
    option.value = quality.id;
    option.textContent = quality.label;
    exportQuality.appendChild(option);
  });
  exportQuality.disabled = format.qualities.length < 2;
  exportDescription.textContent =
    format.description + '. ' + (format.lossy ? 'The file is encoded, then measured again, and its level is lowered a little if the encoder pushed it over your peak ceiling.' : '');
}

function fillExportFormats() {
  exportFormat.replaceChildren();
  exportFormats().forEach((format) => {
    const option = document.createElement('option');
    option.value = format.id;
    option.textContent = format.label + (format.lossy ? '' : ' (lossless)');
    exportFormat.appendChild(option);
  });
  const preferred = exportFormats().find((item) => item.id === 'mp3') || exportFormats()[0];
  if (preferred) {
    exportFormat.value = preferred.id;
  }
  fillQualities();
  byId('export-panel').hidden = exportFormats().length === 0;
}

function triggerDownload(url, name) {
  const link = document.createElement('a');
  link.href = url;
  link.download = name;
  link.rel = 'noopener';
  document.body.appendChild(link);
  link.click();
  link.remove();
}

function describeExport(info) {
  const measured = info.measured || {};
  let text = info.label + ' · ' + formatSize(info.bytes) + ' · ' + formatNumber(measured.integratedLufs, 2, 'LUFS') + ' · ' + formatNumber(measured.truePeakDb, 2, 'dBTP');
  if (typeof info.gainDb === 'number' && info.gainDb < -0.005) {
    text += ' · level lowered by ' + Math.abs(info.gainDb).toFixed(2) + ' dB to stay under your ceiling';
  }
  if (info.overCeiling) {
    text += ' · still above the ceiling; try a higher quality';
  }
  return text;
}

function renderExports() {
  exportList.replaceChildren();
  const entry = results[shownIndex];
  const items = entry && Array.isArray(entry.exports) ? entry.exports : [];
  items.forEach((info) => {
    const item = document.createElement('li');
    item.appendChild(cell('span', describeExport(info)));
    const link = document.createElement('a');
    link.className = 'button';
    link.href = info.url;
    link.download = info.name;
    link.textContent = 'Download again';
    item.appendChild(link);
    exportList.appendChild(item);
  });
}

async function onExport() {
  const format = selectedFormat();
  const entry = results[shownIndex];
  if (exportBusy || !format || !entry || !entry.job) {
    return;
  }
  exportBusy = true;
  exportButton.disabled = true;
  exportStatus.textContent = format.lossy
    ? 'Encoding and checking the file. This can take up to a minute for a long song…'
    : 'Preparing the file…';
  const response = await request('/api/jobs/' + entry.job.id + '/export', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ format: format.id, quality: exportQuality.value, rate: export48k.checked ? 48000 : 0, metadata: exportTags.checked }),
  });
  exportBusy = false;
  exportButton.disabled = false;
  if (response === null) {
    exportStatus.textContent = 'Could not reach the server. Check your connection and try again.';
    return;
  }
  const payload = parseJson(await response.text());
  if (!response.ok || !payload || typeof payload.url !== 'string') {
    exportStatus.textContent = describeError(payload, 'The file could not be prepared (HTTP ' + response.status + ').');
    return;
  }
  entry.exports = (entry.exports || []).filter((item) => item.key !== payload.key);
  entry.exports.push(payload);
  if (results[shownIndex] === entry) {
    renderExports();
    exportStatus.textContent = 'Ready: ' + describeExport(payload);
  }
  triggerDownload(payload.url, payload.name);
}

function setLinks(job) {
  reportTxt.href = job.reportUrl || '#';
  reportJson.href = (job.reportUrl || '') + '?kind=json';
  reportTxt.hidden = !job.reportUrl;
  reportJson.hidden = !job.reportUrl;
  const eq = job.eq || {};
  eqReaeq.hidden = !eq.reaeq;
  eqApo.hidden = !eq.apo;
  eqReaeq.href = eq.reaeq || '#';
  eqApo.href = eq.apo || '#';
}

function showResult(index) {
  const entry = results[index];
  if (!entry || !entry.job) {
    return;
  }
  shownIndex = index;
  const job = entry.job;
  const report = job.report;
  const output = report.output.measurements;
  heroSummary.textContent =
    formatNumber(output.integratedLufs, 2, 'LUFS') + ' and ' + formatNumber(output.truePeakDb, 2, 'dBTP') + ', ' +
    report.gain.iterations + ' loudness pass' + (report.gain.iterations === 1 ? '' : 'es') + '.';
  buildBatchBar();
  buildStats(report);
  measurementsBody.replaceChildren(
    measurementRow('Your track', report.input.measurements, true),
    measurementRow('Reference', report.references.measurements, true),
    measurementRow('Mastered', report.output.measurements, true)
  );
  gainSummary.textContent = 'Gain applied: ' + formatNumber(report.gain.makeupDb, 1, 'dB') + '.';
  const limiter = report.limiter;
  limiterSummary.textContent = limiter
    ? 'Limiter: up to ' + limiter.maxReductionDb.toFixed(1) + ' dB of gain reduction, ' + limiter.averageReductionDb.toFixed(2) +
      ' dB on average, working during ' + Math.round(limiter.reducedFraction * 100) + '% of the track.' + (limiter.softClip ? ' The soft clipper was on.' : '')
    : '';
  fillList(changesList, describeChanges(report.changes));
  fillList(notesList, (report.notes || []).concat(conversionNotes(report)));
  fillList(warningsList, report.warnings);
  buildPlatforms(report);
  addSpectrum(report);
  chartsContainer.replaceChildren();
  addChart('Mid EQ', report.mid);
  if (report.side && Array.isArray(report.side.bands) && report.side.bands.length > 0) {
    addChart('Side EQ', report.side);
  }
  downloadLink.href = job.downloadUrl;
  downloadLink.setAttribute('download', job.downloadName);
  setLinks(job);
  exportStatus.textContent = '';
  renderExports();
  show(resultPanel);
  setupPlayer(job, report);
  waveData = validWaveform(report.waveform) ? report.waveform : null;
  waveCanvas.hidden = waveData === null;
  drawWave();
}

function buildPlatforms(report) {
  platformsBody.replaceChildren();
  const presets = limits && Array.isArray(limits.presets) ? limits.presets : [];
  const loudness = report.output.measurements.integratedLufs;
  const peak = report.output.measurements.truePeakDb;
  presets.forEach((preset) => {
    const row = document.createElement('tr');
    const header = cell('th', preset.name);
    header.scope = 'row';
    row.appendChild(header);
    row.appendChild(cell('td', preset.targetLufs.toFixed(0) + ' LUFS'));
    const change = preset.targetLufs - loudness;
    let text = 'no change';
    if (change <= -0.15) {
      text = signed(change, 1, 'dB') + ' (turned down)';
    } else if (change >= 0.15) {
      text = signed(change, 1, 'dB') + ' (turned up)';
    }
    row.appendChild(cell('td', text));
    const over = peak - preset.ceilingDb;
    row.appendChild(cell('td', over > 0.05 ? 'above by ' + over.toFixed(1) + ' dB' : 'ok'));
    platformsBody.appendChild(row);
  });
}

function levelMatchGains(report) {
  const gains = { original: 1, mastered: 1 };
  const before = report.input.measurements.integratedLufs;
  const after = report.output.measurements.integratedLufs;
  if (typeof before === 'number' && typeof after === 'number' && before > -100 && after > -100) {
    const difference = after - before;
    if (difference > 0) {
      gains.mastered = Math.pow(10, -difference / 20);
    } else {
      gains.original = Math.pow(10, difference / 20);
    }
  }
  return gains;
}

function validWaveform(waveform) {
  return (
    waveform &&
    waveform.input &&
    waveform.output &&
    Array.isArray(waveform.input.peak) &&
    Array.isArray(waveform.input.rms) &&
    Array.isArray(waveform.output.peak) &&
    Array.isArray(waveform.output.rms) &&
    waveform.input.peak.length > 0 &&
    waveform.input.peak.length === waveform.input.rms.length &&
    waveform.output.peak.length > 0 &&
    waveform.output.peak.length === waveform.output.rms.length
  );
}

function setPlayerNote(text) {
  playerNote.textContent = text;
}

function currentDuration() {
  return player && player.impl ? player.impl.duration() : 0;
}

function currentTime() {
  return player && player.impl ? player.impl.time() : 0;
}

function drawWave() {
  if (!waveData) {
    return;
  }
  const ratio = window.devicePixelRatio || 1;
  const width = Math.max(1, Math.round(waveCanvas.clientWidth * ratio));
  const height = Math.max(1, Math.round(waveCanvas.clientHeight * ratio));
  if (waveCanvas.width !== width || waveCanvas.height !== height) {
    waveCanvas.width = width;
    waveCanvas.height = height;
  }
  const context = waveCanvas.getContext('2d');
  if (!context) {
    return;
  }
  const styles = window.getComputedStyle(document.documentElement);
  const back = styles.getPropertyValue('--wave-back').trim() || '#445';
  const front = styles.getPropertyValue('--wave-front').trim() || '#6ea0ff';
  const active = player ? player.active : 'mastered';
  const data = active === 'original' ? waveData.input : waveData.output;
  context.clearRect(0, 0, width, height);
  const middle = height / 2;
  const count = data.peak.length;
  const shape = (value) => Math.pow(Math.min(Math.max(value, 0), 1), 0.6);
  for (let x = 0; x < width; x += 1) {
    const index = Math.min(count - 1, Math.floor((x / width) * count));
    const peak = shape(data.peak[index]) * middle * 0.95;
    const rms = shape(data.rms[index]) * middle * 0.95;
    context.fillStyle = back;
    context.fillRect(x, middle - peak, 1, Math.max(1, peak * 2));
    context.fillStyle = front;
    context.fillRect(x, middle - rms, 1, Math.max(1, rms * 2));
  }
  const duration = currentDuration();
  if (player && duration > 0) {
    const position = Math.round((currentTime() / duration) * width);
    context.fillStyle = styles.getPropertyValue('--text').trim() || '#fff';
    context.fillRect(Math.min(width - 2, position), 0, Math.max(1, Math.round(ratio)), height);
  }
}

function refreshTime() {
  if (!player || !player.impl) {
    return;
  }
  const duration = currentDuration();
  const now = currentTime();
  if (!player.dragging) {
    seekInput.value = duration > 0 ? String(Math.round((now / duration) * 1000)) : '0';
  }
  timeLabel.textContent = formatTime(now) + ' / ' + formatTime(duration);
  drawWave();
}

function seekToFraction(fraction) {
  if (!player || !player.impl) {
    return;
  }
  const duration = currentDuration();
  if (duration > 0) {
    player.impl.seek(Math.min(Math.max(fraction, 0), 1) * duration);
  }
  refreshTime();
}

function applyLevels() {
  if (!player || !player.impl) {
    return;
  }
  player.impl.setLevels(player.active, player.match, player.gains);
  hearOriginal.setAttribute('aria-pressed', String(player.active === 'original'));
  hearMastered.setAttribute('aria-pressed', String(player.active === 'mastered'));
  drawWave();
}

function animate() {
  frameRequest = null;
  if (!player || !player.playing) {
    return;
  }
  refreshTime();
  frameRequest = window.requestAnimationFrame(animate);
}

function setPlaying(playing) {
  player.playing = playing;
  playButton.textContent = playing ? 'Pause' : 'Play';
  if (playing && frameRequest === null) {
    frameRequest = window.requestAnimationFrame(animate);
  }
}

function pausePlayer() {
  if (player && player.impl) {
    player.impl.pause();
    setPlaying(false);
  }
}

async function togglePlay() {
  if (!player || !player.impl) {
    return;
  }
  if (player.playing) {
    pausePlayer();
    return;
  }
  applyLevels();
  setPlayerNote('');
  try {
    await player.impl.play();
    setPlaying(true);
  } catch (error) {
    pausePlayer();
    setPlayerNote('Playback could not start. Tap Play again.');
  }
  refreshTime();
}

function onSeekInput() {
  if (!player) {
    return;
  }
  player.dragging = true;
  seekToFraction(Number(seekInput.value) / 1000);
}

function endSeekDrag() {
  if (player) {
    player.dragging = false;
  }
}

function chooseVersion(name) {
  if (!player || !player.impl) {
    return;
  }
  player.active = name;
  applyLevels();
}

function waveFraction(event) {
  const box = waveCanvas.getBoundingClientRect();
  return box.width > 0 ? (event.clientX - box.left) / box.width : 0;
}

function onWavePointerDown(event) {
  if (!player || !player.impl) {
    return;
  }
  player.scrubbing = true;
  try {
    waveCanvas.setPointerCapture(event.pointerId);
  } catch (error) {
    ignore();
  }
  seekToFraction(waveFraction(event));
}

function onWavePointerMove(event) {
  if (player && player.scrubbing) {
    seekToFraction(waveFraction(event));
  }
}

function onWavePointerEnd() {
  if (player) {
    player.scrubbing = false;
  }
}

function onKeyDown(event) {
  if (!player || !player.impl || resultPanel.hidden || event.ctrlKey || event.metaKey || event.altKey) {
    return;
  }
  const tag = event.target && event.target.tagName ? event.target.tagName : '';
  if (tag === 'INPUT' || tag === 'SELECT' || tag === 'TEXTAREA') {
    return;
  }
  const key = event.key;
  if (key === ' ' || key === 'Spacebar') {
    if (tag === 'BUTTON' || tag === 'A') {
      return;
    }
    event.preventDefault();
    togglePlay();
  } else if (key === 'a' || key === 'A') {
    chooseVersion('original');
  } else if (key === 'b' || key === 'B') {
    chooseVersion('mastered');
  } else if (key === 'ArrowLeft' || key === 'ArrowRight') {
    const duration = currentDuration();
    if (duration > 0) {
      event.preventDefault();
      const step = key === 'ArrowLeft' ? -SEEK_STEP_SECONDS : SEEK_STEP_SECONDS;
      seekToFraction((currentTime() + step) / duration);
    }
  }
}

function handleEnded() {
  if (!player) {
    return;
  }
  setPlaying(false);
  refreshTime();
}

function handleMediaError() {
  if (player) {
    pausePlayer();
    setPlayerNote('The preview could not be loaded. You can still download the WAV file.');
  }
}

function withTimeout(promise, milliseconds) {
  return new Promise((resolve, reject) => {
    const timer = window.setTimeout(() => reject(new Error('The preview took too long to load')), milliseconds);
    promise.then(
      (value) => {
        window.clearTimeout(timer);
        resolve(value);
      },
      (error) => {
        window.clearTimeout(timer);
        reject(error);
      }
    );
  });
}

async function createBufferImpl(job, Context) {
  let context = null;
  try {
    context = new Context();
    const decode = async (url) => {
      const response = await request(url);
      if (response === null || !response.ok) {
        throw new Error('The preview could not be loaded');
      }
      const data = await response.arrayBuffer();
      return new Promise((resolve, reject) => {
        const pending = context.decodeAudioData(data, resolve, reject);
        if (pending && typeof pending.then === 'function') {
          pending.then(resolve, reject);
        }
      });
    };
    const loaded = await withTimeout(Promise.all([decode(job.preview.original), decode(job.preview.mastered)]), PREVIEW_LOAD_TIMEOUT_MS);
    let buffers = { original: loaded[0], mastered: loaded[1] };
    const length = buffers.mastered.duration;
    const gains = {};
    ['original', 'mastered'].forEach((name) => {
      gains[name] = context.createGain();
      gains[name].gain.value = 0;
      gains[name].connect(context.destination);
    });
    let sources = null;
    let generation = 0;
    let startedAt = 0;
    let startOffset = 0;
    let pausedAt = 0;
    const impl = { kind: 'buffer', playing: false, onEnded: null };

    const stopSources = () => {
      generation += 1;
      if (sources) {
        ['original', 'mastered'].forEach((name) => {
          try {
            sources[name].onended = null;
            sources[name].stop();
          } catch (error) {
            ignore();
          }
          try {
            sources[name].disconnect();
          } catch (error) {
            ignore();
          }
        });
        sources = null;
      }
    };
    const start = (offset) => {
      stopSources();
      const token = generation;
      const when = context.currentTime + 0.03;
      sources = {};
      ['original', 'mastered'].forEach((name) => {
        const node = context.createBufferSource();
        node.buffer = buffers[name];
        node.connect(gains[name]);
        node.start(when, offset);
        sources[name] = node;
      });
      startedAt = when;
      startOffset = offset;
      sources.mastered.onended = () => {
        if (token === generation && impl.playing) {
          pausedAt = length;
          impl.playing = false;
          stopSources();
          if (impl.onEnded) {
            impl.onEnded();
          }
        }
      };
    };

    impl.duration = () => length;
    impl.time = () => (impl.playing ? Math.min(length, startOffset + Math.max(0, context.currentTime - startedAt)) : pausedAt);
    impl.seek = (seconds) => {
      const target = Math.min(Math.max(seconds, 0), length);
      pausedAt = target;
      if (impl.playing) {
        start(target);
      }
    };
    impl.play = async () => {
      if (context.state === 'suspended') {
        await context.resume();
      }
      if (pausedAt >= length - 0.05) {
        pausedAt = 0;
      }
      impl.playing = true;
      start(pausedAt);
    };
    impl.pause = () => {
      if (impl.playing) {
        pausedAt = impl.time();
        impl.playing = false;
        stopSources();
      }
    };
    impl.setLevels = (active, match, matchGains) => {
      ['original', 'mastered'].forEach((name) => {
        const level = active === name ? (match ? matchGains[name] : 1) : 0;
        gains[name].gain.setTargetAtTime(level, context.currentTime, 0.015);
      });
    };
    impl.destroy = () => {
      impl.playing = false;
      stopSources();
      ['original', 'mastered'].forEach((name) => {
        try {
          gains[name].disconnect();
        } catch (error) {
          ignore();
        }
      });
      buffers = null;
      context.close().then(ignore, ignore);
    };
    return impl;
  } catch (error) {
    if (context) {
      context.close().then(ignore, ignore);
    }
    throw error;
  }
}

function createAudio(source) {
  const audio = document.createElement('audio');
  audio.preload = 'auto';
  audio.src = source;
  return audio;
}

function createElementImpl(job) {
  const original = createAudio(job.preview.original);
  const mastered = createAudio(job.preview.mastered);
  const impl = { kind: 'element', playing: false, onEnded: null, context: null, nodes: null, failed: false };
  const ensureGraph = () => {
    if (impl.nodes || impl.failed) {
      return;
    }
    const Context = window.AudioContext || window.webkitAudioContext;
    if (!Context) {
      impl.failed = true;
      return;
    }
    let context = null;
    try {
      context = new Context();
      const nodes = {};
      [['original', original], ['mastered', mastered]].forEach(([name, element]) => {
        const source = context.createMediaElementSource(element);
        const gain = context.createGain();
        source.connect(gain);
        gain.connect(context.destination);
        nodes[name] = gain;
        element.muted = false;
        element.volume = 1;
      });
      impl.context = context;
      impl.nodes = nodes;
    } catch (error) {
      impl.failed = true;
      if (context) {
        context.close().then(ignore, ignore);
      }
    }
  };
  mastered.addEventListener('timeupdate', () => {
    if (impl.playing && Math.abs(original.currentTime - mastered.currentTime) > 0.1) {
      original.currentTime = mastered.currentTime;
    }
  });
  mastered.addEventListener('durationchange', refreshTime);
  mastered.addEventListener('loadedmetadata', refreshTime);
  mastered.addEventListener('ended', () => {
    original.pause();
    impl.playing = false;
    if (impl.onEnded) {
      impl.onEnded();
    }
  });
  mastered.addEventListener('error', handleMediaError);
  original.addEventListener('error', handleMediaError);
  playerMedia.replaceChildren(original, mastered);

  impl.duration = () => (Number.isFinite(mastered.duration) ? mastered.duration : 0);
  impl.time = () => mastered.currentTime;
  impl.seek = (seconds) => {
    mastered.currentTime = seconds;
    original.currentTime = seconds;
  };
  impl.play = async () => {
    ensureGraph();
    if (impl.context && impl.context.state === 'suspended') {
      await impl.context.resume();
    }
    const duration = impl.duration();
    if (mastered.ended || (duration > 0 && mastered.currentTime >= duration - 0.05)) {
      mastered.currentTime = 0;
    }
    original.currentTime = mastered.currentTime;
    await Promise.all([mastered.play(), original.play()]);
    impl.playing = true;
  };
  impl.pause = () => {
    original.pause();
    mastered.pause();
    impl.playing = false;
  };
  impl.setLevels = (active, match, matchGains) => {
    ['original', 'mastered'].forEach((name) => {
      const level = active === name ? (match ? matchGains[name] : 1) : 0;
      if (impl.nodes) {
        impl.nodes[name].gain.setTargetAtTime(level, impl.context.currentTime, 0.015);
      } else {
        const element = name === 'original' ? original : mastered;
        element.muted = active !== name;
        element.volume = Math.min(1, level);
      }
    });
  };
  impl.destroy = () => {
    impl.playing = false;
    [original, mastered].forEach((element) => {
      element.pause();
      element.removeAttribute('src');
      element.load();
    });
    if (impl.context) {
      impl.context.close().then(ignore, ignore);
    }
    playerMedia.replaceChildren();
  };
  return impl;
}

async function setupPlayer(job, report) {
  teardownPlayer();
  if (!job.preview) {
    playerSection.hidden = true;
    return;
  }
  const token = playerToken + 1;
  playerToken = token;
  player = {
    impl: null,
    active: 'mastered',
    match: matchLevel.checked,
    gains: levelMatchGains(report),
    playing: false,
    dragging: false,
    scrubbing: false,
  };
  playerSection.hidden = false;
  playButton.disabled = true;
  playerMode.textContent = 'Loading both versions…';
  const Context = window.AudioContext || window.webkitAudioContext;
  const seconds = report.input.frames / Math.max(1, report.input.sampleRate);
  const coarse = Boolean(window.matchMedia && window.matchMedia('(pointer: coarse)').matches);
  const longest = coarse ? BUFFER_PLAYER_MAX_SECONDS_TOUCH : BUFFER_PLAYER_MAX_SECONDS;
  let impl = null;
  let mode = '';
  if (Context && seconds > 0 && seconds <= longest) {
    try {
      impl = await createBufferImpl(job, Context);
      mode = 'Sample-accurate A/B: both versions are loaded and play on one clock, so switching never skips or drifts.';
    } catch (error) {
      impl = null;
    }
    if (token !== playerToken) {
      if (impl) {
        impl.destroy();
      }
      return;
    }
  }
  if (impl === null) {
    impl = createElementImpl(job);
    mode = seconds > longest ? 'Standard A/B: this track is long, so two streams are kept in step.' : 'Standard A/B: two streams are kept in step.';
  }
  impl.onEnded = handleEnded;
  player.impl = impl;
  playerMode.textContent = mode;
  playButton.disabled = false;
  const before = report.input.measurements.integratedLufs;
  const after = report.output.measurements.integratedLufs;
  const louder = typeof before === 'number' && typeof after === 'number' && before > -100 && after > -100 ? after - before : 0;
  setPlayerNote(
    Math.abs(louder) >= 0.1
      ? 'The mastered version is ' + Math.abs(louder).toFixed(1) + ' dB ' + (louder > 0 ? 'louder' : 'quieter') +
        ' than your original. Match loudness compares them at the same volume.'
      : ''
  );
  setPlaying(false);
  applyLevels();
  refreshTime();
}

function teardownPlayer() {
  playerToken += 1;
  waveData = null;
  if (frameRequest !== null) {
    window.cancelAnimationFrame(frameRequest);
    frameRequest = null;
  }
  const current = player;
  player = null;
  if (current && current.impl) {
    current.impl.destroy();
  }
  playerMedia.replaceChildren();
  playButton.textContent = 'Play';
  playButton.disabled = false;
  seekInput.value = '0';
  timeLabel.textContent = '0:00 / 0:00';
  playerMode.textContent = '';
  setPlayerNote('');
  playerSection.hidden = true;
}

async function onLogout() {
  await request('/api/logout', { method: 'POST' });
  redirectToLogin();
}

async function showAccount() {
  const response = await request('/api/session', { cache: 'no-store' });
  if (response === null || !response.ok) {
    return;
  }
  const session = parseJson(await response.text());
  if (session && session.authRequired && session.user) {
    accountName.textContent = 'Signed in as ' + session.user;
    accountBox.hidden = false;
  }
}

function describeLimits() {
  const minutes = Math.round(limits.maxDurationSec / 60);
  return (
    'Limits: ' + limits.maxFileMb + ' MB and ' + minutes + ' minutes per file, ' + limits.jobsPerHour + ' jobs per hour. ' +
    'Results stay available for ' + limits.resultMinutes + ' minutes.'
  );
}

async function init() {
  setupDropZones();
  aggressivenessInput.addEventListener('input', () => {
    aggressivenessValue.textContent = aggressivenessInput.value + '%';
    clearStyleSelection();
  });
  byId('dynamics').addEventListener('change', clearStyleSelection);
  byId('soft-clip').addEventListener('change', clearStyleSelection);
  byId('transparent').addEventListener('change', clearStyleSelection);
  styleButtons.forEach((button) => button.addEventListener('click', () => applyStyle(button.dataset.style)));
  [targetInput, ceilingInput].forEach((input) => input.addEventListener('input', refreshPresetChips));
  form.addEventListener('input', updatePlan);
  form.addEventListener('change', updatePlan);
  form.addEventListener('submit', onSubmit);
  cancelButton.addEventListener('click', onCancel);
  retryButton.addEventListener('click', onLeaveResult);
  againButton.addEventListener('click', onLeaveResult);
  logoutButton.addEventListener('click', onLogout);
  playButton.addEventListener('click', togglePlay);
  seekInput.addEventListener('input', onSeekInput);
  seekInput.addEventListener('change', endSeekDrag);
  seekInput.addEventListener('blur', endSeekDrag);
  seekInput.addEventListener('pointercancel', endSeekDrag);
  waveCanvas.addEventListener('pointerdown', onWavePointerDown);
  waveCanvas.addEventListener('pointermove', onWavePointerMove);
  waveCanvas.addEventListener('pointerup', onWavePointerEnd);
  waveCanvas.addEventListener('pointercancel', onWavePointerEnd);
  window.addEventListener('resize', drawWave);
  document.addEventListener('keydown', onKeyDown);
  hearOriginal.addEventListener('click', () => chooseVersion('original'));
  hearMastered.addEventListener('click', () => chooseVersion('mastered'));
  matchLevel.addEventListener('change', () => {
    if (player) {
      player.match = matchLevel.checked;
      applyLevels();
    }
  });
  exportFormat.addEventListener('change', fillQualities);
  exportButton.addEventListener('click', onExport);
  savedSelect.addEventListener('change', onSavedChange);
  savedSave.addEventListener('click', onSavedSave);
  savedDelete.addEventListener('click', onSavedDelete);
  window.addEventListener('pagehide', teardownPlayer);
  applyStyle('balanced');
  refreshSavedSelect('');
  refreshReadyHint();
  updatePlan();
  show(form);
  const ready = await loadConfig();
  if (!ready) {
    setStatus('Could not reach the server. Reload the page to try again.');
    return;
  }
  setStatus('');
  showAccount();
  const extensions = allowedFormats();
  referenceInput.setAttribute('accept', extensions.join(',') + ',audio/*');
  trackInput.setAttribute('accept', extensions.join(',') + ',audio/*');
  formatsHint.textContent =
    'Accepted files: ' + extensions.map((item) => item.slice(1).toUpperCase()).join(', ') +
    '. Large WAV files can be sent as FLAC or MP3 instead, which are much smaller.';
  limitsText.textContent = describeLimits();
  buildPresetChips();
  fillExportFormats();
  submitButton.disabled = false;
  const saved = readStorage();
  if (saved) {
    resumeJob(saved);
    return;
  }
  const response = await request('/api/jobs');
  const payload = response && response.ok ? parseJson(await response.text()) : null;
  const open = payload && Array.isArray(payload.jobs) ? payload.jobs.find((item) => item.status === 'running' || item.status === 'queued' || item.status === 'done') : null;
  if (open && !busy) {
    resumeJob(open.id);
  }
}

init();
