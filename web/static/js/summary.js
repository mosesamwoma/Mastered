import { formatNumber, signed } from './format.js';

export function describeChanges(changes) {
  if (!changes || typeof changes !== 'object') {
    return [];
  }
  const lines = [];
  if (typeof changes.loudnessChangeLu === 'number') {
    lines.push('Loudness changed by ' + signed(changes.loudnessChangeLu, 1, 'LU') + ' and the true peak by ' + signed(changes.truePeakChangeDb, 1, 'dB') + '.');
  }
  if (changes.bands > 0) {
    lines.push(
      'EQ: ' + changes.bands + ' band' + (changes.bands === 1 ? '' : 's') + ', boosting up to ' + signed(changes.midBoostDb, 1, 'dB') +
        ' and cutting up to ' + signed(changes.midCutDb, 1, 'dB') + ' in the mid channel.'
    );
  } else {
    lines.push('EQ: no correction was needed.');
  }
  if (Math.abs(changes.widthChangeDb) >= 0.05) {
    lines.push('Stereo width changed by ' + signed(changes.widthChangeDb, 1, 'dB') + '.');
  }
  if (changes.monoBassHz > 0) {
    lines.push('Bass below ' + formatNumber(changes.monoBassHz, 0, 'Hz') + ' was made mono.');
  }
  if (changes.dynamicsApplied) {
    lines.push('Multiband compression was applied to match the dynamics of the reference.');
  }
  if (changes.softClipApplied) {
    lines.push('The soft clipper rounded the tallest peaks before the limiter.');
  }
  if (changes.limiterMaxReductionDb > 0.05) {
    lines.push('The limiter reduced peaks by up to ' + formatNumber(changes.limiterMaxReductionDb, 1, 'dB') + ' (' + formatNumber(changes.limiterAverageReductionDb, 2, 'dB') + ' on average).');
  } else {
    lines.push('The limiter did not need to reduce any peaks.');
  }
  return lines;
}

export function conversionNotes(report) {
  const conversion = report && report.conversion;
  if (!conversion || typeof conversion !== 'object') {
    return [];
  }
  const lines = [];
  const keys = Object.keys(conversion)
    .filter((key) => key !== 'track')
    .sort()
    .concat(['track']);
  keys.forEach((key) => {
    const label = key === 'track' ? 'Your track' : key === 'reference' ? 'Reference' : 'Reference ' + key.slice('reference'.length);
    (Array.isArray(conversion[key]) ? conversion[key] : []).forEach((text) => lines.push(label + ': ' + text + '.'));
  });
  return lines;
}
