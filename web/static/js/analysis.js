const FFT_SIZE = 2048;
const MIN_HZ = 30;
const FLOOR_DB = -105;
const CEIL_DB = -25;
const VECTOR_POINTS = 14000;

function fft(re, im) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i += 1) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) {
      j ^= bit;
    }
    j ^= bit;
    if (i < j) {
      const tr = re[i];
      re[i] = re[j];
      re[j] = tr;
      const ti = im[i];
      im[i] = im[j];
      im[j] = ti;
    }
  }
  for (let size = 2; size <= n; size <<= 1) {
    const angle = (-2 * Math.PI) / size;
    const wr = Math.cos(angle);
    const wi = Math.sin(angle);
    for (let start = 0; start < n; start += size) {
      let cr = 1;
      let ci = 0;
      for (let k = 0; k < size / 2; k += 1) {
        const a = start + k;
        const b = a + size / 2;
        const xr = re[b] * cr - im[b] * ci;
        const xi = re[b] * ci + im[b] * cr;
        re[b] = re[a] - xr;
        im[b] = im[a] - xi;
        re[a] += xr;
        im[a] += xi;
        const next = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = next;
      }
    }
  }
}

function colour(value) {
  const t = Math.max(0, Math.min(1, value));
  const stops = [
    [0, 5, 6, 12],
    [0.25, 58, 24, 120],
    [0.5, 190, 55, 100],
    [0.75, 250, 150, 40],
    [1, 255, 245, 190],
  ];
  for (let i = 1; i < stops.length; i += 1) {
    if (t <= stops[i][0]) {
      const a = stops[i - 1];
      const b = stops[i];
      const f = (t - a[0]) / (b[0] - a[0]);
      return [a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f, a[3] + (b[3] - a[3]) * f];
    }
  }
  return [255, 245, 190];
}

function drawSpectrogram(canvas, buffer) {
  const context = canvas.getContext('2d');
  const width = canvas.width;
  const height = canvas.height;
  const left = buffer.getChannelData(0);
  const right = buffer.numberOfChannels > 1 ? buffer.getChannelData(1) : left;
  const rate = buffer.sampleRate;
  const maxHz = Math.min(20000, rate / 2 - 100);
  const image = context.createImageData(width, height);
  const window_ = new Float64Array(FFT_SIZE);
  for (let i = 0; i < FFT_SIZE; i += 1) {
    window_[i] = 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / (FFT_SIZE - 1));
  }
  const rowBins = new Float64Array(height);
  for (let row = 0; row < height; row += 1) {
    const f = MIN_HZ * Math.pow(maxHz / MIN_HZ, 1 - row / (height - 1));
    rowBins[row] = (f / rate) * FFT_SIZE;
  }
  const re = new Float64Array(FFT_SIZE);
  const im = new Float64Array(FFT_SIZE);
  const span = Math.max(1, buffer.length - FFT_SIZE);
  const norm = FFT_SIZE / 4;
  for (let col = 0; col < width; col += 1) {
    const start = Math.floor((col / Math.max(1, width - 1)) * span);
    for (let i = 0; i < FFT_SIZE; i += 1) {
      const index = Math.min(buffer.length - 1, start + i);
      re[i] = 0.5 * (left[index] + right[index]) * window_[i];
      im[i] = 0;
    }
    fft(re, im);
    for (let row = 0; row < height; row += 1) {
      const position = rowBins[row];
      const lower = Math.floor(position);
      const fraction = position - lower;
      const a = Math.hypot(re[lower], im[lower]);
      const b = Math.hypot(re[lower + 1], im[lower + 1]);
      const magnitude = (a * (1 - fraction) + b * fraction) / norm;
      const db = 20 * Math.log10(magnitude + 1e-9);
      const [r, g, bl] = colour((db - FLOOR_DB) / (CEIL_DB - FLOOR_DB));
      const offset = (row * width + col) * 4;
      image.data[offset] = r;
      image.data[offset + 1] = g;
      image.data[offset + 2] = bl;
      image.data[offset + 3] = 255;
    }
  }
  context.putImageData(image, 0, 0);
}

function drawVectorscope(canvas, buffer, dot) {
  const context = canvas.getContext('2d');
  const size = canvas.width;
  const half = size / 2;
  const left = buffer.getChannelData(0);
  const right = buffer.numberOfChannels > 1 ? buffer.getChannelData(1) : left;
  context.fillStyle = '#05060a';
  context.fillRect(0, 0, size, size);
  context.strokeStyle = 'rgba(255,255,255,0.12)';
  context.lineWidth = 1;
  context.beginPath();
  context.moveTo(half, 0);
  context.lineTo(half, size);
  context.moveTo(0, half);
  context.lineTo(size, half);
  context.moveTo(0, 0);
  context.lineTo(size, size);
  context.moveTo(size, 0);
  context.lineTo(0, size);
  context.stroke();
  context.beginPath();
  context.arc(half, half, half * 0.9, 0, Math.PI * 2);
  context.stroke();
  context.fillStyle = dot;
  const scale = half * 0.9;
  const step = Math.max(1, Math.floor(buffer.length / VECTOR_POINTS));
  for (let i = 0; i < buffer.length; i += step) {
    const l = left[i];
    const r = right[i];
    const x = (l - r) * Math.SQRT1_2;
    const y = (l + r) * Math.SQRT1_2;
    context.globalAlpha = 0.35;
    context.fillRect(half + x * scale, half - y * scale, 1.6, 1.6);
  }
  context.globalAlpha = 1;
  let ll = 0;
  let rr = 0;
  let lr = 0;
  let mid = 0;
  let side = 0;
  for (let i = 0; i < buffer.length; i += 1) {
    const l = left[i];
    const r = right[i];
    ll += l * l;
    rr += r * r;
    lr += l * r;
    const m = (l + r) * 0.5;
    const s = (l - r) * 0.5;
    mid += m * m;
    side += s * s;
  }
  const correlation = ll > 0 && rr > 0 ? lr / Math.sqrt(ll * rr) : 1;
  const sideShare = mid + side > 0 ? (side / (mid + side)) * 100 : 0;
  return { correlation, sideShare };
}

async function decode(url, context) {
  const response = await fetch(url, { credentials: 'same-origin' });
  if (!response.ok) {
    throw new Error('HTTP ' + response.status);
  }
  const bytes = await response.arrayBuffer();
  return new Promise((resolve, reject) => {
    const result = context.decodeAudioData(bytes, resolve, reject);
    if (result && typeof result.then === 'function') {
      result.then(resolve, reject);
    }
  });
}

export async function renderAnalysis(preview, elements) {
  const Offline = window.OfflineAudioContext || window.webkitOfflineAudioContext;
  if (!Offline || !preview) {
    elements.status.textContent = 'This browser cannot draw the analysis.';
    return false;
  }
  elements.status.textContent = 'Drawing the analysis…';
  try {
    const context = new Offline(2, 44100, 44100);
    const original = await decode(preview.original, context);
    const mastered = await decode(preview.mastered, context);
    const dot = getComputedStyle(document.documentElement).getPropertyValue('--accent-2').trim() || '#4cc9f0';
    drawSpectrogram(elements.specOriginal, original);
    drawSpectrogram(elements.specMastered, mastered);
    const a = drawVectorscope(elements.vectorOriginal, original, dot);
    const b = drawVectorscope(elements.vectorMastered, mastered, dot);
    elements.vectorOriginalText.textContent = 'Correlation ' + a.correlation.toFixed(2) + ' · side energy ' + a.sideShare.toFixed(0) + '%';
    elements.vectorMasteredText.textContent = 'Correlation ' + b.correlation.toFixed(2) + ' · side energy ' + b.sideShare.toFixed(0) + '%';
    elements.status.textContent = 'Drawn from the preview versions that you hear in the player.';
    return true;
  } catch (error) {
    elements.status.textContent = 'The analysis could not be drawn in this browser.';
    return false;
  }
}
