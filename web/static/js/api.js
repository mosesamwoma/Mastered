const STORAGE_KEY = 'masteredJob';

export function delay(ms) {
  return new Promise((resolve) => window.setTimeout(resolve, ms));
}

export function redirectToLogin() {
  window.location.assign('/login');
}

export async function request(url, options) {
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

export function parseJson(text) {
  try {
    return JSON.parse(text);
  } catch (error) {
    return null;
  }
}

export function describeError(payload, fallback) {
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

export function readStorage() {
  try {
    return window.sessionStorage.getItem(STORAGE_KEY);
  } catch (error) {
    return null;
  }
}

export function writeStorage(value) {
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

export function ignore() {
  return undefined;
}
