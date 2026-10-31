'use strict';

(function () {
  const form = document.getElementById('login-form');
  const usernameInput = document.getElementById('username');
  const passwordInput = document.getElementById('password');
  const message = document.getElementById('login-message');
  const submitButton = document.getElementById('login-submit');

  function setMessage(text) {
    message.textContent = text;
    message.hidden = text === '';
  }

  async function request(url, options) {
    try {
      return await fetch(url, options);
    } catch (error) {
      return null;
    }
  }

  async function readJson(response) {
    try {
      return await response.json();
    } catch (error) {
      return null;
    }
  }

  function describe(body, status) {
    if (body && typeof body.detail === 'string') {
      return body.detail;
    }
    if (body && Array.isArray(body.detail) && body.detail.length > 0) {
      return 'Check your username and password.';
    }
    return 'Sign-in failed (HTTP ' + status + ').';
  }

  async function onSubmit(event) {
    event.preventDefault();
    const username = usernameInput.value.trim();
    const password = passwordInput.value;
    if (username === '' || password === '') {
      setMessage('Enter your username and password.');
      return;
    }
    submitButton.disabled = true;
    setMessage('');
    const response = await request('/api/login', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ username: username, password: password }),
    });
    if (response !== null && response.ok) {
      window.location.assign('/');
      return;
    }
    if (response === null) {
      setMessage('Could not reach the server. Please try again.');
    } else {
      setMessage(describe(await readJson(response), response.status));
    }
    passwordInput.value = '';
    submitButton.disabled = false;
    passwordInput.focus();
  }

  async function init() {
    form.addEventListener('submit', onSubmit);
    const response = await request('/api/session', { cache: 'no-store' });
    if (response !== null && response.ok) {
      const session = await readJson(response);
      if (session && (!session.authRequired || session.user)) {
        window.location.assign('/');
        return;
      }
    }
    usernameInput.focus();
  }

  init();
})();
