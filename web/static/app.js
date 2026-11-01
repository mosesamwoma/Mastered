'use strict';

(function () {
  const POLL_INTERVAL_MS = 1500;
  const CONFIG_RETRY_MS = 3000;
  const CONFIG_MAX_ATTEMPTS = 40;
  const MAX_POLL_FAILURES = 6;
  const STORAGE_KEY = 'masteredJob';
  const SVG_NS = 'http://www.w3.org/2000/svg';
  const SEEK_STEP_SECONDS = 5;

  // Styles only set options the form already has, so what they do stays visible and editable.
  const STYLES = {
    gentle: { strength: 60, dynamics: false, softClip: false, note: 'A light touch: the tone follows the reference only partly and dynamics are left alone.' },
    balanced: { strength: 85, dynamics: false, softClip: false, note: 'The default: tone follows the reference closely, dynamics are left alone.' },
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
  ];
  const submitButton = byId('submit');
  const formMessage = byId('form-message');
  const serverStatus = byId('server-status');
  const limitsText = byId('limits');
  const progressPanel = byId('progress-panel');
  const progressTitle = byId('progress-title');
  const progressSteps = byId('progress-steps');
  const progressBar = byId('progress-bar');
  const progressMessage = byId('progress-message');
  const cancelButton = byId('cancel');
  const errorPanel = byId('error-panel');
  const errorMessage = byId('error-message');
  const retryButton = byId('retry');
  const resultPanel = byId('result-panel');
  const downloadLink = byId('download');
  const heroSummary = byId('hero-summary');
  const statsBox = byId('stats');
  const warningsList = byId('warnings');
  const measurementsBody = byId('measurements');
  const platformsBody = byId('platforms');
  const gainSummary = byId('gain-summary');
  const limiterSummary = byId('limiter-summary');
  const spectrumBox = byId('spectrum');
  const chartsContainer = byId('charts');
  const againButton = byId('again');
  const accountBox = byId('account');
  const accountName = byId('account-name');
  const logoutButton = byId('logout');
  const formatsHint = byId('formats-hint');
  const playerSection = byId('player');
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
  let pollTimer = null;
  let pollFailures = 0;
  let activeUpload = null;
  let busy = false;
  let player = null;
  let waveData = null;
  let frameRequest = null;

  function readStorage() {
    try {
      return window.sessionStorage.getItem(STORAGE_KEY);
    } catch (error) {
      return null;
    }
  }

  function writeStorage(value) {
    try {
      if (value) {
        window.sessionStorage.setItem(STORAGE_KEY, value);
      } else {
        window.sessionStorage.removeItem(STORAGE_KEY);
      }
    } catch (error) {
      return;
    }
  }

  function ignore() {
    return undefined;
  }

  function delay(ms) {
    return new Promise((resolve) => window.setTimeout(resolve, ms));
  }

  function redirectToLogin() {
    window.location.assign('/login');
  }

  async function request(url, options) {
    let response;
    try {
      response = await fetch(url, options);
    } catch (error) {
      return null;
    }
    if (response.status === 401) {
      redirectToLogin();
    }
    return response;
  }

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
    if (/measuring|matching loudness|writing|adjusting|checking/i.test(text)) {
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

  function setProgress(value, title, message, step) {
    if (value === null) {
      progressBar.removeAttribute('value');
    } else {
      progressBar.value = value;
    }
    progressTitle.textContent = title;
    progressMessage.textContent = message;
    paintSteps(typeof step === 'number' ? step : stepIndexFor(message, value));
  }

  function formatNumber(value, digits, unit) {
    if (typeof value !== 'number' || !Number.isFinite(value) || value <= -100) {
      return '–';
    }
    return value.toFixed(digits) + (unit ? ' ' + unit : '');
  }

  function signed(value, digits, unit) {
    if (typeof value !== 'number' || !Number.isFinite(value)) {
      return '–';
    }
    return (value > 0 ? '+' : value < 0 ? '−' : '') + Math.abs(value).toFixed(digits) + (unit ? ' ' + unit : '');
  }

  function describeError(payload, fallback) {
    if (payload && typeof payload.detail === 'string') {
      return payload.detail;
    }
    if (payload && Array.isArray(payload.detail) && payload.detail.length > 0) {
      return payload.detail
        .map((item) => {
          const field = Array.isArray(item.loc) ? item.loc[item.loc.length - 1] : '';
          return field ? field + ': ' + item.msg : String(item.msg);
        })
        .join('; ');
    }
    return fallback;
  }

  function parseJson(text) {
    try {
      return JSON.parse(text);
    } catch (error) {
      return null;
    }
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

  function formatSize(bytes) {
    return bytes >= 1048576 ? (bytes / 1048576).toFixed(1) + ' MB' : Math.max(1, Math.round(bytes / 1024)) + ' KB';
  }

  function refreshDropZone(entry) {
    const [zone, input, nameNode, placeholder] = entry;
    const file = input.files && input.files[0];
    nameNode.textContent = file ? file.name + ' (' + formatSize(file.size) + ')' : placeholder;
    zone.classList.toggle('filled', Boolean(file));
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

  function applyStyle(name) {
    const style = STYLES[name];
    if (!style) {
      return;
    }
    aggressivenessInput.value = String(style.strength);
    aggressivenessValue.textContent = style.strength + '%';
    byId('dynamics').checked = style.dynamics;
    byId('soft-clip').checked = style.softClip;
    styleNote.textContent = style.note;
    styleButtons.forEach((button) => button.setAttribute('aria-pressed', String(button.dataset.style === name)));
  }

  function clearStyleSelection() {
    styleButtons.forEach((button) => button.setAttribute('aria-pressed', 'false'));
    styleNote.textContent = 'Custom settings.';
  }

  function checkInputs() {
    const files = [
      ['The reference song', referenceInput.files[0]],
      ['Your track', trackInput.files[0]],
    ];
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
        return label + ' is larger than ' + limits.maxFileMb + ' MB.';
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

  function buildFormData() {
    const data = new FormData();
    data.append('reference', referenceInput.files[0]);
    data.append('track', trackInput.files[0]);
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

  function stopPolling() {
    if (pollTimer !== null) {
      window.clearTimeout(pollTimer);
      pollTimer = null;
    }
  }

  function schedulePoll() {
    stopPolling();
    pollTimer = window.setTimeout(poll, POLL_INTERVAL_MS);
  }

  async function discardJob(id) {
    if (id) {
      await request('/api/jobs/' + id, { method: 'DELETE' });
    }
  }

  function backToForm() {
    teardownPlayer();
    stopPolling();
    busy = false;
    jobId = null;
    writeStorage(null);
    setFormMessage('');
    submitButton.disabled = limits === null;
    show(form);
  }

  function showError(message) {
    teardownPlayer();
    stopPolling();
    busy = false;
    errorMessage.textContent = message;
    show(errorPanel);
  }

  async function poll() {
    pollTimer = null;
    const id = jobId;
    if (!id) {
      return;
    }
    const response = await request('/api/jobs/' + id, { cache: 'no-store' });
    if (id !== jobId) {
      return;
    }
    if (response === null || response.status >= 500) {
      pollFailures += 1;
      if (pollFailures >= MAX_POLL_FAILURES) {
        showError('Lost connection to the server. If it restarted, your job was lost. Please try again.');
        return;
      }
      progressMessage.textContent = 'Reconnecting to the server…';
      schedulePoll();
      return;
    }
    if (response.status === 404) {
      writeStorage(null);
      jobId = null;
      showError('This job has expired or the server restarted. Please upload your files again.');
      return;
    }
    const job = response.ok ? parseJson(await response.text()) : null;
    if (job === null) {
      showError('The server sent an unexpected response (HTTP ' + response.status + ').');
      return;
    }
    pollFailures = 0;
    if (job.status === 'done') {
      showResult(job);
    } else if (job.status === 'failed') {
      showError(job.error || 'Mastering failed.');
    } else if (job.status === 'queued') {
      const position = job.position > 0 ? 'Your place in the queue: ' + job.position : 'Waiting to start';
      setProgress(null, 'Waiting in the queue', position, 0);
      schedulePoll();
    } else {
      setProgress(job.progress, 'Mastering', job.message || 'Working');
      schedulePoll();
    }
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
    busy = true;
    submitButton.disabled = true;
    setProgress(0, 'Uploading', 'Uploading your files…', 0);
    show(progressPanel);
    const result = await startUpload(buildFormData());
    activeUpload = null;
    if (result.status === -1) {
      backToForm();
      return;
    }
    if (result.status !== 202 || !result.body || typeof result.body.id !== 'string') {
      const fallback =
        result.status === 0 ? 'Could not reach the server. Check your connection and try again.' : 'The upload failed (HTTP ' + result.status + ').';
      showError(describeError(result.body, fallback));
      return;
    }
    jobId = result.body.id;
    writeStorage(jobId);
    pollFailures = 0;
    setProgress(null, 'Waiting in the queue', 'Upload complete', 0);
    poll();
  }

  async function onCancel() {
    if (activeUpload !== null) {
      activeUpload.abort();
      return;
    }
    const id = jobId;
    backToForm();
    await discardJob(id);
  }

  async function onRetry() {
    const id = jobId;
    backToForm();
    await discardJob(id);
  }

  function cell(tag, text) {
    const node = document.createElement(tag);
    node.textContent = text;
    return node;
  }

  function measurementRow(label, values, hasMono) {
    const row = document.createElement('tr');
    const header = cell('th', label);
    header.scope = 'row';
    row.appendChild(header);
    const peakToLoudness = typeof values.plrDb === 'number' && values.plrDb > 0 ? values.plrDb : null;
    row.appendChild(cell('td', formatNumber(values.integratedLufs, 1, 'LUFS')));
    row.appendChild(cell('td', formatNumber(values.truePeakDb, 1, 'dBTP')));
    row.appendChild(cell('td', formatNumber(values.loudnessRangeLu, 1, 'LU')));
    row.appendChild(cell('td', peakToLoudness === null ? '–' : peakToLoudness.toFixed(1) + ' dB'));
    row.appendChild(cell('td', formatNumber(values.stereoCorrelation, 2, '')));
    row.appendChild(cell('td', hasMono && typeof values.monoLossDb === 'number' ? signed(values.monoLossDb, 1, 'dB') : '–'));
    return row;
  }

  function svgNode(name, attributes) {
    const node = document.createElementNS(SVG_NS, name);
    Object.keys(attributes).forEach((key) => node.setAttribute(key, String(attributes[key])));
    return node;
  }

  function frequencyLabel(value) {
    return value >= 1000 ? value / 1000 + 'k' : String(value);
  }

  function chartFrame(frequencies, limit) {
    const width = 640;
    const height = 260;
    const left = 46;
    const right = 12;
    const top = 12;
    const bottom = 30;
    const plotWidth = width - left - right;
    const plotHeight = height - top - bottom;
    const logMin = Math.log(frequencies[0]);
    const logMax = Math.log(frequencies[frequencies.length - 1]);
    const xOf = (frequency) => left + ((Math.log(frequency) - logMin) / (logMax - logMin)) * plotWidth;
    const yOf = (value) => top + (1 - (value + limit) / (2 * limit)) * plotHeight;
    const pathOf = (values) =>
      values
        .map((value, index) => (index === 0 ? 'M' : 'L') + xOf(frequencies[index]).toFixed(1) + ' ' + yOf(value).toFixed(1))
        .join(' ');
    const svg = svgNode('svg', { viewBox: '0 0 ' + width + ' ' + height, role: 'img' });
    [-limit, -limit / 2, 0, limit / 2, limit].forEach((value) => {
      const y = yOf(value);
      svg.appendChild(svgNode('line', { class: 'chart-axis', x1: left, x2: width - right, y1: y.toFixed(1), y2: y.toFixed(1) }));
      const label = svgNode('text', { class: 'chart-label', x: left - 6, y: (y + 4).toFixed(1), 'text-anchor': 'end' });
      label.textContent = (value > 0 ? '+' : '') + value + ' dB';
      svg.appendChild(label);
    });
    [50, 100, 200, 500, 1000, 2000, 5000, 10000].forEach((frequency) => {
      if (frequency < frequencies[0] || frequency > frequencies[frequencies.length - 1]) {
        return;
      }
      const x = xOf(frequency);
      svg.appendChild(svgNode('line', { class: 'chart-axis', x1: x.toFixed(1), x2: x.toFixed(1), y1: top, y2: height - bottom }));
      const label = svgNode('text', { class: 'chart-label', x: x.toFixed(1), y: height - 10, 'text-anchor': 'middle' });
      label.textContent = frequencyLabel(frequency);
      svg.appendChild(label);
    });
    return { svg, pathOf };
  }

  function buildChart(curve) {
    const peak = Math.max(0, ...curve.targetDb.map(Math.abs), ...curve.responseDb.map(Math.abs));
    const limit = Math.max(6, Math.ceil(peak / 3) * 3);
    const frame = chartFrame(curve.frequencies, limit);
    frame.svg.setAttribute('aria-label', 'EQ curve');
    frame.svg.appendChild(svgNode('path', { class: 'chart-target', d: frame.pathOf(curve.targetDb) }));
    frame.svg.appendChild(svgNode('path', { class: 'chart-fit', d: frame.pathOf(curve.responseDb) }));
    return frame.svg;
  }

  function buildSpectrumChart(spectrum) {
    const series = [
      ['chart-input', spectrum.input],
      ['chart-reference', spectrum.reference],
      ['chart-output', spectrum.output],
    ].filter((entry) => Array.isArray(entry[1]) && entry[1].length === spectrum.frequencies.length);
    const peak = Math.max(0, ...series.flatMap((entry) => entry[1].map(Math.abs)));
    const limit = Math.min(60, Math.max(12, Math.ceil(peak / 6) * 6));
    const frame = chartFrame(spectrum.frequencies, limit);
    frame.svg.setAttribute('aria-label', 'Average spectrum of your track, the reference and the master');
    series.forEach((entry) => {
      const clipped = entry[1].map((value) => Math.max(-limit, Math.min(limit, value)));
      frame.svg.appendChild(svgNode('path', { class: entry[0], d: frame.pathOf(clipped) }));
    });
    return frame.svg;
  }

  function bandTypeLabel(type) {
    if (type === 'shelf-low') {
      return 'Low shelf';
    }
    if (type === 'shelf-high') {
      return 'High shelf';
    }
    return 'Peak';
  }

  function buildBandTable(bands) {
    const wrap = document.createElement('div');
    wrap.className = 'table-wrap';
    const table = document.createElement('table');
    const head = document.createElement('tr');
    ['Type', 'Frequency', 'Gain', 'Q'].forEach((title) => {
      const th = cell('th', title);
      th.scope = 'col';
      head.appendChild(th);
    });
    const thead = document.createElement('thead');
    thead.appendChild(head);
    table.appendChild(thead);
    const tbody = document.createElement('tbody');
    bands.forEach((band) => {
      const row = document.createElement('tr');
      row.appendChild(cell('td', bandTypeLabel(band.type)));
      row.appendChild(cell('td', formatNumber(band.frequency, 0, 'Hz')));
      row.appendChild(cell('td', signed(band.gain, 1, 'dB')));
      row.appendChild(cell('td', formatNumber(band.q, 2, '')));
      tbody.appendChild(row);
    });
    table.appendChild(tbody);
    wrap.appendChild(table);
    return wrap;
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

  function addSpectrum(report) {
    spectrumBox.replaceChildren();
    const spectrum = report.spectrum;
    if (!spectrum || !Array.isArray(spectrum.frequencies) || spectrum.frequencies.length < 2 || !Array.isArray(spectrum.input)) {
      spectrumBox.appendChild(cell('p', 'No spectrum is available for this job.'));
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
    const cards = [];
    cards.push(statCard('Loudness', formatNumber(output.integratedLufs, 1, 'LUFS'), 'was ' + formatNumber(input.integratedLufs, 1, 'LUFS'), report.gain.targetReached));
    cards.push(statCard('True peak', formatNumber(output.truePeakDb, 1, 'dBTP'), 'was ' + formatNumber(input.truePeakDb, 1, 'dBTP'), false));
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

  function formatTime(seconds) {
    if (!Number.isFinite(seconds) || seconds < 0) {
      return '0:00';
    }
    const total = Math.floor(seconds);
    return Math.floor(total / 60) + ':' + String(total % 60).padStart(2, '0');
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

  function setPlayerNote(text) {
    playerNote.textContent = text;
  }

  function currentDuration() {
    return player && Number.isFinite(player.mastered.duration) ? player.mastered.duration : 0;
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
      const position = Math.round((player.mastered.currentTime / duration) * width);
      context.fillStyle = styles.getPropertyValue('--text').trim() || '#fff';
      context.fillRect(Math.min(width - 2, position), 0, Math.max(1, Math.round(ratio)), height);
    }
  }

  function seekToFraction(fraction) {
    if (!player) {
      return;
    }
    const duration = currentDuration();
    if (duration > 0) {
      const target = Math.min(Math.max(fraction, 0), 1) * duration;
      player.mastered.currentTime = target;
      player.original.currentTime = target;
    }
    refreshTime();
  }

  function createAudio(source) {
    const audio = document.createElement('audio');
    audio.preload = 'auto';
    audio.src = source;
    return audio;
  }

  function applyLevels() {
    if (!player) {
      return;
    }
    ['original', 'mastered'].forEach((name) => {
      const active = player.active === name;
      const level = active ? (player.match ? player.gains[name] : 1) : 0;
      if (player.nodes) {
        player.nodes[name].gain.setTargetAtTime(level, player.context.currentTime, 0.015);
      } else {
        player[name].muted = !active;
        player[name].volume = Math.min(1, level);
      }
    });
    hearOriginal.setAttribute('aria-pressed', String(player.active === 'original'));
    hearMastered.setAttribute('aria-pressed', String(player.active === 'mastered'));
    drawWave();
  }

  function ensureGraph() {
    if (player.nodes || player.graphFailed) {
      return;
    }
    const Context = window.AudioContext || window.webkitAudioContext;
    if (!Context) {
      player.graphFailed = true;
      return;
    }
    let context = null;
    try {
      context = new Context();
      const nodes = {};
      ['original', 'mastered'].forEach((name) => {
        const source = context.createMediaElementSource(player[name]);
        const gain = context.createGain();
        source.connect(gain);
        gain.connect(context.destination);
        nodes[name] = gain;
      });
      player.context = context;
      player.nodes = nodes;
      ['original', 'mastered'].forEach((name) => {
        player[name].muted = false;
        player[name].volume = 1;
      });
    } catch (error) {
      player.graphFailed = true;
      if (context) {
        context.close().then(ignore, ignore);
      }
    }
  }

  function refreshTime() {
    if (!player) {
      return;
    }
    const clock = player.mastered;
    const duration = currentDuration();
    if (!player.dragging) {
      seekInput.value = duration > 0 ? String(Math.round((clock.currentTime / duration) * 1000)) : '0';
    }
    timeLabel.textContent = formatTime(clock.currentTime) + ' / ' + formatTime(duration);
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
    player.original.pause();
    player.mastered.pause();
    setPlaying(false);
  }

  async function togglePlay() {
    if (!player) {
      return;
    }
    if (player.playing) {
      pausePlayer();
      return;
    }
    ensureGraph();
    if (player.context && player.context.state === 'suspended') {
      try {
        await player.context.resume();
      } catch (error) {
        setPlayerNote('The browser blocked audio playback. Tap Play again.');
        return;
      }
    }
    const duration = currentDuration();
    if (player.mastered.ended || (duration > 0 && player.mastered.currentTime >= duration - 0.05)) {
      player.mastered.currentTime = 0;
    }
    player.original.currentTime = player.mastered.currentTime;
    applyLevels();
    setPlayerNote('');
    try {
      await Promise.all([player.mastered.play(), player.original.play()]);
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

  function onTimeUpdate() {
    if (!player) {
      return;
    }
    if (player.playing && Math.abs(player.original.currentTime - player.mastered.currentTime) > 0.2) {
      player.original.currentTime = player.mastered.currentTime;
    }
    refreshTime();
  }

  function onEnded() {
    if (!player) {
      return;
    }
    player.original.pause();
    setPlaying(false);
    refreshTime();
  }

  function onMediaError() {
    if (player) {
      pausePlayer();
      setPlayerNote('The preview could not be loaded. You can still download the WAV file.');
    }
  }

  function chooseVersion(name) {
    if (!player) {
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
    if (!player) {
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
    if (!player || resultPanel.hidden || event.ctrlKey || event.metaKey || event.altKey) {
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
        seekToFraction((player.mastered.currentTime + step) / duration);
      }
    }
  }

  function setupPlayer(job, report) {
    teardownPlayer();
    if (!job.preview) {
      playerSection.hidden = true;
      return;
    }
    const original = createAudio(job.preview.original);
    const mastered = createAudio(job.preview.mastered);
    player = {
      original,
      mastered,
      active: 'mastered',
      match: matchLevel.checked,
      gains: levelMatchGains(report),
      context: null,
      nodes: null,
      graphFailed: false,
      playing: false,
      dragging: false,
      scrubbing: false,
    };
    mastered.addEventListener('timeupdate', onTimeUpdate);
    mastered.addEventListener('durationchange', refreshTime);
    mastered.addEventListener('loadedmetadata', refreshTime);
    mastered.addEventListener('ended', onEnded);
    mastered.addEventListener('error', onMediaError);
    original.addEventListener('error', onMediaError);
    playerMedia.replaceChildren(original, mastered);
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
    playerSection.hidden = false;
    applyLevels();
    refreshTime();
  }

  function teardownPlayer() {
    waveData = null;
    if (frameRequest !== null) {
      window.cancelAnimationFrame(frameRequest);
      frameRequest = null;
    }
    if (!player) {
      playerSection.hidden = true;
      return;
    }
    const current = player;
    player = null;
    ['original', 'mastered'].forEach((name) => {
      current[name].pause();
      current[name].removeAttribute('src');
      current[name].load();
    });
    if (current.context) {
      current.context.close().then(ignore, ignore);
    }
    playerMedia.replaceChildren();
    playButton.textContent = 'Play';
    seekInput.value = '0';
    timeLabel.textContent = '0:00 / 0:00';
    setPlayerNote('');
    playerSection.hidden = true;
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

  function showResult(job) {
    stopPolling();
    busy = false;
    const report = job.report;
    const output = report.output.measurements;
    heroSummary.textContent =
      formatNumber(output.integratedLufs, 1, 'LUFS') + ' and ' + formatNumber(output.truePeakDb, 1, 'dBTP') + ', ' +
      report.gain.iterations + ' loudness pass' + (report.gain.iterations === 1 ? '' : 'es') + '.';
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
        ' dB on average, working during ' + Math.round(limiter.reducedFraction * 100) + '% of the track.'
      : '';
    warningsList.replaceChildren();
    (report.warnings || []).forEach((warning) => warningsList.appendChild(cell('li', warning)));
    warningsList.hidden = warningsList.children.length === 0;
    buildPlatforms(report);
    addSpectrum(report);
    chartsContainer.replaceChildren();
    addChart('Mid EQ', report.mid);
    if (report.side && Array.isArray(report.side.bands) && report.side.bands.length > 0) {
      addChart('Side EQ', report.side);
    }
    downloadLink.href = job.downloadUrl;
    downloadLink.setAttribute('download', job.downloadName);
    show(resultPanel);
    setupPlayer(job, report);
    waveData = validWaveform(report.waveform) ? report.waveform : null;
    waveCanvas.hidden = waveData === null;
    drawWave();
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

  async function onAgain() {
    const id = jobId;
    backToForm();
    await discardJob(id);
  }

  async function init() {
    setupDropZones();
    aggressivenessInput.addEventListener('input', () => {
      aggressivenessValue.textContent = aggressivenessInput.value + '%';
      clearStyleSelection();
    });
    byId('dynamics').addEventListener('change', clearStyleSelection);
    byId('soft-clip').addEventListener('change', clearStyleSelection);
    styleButtons.forEach((button) => button.addEventListener('click', () => applyStyle(button.dataset.style)));
    [targetInput, ceilingInput].forEach((input) => input.addEventListener('input', refreshPresetChips));
    form.addEventListener('submit', onSubmit);
    cancelButton.addEventListener('click', onCancel);
    retryButton.addEventListener('click', onRetry);
    againButton.addEventListener('click', onAgain);
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
    applyStyle('balanced');
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
    formatsHint.textContent = 'Accepted files: ' + extensions.map((item) => item.slice(1).toUpperCase()).join(', ') + '.';
    limitsText.textContent =
      'Limits: ' + limits.maxFileMb + ' MB and ' + Math.round(limits.maxDurationSec / 60) + ' minutes per file, ' +
      limits.jobsPerHour + ' jobs per hour.';
    buildPresetChips();
    submitButton.disabled = false;
    const saved = readStorage();
    if (saved) {
      jobId = saved;
      busy = true;
      setProgress(null, 'Reconnecting', 'Checking on your earlier job…', 0);
      show(progressPanel);
      poll();
    }
  }

  init();
})();
