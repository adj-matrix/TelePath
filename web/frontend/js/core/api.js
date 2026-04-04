(function initTelePathApi(global) {
  const app = global.TelePath;

  async function fetchJson(url, options = {}) {
    const response = await fetch(url, options);
    const payload = await response.json();
    if (!response.ok) {
      throw new Error(payload.error || `request failed: ${response.status}`);
    }
    return payload;
  }

  async function refreshSession() {
    const session = await fetchJson("/api/session");
    app.state.lastSession = session;
    app.renderSession(session);
  }

  Object.assign(app, {
    fetchJson,
    refreshSession,
  });
})(window);
