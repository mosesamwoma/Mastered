'use strict';

(function () {
  const POLL_INTERVAL_MS = 1500;
  const CONFIG_RETRY_MS = 3000;
  const CONFIG_MAX_ATTEMPTS = 40;
  const MAX_POLL_FAILURES = 6;
  const STORAGE_KEY = 'masteredJob';
  const SVG_NS = 'http://www.w3.org/2000/svg';

  const byId = (id) => document.getElementById(id);
  const form = byId('job-form');
  const referenceInput = byId('reference');
  const trackInput = byId('track');
  const targetInput = byId('target');
  const aggressivenessInput = byId('aggressiveness');
  const aggressivenessValue = byId('aggressiveness-value');
  const ceilingInput = byId('ceiling');
  const bandsInput = byId('bands');
  const bitDepthSelect = byId('bit-depth');
  const precisionSelect = byId('precision');
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
  const progressBar = byId('progress-bar');
  const progressMessage = byId('progress-message');
  const cancelButton = byId('cancel');
  const errorPanel = byId('error-panel');
  const errorMessage = byId('error-message');
  const retryButton = byId('retry');
  const resultPanel = byId('result-panel');
  const downloadLink = byId('download');
  const warningsList = byId('warnings');
  const measurementsBody = byId('measurements');
  const gainSummary = byId('gain-summary');
  const chartsContainer = byId('charts');
  const againButton = byId('again');
  const formatsHint = byId('formats-hint');
  const playerSection = byId('player');
  const playerMedia = byId('player-media');
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

  async function request(url, options) {
    try {
      return await fetch(url, options);
    } catch (error) {
      return null;
    }
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

  function setProgress(value, title, message) {
    if (value === null) {
      progressBar.removeAttribute('value');
    } else {
      progressBar.value = value;
    }
    progressTitle.textContent = title;
    progressMessage.textContent = message;
  }

  function formatNumber(value, digits, unit) {
    if (typeof value !== 'number' || !Number.isFinite(value) || value <= -100) {
      return '–';
    }
    return value.toFixed(digits) + (unit ? ' ' + unit : '');
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
          setProgress(percent, 'Uploading', 'Uploading your files… ' + Math.round(percent) + '%');
        }
      });
      xhr.addEventListener('load', () => resolve({ status: xhr.status, body: parseJson(xhr.responseText) }));
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
      setProgress(null, 'Waiting in the queue', position);
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
    setProgress(0, 'Uploading', 'Uploading your files…');
    show(progressPanel);
    const result = await startUpload(buildFormData());
    activeUpload = null;
    if (result.status === -1) {
      backToForm();
      return;
    }
    if (result.status !== 202 || !result.body || typeof result.body.id !== 'string') {
      const fallback = result.status === 0
        ? 'Could not reach the server. Check your connection and try again.'
        : 'The upload failed (HTTP ' + result.status + ').';
      showError(describeError(result.body, fallback));
      return;
    }
    jobId = result.body.id;
    writeStorage(jobId);
    pollFailures = 0;
    setProgress(null, 'Waiting in the queue', 'Upload complete');
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

  function measurementRow(label, values) {
    const row = document.createElement('tr');
    const header = cell('th', label);
    header.scope = 'row';
    row.appendChild(header);
    row.appendChild(cell('td', formatNumber(values.integratedLufs, 1, 'LUFS')));
    row.appendChild(cell('td', formatNumber(values.truePeakDb, 1, 'dBTP')));
    row.appendChild(cell('td', formatNumber(values.loudnessRangeLu, 1, 'LU')));
    row.appendChild(cell('td', formatNumber(values.stereoCorrelation, 2, '')));
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

  function buildChart(curve) {
    const width = 640;
    const height = 260;
    const left = 46;
    const right = 12;
    const top = 12;
    const bottom = 30;
    const plotWidth = width - left - right;
    const plotHeight = height - top - bottom;
    const frequencies = curve.frequencies;
    const logMin = Math.log(frequencies[0]);
    const logMax = Math.log(frequencies[frequencies.length - 1]);
    const peak = Math.max(0, ...curve.targetDb.map(Math.abs), ...curve.responseDb.map(Math.abs));
    const limit = Math.max(6, Math.ceil(peak / 3) * 3);
    const xOf = (frequency) => left + ((Math.log(frequency) - logMin) / (logMax - logMin)) * plotWidth;
    const yOf = (value) => top + (1 - (value + limit) / (2 * limit)) * plotHeight;
    const pathOf = (values) =>
      values.map((value, index) => (index === 0 ? 'M' : 'L') + xOf(frequencies[index]).toFixed(1) + ' ' + yOf(value).toFixed(1)).join(' ');

    const svg = svgNode('svg', { viewBox: '0 0 ' + width + ' ' + height, role: 'img', 'aria-label': 'EQ curve' });
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
    svg.appendChild(svgNode('path', { class: 'chart-target', d: pathOf(curve.targetDb) }));
    svg.appendChild(svgNode('path', { class: 'chart-fit', d: pathOf(curve.responseDb) }));
    return svg;
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
      row.appendChild(cell('td', (band.gain > 0 ? '+' : '') + formatNumber(band.gain, 1, 'dB')));
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
    block.appendChild(cell('h3', title));
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
    const duration = Number.isFinite(clock.duration) ? clock.duration : 0;
    if (!player.dragging) {
      seekInput.value = duration > 0 ? String(Math.round((clock.currentTime / duration) * 1000)) : '0';
    }
    timeLabel.textContent = formatTime(clock.currentTime) + ' / ' + formatTime(duration);
  }

  function setPlaying(playing) {
    player.playing = playing;
    playButton.textContent = playing ? 'Pause' : 'Play';
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
    const duration = Number.isFinite(player.mastered.duration) ? player.mastered.duration : 0;
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

  function onSeek() {
    if (!player) {
      return;
    }
    const duration = Number.isFinite(player.mastered.duration) ? player.mastered.duration : 0;
    if (duration > 0) {
      const target = (Number(seekInput.value) / 1000) * duration;
      player.mastered.currentTime = target;
      player.original.currentTime = target;
    }
    refreshTime();
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
    applyLevels();
    refreshTime();
    playerSection.hidden = false;
  }

  function teardownPlayer() {
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

  function showResult(job) {
    stopPolling();
    busy = false;
    const report = job.report;
    measurementsBody.replaceChildren(
      measurementRow('Your track', report.input.measurements),
      measurementRow('Reference', report.references.measurements),
      measurementRow('Mastered', report.output.measurements)
    );
    const passes = report.gain.iterations;
    gainSummary.textContent =
      'Gain applied: ' + formatNumber(report.gain.makeupDb, 1, 'dB') + ' after ' + passes + ' loudness pass' + (passes === 1 ? '' : 'es') + '.';
    warningsList.replaceChildren();
    (report.warnings || []).forEach((warning) => warningsList.appendChild(cell('li', warning)));
    warningsList.hidden = warningsList.children.length === 0;
    chartsContainer.replaceChildren();
    addChart('Mid EQ', report.mid);
    if (report.side && Array.isArray(report.side.bands) && report.side.bands.length > 0) {
      addChart('Side EQ', report.side);
    }
    downloadLink.href = job.downloadUrl;
    downloadLink.setAttribute('download', job.downloadName);
    setupPlayer(job, report);
    show(resultPanel);
  }

  async function onAgain() {
    const id = jobId;
    backToForm();
    await discardJob(id);
  }

  async function init() {
    aggressivenessInput.addEventListener('input', () => {
      aggressivenessValue.textContent = aggressivenessInput.value + '%';
    });
    form.addEventListener('submit', onSubmit);
    cancelButton.addEventListener('click', onCancel);
    retryButton.addEventListener('click', onRetry);
    againButton.addEventListener('click', onAgain);
    playButton.addEventListener('click', togglePlay);
    seekInput.addEventListener('input', onSeek);
    seekInput.addEventListener('pointerdown', () => {
      if (player) {
        player.dragging = true;
      }
    });
    seekInput.addEventListener('pointerup', () => {
      if (player) {
        player.dragging = false;
      }
    });
    hearOriginal.addEventListener('click', () => chooseVersion('original'));
    hearMastered.addEventListener('click', () => chooseVersion('mastered'));
    matchLevel.addEventListener('change', () => {
      if (player) {
        player.match = matchLevel.checked;
        applyLevels();
      }
    });
    show(form);
    const ready = await loadConfig();
    if (!ready) {
      setStatus('Could not reach the server. Reload the page to try again.');
      return;
    }
    setStatus('');
    const extensions = allowedFormats();
    referenceInput.setAttribute('accept', extensions.join(',') + ',audio/*');
    trackInput.setAttribute('accept', extensions.join(',') + ',audio/*');
    formatsHint.textContent = 'Accepted files: ' + extensions.map((item) => item.slice(1).toUpperCase()).join(', ') + '.';
    limitsText.textContent =
      'Limits: ' + limits.maxFileMb + ' MB and ' + Math.round(limits.maxDurationSec / 60) + ' minutes per file, ' +
      limits.jobsPerHour + ' jobs per hour.';
    submitButton.disabled = false;
    const saved = readStorage();
    if (saved) {
      jobId = saved;
      busy = true;
      setProgress(null, 'Reconnecting', 'Checking on your earlier job…');
      show(progressPanel);
      poll();
    }
  }

  init();
})();
