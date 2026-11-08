import { cell, formatNumber, formatTime, frequencyLabel, signed } from './format.js';

const SVG_NS = 'http://www.w3.org/2000/svg';

function svgNode(name, attributes) {
  const node = document.createElementNS(SVG_NS, name);
  Object.keys(attributes).forEach((key) => node.setAttribute(key, String(attributes[key])));
  return node;
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

export function buildChart(curve) {
  const peak = Math.max(0, ...curve.targetDb.map(Math.abs), ...curve.responseDb.map(Math.abs));
  const limit = Math.max(6, Math.ceil(peak / 3) * 3);
  const frame = chartFrame(curve.frequencies, limit);
  frame.svg.setAttribute('aria-label', 'EQ curve');
  frame.svg.appendChild(svgNode('path', { class: 'chart-target', d: frame.pathOf(curve.targetDb) }));
  frame.svg.appendChild(svgNode('path', { class: 'chart-fit', d: frame.pathOf(curve.responseDb) }));
  return frame.svg;
}

export function buildSpectrumChart(spectrum) {
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

export function buildBandTable(bands) {
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

export function buildTimelineChart(timeline) {
  const input = Array.isArray(timeline.input) ? timeline.input : [];
  const output = Array.isArray(timeline.output) ? timeline.output : [];
  const count = Math.max(input.length, output.length);
  const values = input.concat(output).filter((value) => typeof value === 'number' && Number.isFinite(value) && value > -70);
  if (count < 3 || values.length < 3 || !(timeline.stepSec > 0)) {
    return null;
  }
  const low = Math.floor((Math.min(...values) - 2) / 3) * 3;
  const high = Math.ceil((Math.max(...values) + 2) / 3) * 3;
  const width = 640;
  const height = 240;
  const left = 50;
  const right = 12;
  const top = 12;
  const bottom = 28;
  const plotWidth = width - left - right;
  const plotHeight = height - top - bottom;
  const xOf = (index) => left + (index / (count - 1)) * plotWidth;
  const yOf = (value) => top + (1 - (Math.max(low, Math.min(high, value)) - low) / (high - low)) * plotHeight;
  const svg = svgNode('svg', { viewBox: '0 0 ' + width + ' ' + height, role: 'img' });
  svg.setAttribute('aria-label', 'Short-term loudness of your track and the master over time');
  for (let value = low; value <= high; value += 3) {
    const y = yOf(value);
    svg.appendChild(svgNode('line', { class: 'chart-axis', x1: left, x2: width - right, y1: y.toFixed(1), y2: y.toFixed(1) }));
    const label = svgNode('text', { class: 'chart-label', x: left - 6, y: (y + 4).toFixed(1), 'text-anchor': 'end' });
    label.textContent = value + '';
    svg.appendChild(label);
  }
  for (let tick = 0; tick <= 4; tick += 1) {
    const index = Math.round((tick / 4) * (count - 1));
    const x = xOf(index);
    const label = svgNode('text', { class: 'chart-label', x: x.toFixed(1), y: height - 8, 'text-anchor': 'middle' });
    label.textContent = formatTime(1.5 + index * timeline.stepSec);
    svg.appendChild(label);
  }
  const pathOf = (series) =>
    series
      .map((value, index) => (index === 0 ? 'M' : 'L') + xOf(index).toFixed(1) + ' ' + yOf(typeof value === 'number' && Number.isFinite(value) ? value : low).toFixed(1))
      .join(' ');
  if (input.length > 1) {
    svg.appendChild(svgNode('path', { class: 'chart-input', d: pathOf(input) }));
  }
  if (output.length > 1) {
    svg.appendChild(svgNode('path', { class: 'chart-output', d: pathOf(output) }));
  }
  return svg;
}
