export function formatNumber(value, digits, unit) {
  if (typeof value !== 'number' || !Number.isFinite(value) || value <= -100) {
    return '–';
  }
  return value.toFixed(digits) + (unit ? ' ' + unit : '');
}

export function signed(value, digits, unit) {
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    return '–';
  }
  const rounded = Number(Math.abs(value).toFixed(digits));
  return (rounded === 0 ? '' : value > 0 ? '+' : '−') + rounded.toFixed(digits) + (unit ? ' ' + unit : '');
}

export function formatSize(bytes) {
  return bytes >= 1048576 ? (bytes / 1048576).toFixed(1) + ' MB' : Math.max(1, Math.round(bytes / 1024)) + ' KB';
}

export function describeWait(seconds) {
  if (seconds < 45) {
    return 'less than a minute';
  }
  const minutes = Math.round(seconds / 60);
  return 'about ' + Math.max(1, minutes) + ' minute' + (minutes === 1 ? '' : 's');
}

export function formatTime(seconds) {
  if (!Number.isFinite(seconds) || seconds < 0) {
    return '0:00';
  }
  const total = Math.floor(seconds);
  return Math.floor(total / 60) + ':' + String(total % 60).padStart(2, '0');
}

export function frequencyLabel(value) {
  return value >= 1000 ? value / 1000 + 'k' : String(value);
}

export function cell(tag, text) {
  const node = document.createElement(tag);
  node.textContent = text;
  return node;
}
