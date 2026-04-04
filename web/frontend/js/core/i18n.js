(function initTelePathI18n(global) {
  const app = global.TelePath;

  function formatNumber(value, digits = 0) {
    const locale = app.state.currentLanguage === "zh-CN" ? "zh-CN" : "en-US";
    return new Intl.NumberFormat(locale, {
      maximumFractionDigits: digits,
      minimumFractionDigits: digits,
    }).format(value);
  }

  function formatPercent(value) {
    return `${formatNumber(value * 100, 2)}%`;
  }

  function deepLookup(object, path) {
    return path.split(".").reduce((current, key) => {
      if (current && Object.prototype.hasOwnProperty.call(current, key)) {
        return current[key];
      }
      return undefined;
    }, object);
  }

  function t(key, replacements = {}) {
    const raw = deepLookup(app.state.translations, key);
    const source = typeof raw === "string" ? raw : key;
    return source.replace(/\{(\w+)\}/g, (_, token) => {
      if (Object.prototype.hasOwnProperty.call(replacements, token)) {
        return String(replacements[token]);
      }
      return `{${token}}`;
    });
  }

  function workloadLabel(workload) {
    return t(`workloads.${workload}`);
  }

  function backendLabel(backend) {
    return t(`backends.${backend}`);
  }

  function applyTranslations() {
    document.documentElement.lang = app.state.currentLanguage;
    document.title = t("meta.title");

    for (const element of document.querySelectorAll("[data-i18n]")) {
      element.textContent = t(element.dataset.i18n);
    }

    for (const element of document.querySelectorAll("[data-i18n-placeholder]")) {
      element.setAttribute("placeholder", t(element.dataset.i18nPlaceholder));
    }
  }

  function preferredLanguage() {
    const stored = window.localStorage.getItem("telepath-console-language");
    if (stored) {
      return stored;
    }
    const browserLanguage = navigator.language || "en";
    return browserLanguage.toLowerCase().startsWith("zh") ? "zh-CN" : "en";
  }

  async function loadLanguage(language) {
    const payload = await app.fetchJson(`/api/i18n/${encodeURIComponent(language)}`);
    app.state.translations = payload;
    app.state.currentLanguage = language;
    window.localStorage.setItem("telepath-console-language", language);
    applyTranslations();
    app.populateLanguageOptions();
    if (app.state.config) {
      app.fillDefaults(app.state.config);
    }
    app.refreshAllViews();
  }

  Object.assign(app, {
    formatNumber,
    formatPercent,
    t,
    workloadLabel,
    backendLabel,
    applyTranslations,
    preferredLanguage,
    loadLanguage,
  });
})(window);
