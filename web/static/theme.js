(function () {
  try {
    var saved = window.localStorage.getItem('masteredTheme');
    if (saved === 'light' || saved === 'dark') {
      document.documentElement.setAttribute('data-theme', saved);
    }
  } catch (error) {
    return;
  }
})();
