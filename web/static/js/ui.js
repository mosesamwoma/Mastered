const THEME_KEY = 'masteredTheme';
const TOAST_MS = 4200;

export function toast(text, kind) {
  const box = document.getElementById('toasts');
  if (!box || !text) {
    return;
  }
  const item = document.createElement('div');
  item.className = 'toast' + (kind ? ' ' + kind : '');
  item.textContent = text;
  box.appendChild(item);
  while (box.children.length > 3) {
    box.removeChild(box.firstChild);
  }
  window.setTimeout(() => {
    if (item.parentNode === box) {
      box.removeChild(item);
    }
  }, TOAST_MS);
}

function currentTheme() {
  const set = document.documentElement.getAttribute('data-theme');
  if (set === 'light' || set === 'dark') {
    return set;
  }
  return window.matchMedia && window.matchMedia('(prefers-color-scheme: light)').matches ? 'light' : 'dark';
}

export function initTheme(button) {
  if (!button) {
    return;
  }
  button.addEventListener('click', () => {
    const next = currentTheme() === 'dark' ? 'light' : 'dark';
    document.documentElement.setAttribute('data-theme', next);
    try {
      window.localStorage.setItem(THEME_KEY, next);
    } catch (error) {
      return;
    }
    window.dispatchEvent(new Event('resize'));
  });
}

export function initTabs(onShow) {
  const tabs = Array.from(document.querySelectorAll('.tab'));
  const panels = Array.from(document.querySelectorAll('.tabpanel'));
  function select(name, focus) {
    tabs.forEach((tab) => {
      const on = tab.dataset.tab === name;
      tab.setAttribute('aria-selected', String(on));
      tab.tabIndex = on ? 0 : -1;
      if (on && focus) {
        tab.focus();
      }
    });
    panels.forEach((panel) => {
      panel.hidden = panel.dataset.panel !== name;
    });
    window.dispatchEvent(new Event('resize'));
    if (onShow) {
      onShow(name);
    }
  }
  tabs.forEach((tab, index) => {
    tab.addEventListener('click', () => select(tab.dataset.tab, false));
    tab.addEventListener('keydown', (event) => {
      let target = -1;
      if (event.key === 'ArrowRight') {
        target = (index + 1) % tabs.length;
      } else if (event.key === 'ArrowLeft') {
        target = (index + tabs.length - 1) % tabs.length;
      } else if (event.key === 'Home') {
        target = 0;
      } else if (event.key === 'End') {
        target = tabs.length - 1;
      }
      if (target >= 0) {
        event.preventDefault();
        select(tabs[target].dataset.tab, true);
      }
    });
  });
  return { select: (name) => select(name, false) };
}
