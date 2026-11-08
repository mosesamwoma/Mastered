const ROUNDS = 10;

function chance(count, correct) {
  let total = 0;
  let term = 1;
  const terms = [];
  for (let k = 0; k <= count; k += 1) {
    terms.push(term);
    term = (term * (count - k)) / (k + 1);
  }
  for (let k = correct; k <= count; k += 1) {
    total += terms[k];
  }
  return total / Math.pow(2, count);
}

function randomBit() {
  if (window.crypto && window.crypto.getRandomValues) {
    return window.crypto.getRandomValues(new Uint8Array(1))[0] & 1;
  }
  return Math.random() < 0.5 ? 1 : 0;
}

export function createAbx(api) {
  const byId = (id) => document.getElementById(id);
  const card = byId('abx-card');
  const idle = byId('abx-idle');
  const run = byId('abx-run');
  const roundText = byId('abx-round');
  const resultText = byId('abx-result');
  const player = byId('player');
  let active = false;
  let round = 0;
  let correct = 0;
  let hidden = 'original';

  function nextRound() {
    round += 1;
    hidden = randomBit() === 1 ? 'mastered' : 'original';
    roundText.textContent = 'Round ' + round + ' of ' + ROUNDS;
  }

  function begin() {
    if (!api.ready()) {
      return;
    }
    active = true;
    round = 0;
    correct = 0;
    resultText.textContent = '';
    idle.hidden = true;
    run.hidden = false;
    player.classList.add('player-blind');
    api.setBlind(true);
    nextRound();
  }

  function finish(stopped) {
    const done = round - (stopped ? 1 : 0);
    active = false;
    idle.hidden = false;
    run.hidden = true;
    player.classList.remove('player-blind');
    api.setBlind(false);
    api.pause();
    if (stopped || done < 1) {
      resultText.textContent = 'The test was stopped.';
      return;
    }
    const p = chance(done, correct);
    const verdict =
      p < 0.05
        ? 'That is very unlikely to be luck, so you can really hear the difference.'
        : 'That could easily be luck, so you could not reliably tell them apart at the same volume.';
    resultText.textContent = 'You got ' + correct + ' of ' + done + ' right (chance of doing this well by luck: ' + (p * 100).toFixed(1) + '%). ' + verdict;
  }

  function hear(name) {
    api.choose(name);
    api.play();
  }

  function guess(answer) {
    if (!active) {
      return;
    }
    if (answer === hidden) {
      correct += 1;
    }
    if (round >= ROUNDS) {
      finish(false);
      return;
    }
    nextRound();
  }

  byId('abx-start').addEventListener('click', begin);
  byId('abx-stop').addEventListener('click', () => finish(true));
  byId('abx-play-a').addEventListener('click', () => hear('original'));
  byId('abx-play-b').addEventListener('click', () => hear('mastered'));
  byId('abx-play-x').addEventListener('click', () => hear(hidden));
  byId('abx-guess-a').addEventListener('click', () => guess('original'));
  byId('abx-guess-b').addEventListener('click', () => guess('mastered'));

  return {
    isActive: () => active,
    setAvailable(value) {
      card.hidden = !value;
    },
    reset() {
      if (active) {
        active = false;
        player.classList.remove('player-blind');
        api.setBlind(false);
      }
      idle.hidden = false;
      run.hidden = true;
      resultText.textContent = '';
      card.hidden = true;
    },
  };
}
