const singleForm = document.getElementById("benchmarkForm");
const sweepForm = document.getElementById("sweepForm");
const workloadSelect = document.getElementById("workload");
const sweepWorkloadSelect = document.getElementById("sweep_workload");
const runButton = document.getElementById("runButton");
const sweepButton = document.getElementById("sweepButton");
const languageSelect = document.getElementById("languageSelect");
const healthStatus = document.getElementById("healthStatus");
const binaryStatus = document.getElementById("binaryStatus");
const latestThroughput = document.getElementById("latestThroughput");
const latestHitRate = document.getElementById("latestHitRate");
const resultEmpty = document.getElementById("resultEmpty");
const resultContent = document.getElementById("resultContent");
const sweepEmpty = document.getElementById("sweepEmpty");
const sweepContent = document.getElementById("sweepContent");
const rawJson = document.getElementById("rawJson");
const recentRuns = document.getElementById("recentRuns");
const recentSweeps = document.getElementById("recentSweeps");

const singleFields = [
  "threads",
  "pool_size",
  "block_count",
  "ops_per_thread",
  "hotset_size",
  "hot_access_percent",
];

const appState = {
  config: null,
  translations: null,
  currentLanguage: "en",
  lastHealth: null,
  lastSession: null,
  lastSingleRun: null,
  lastSweep: null,
  lastRawPayload: null,
};

function formatNumber(value, digits = 0) {
  const locale = appState.currentLanguage === "zh-CN" ? "zh-CN" : "en-US";
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
  const raw = deepLookup(appState.translations, key);
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
  document.documentElement.lang = appState.currentLanguage;
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
  if (browserLanguage.toLowerCase().startsWith("zh")) {
    return "zh-CN";
  }
  return "en";
}

async function loadLanguage(language) {
  const payload = await fetchJson(`/api/i18n/${encodeURIComponent(language)}`);
  appState.translations = payload;
  appState.currentLanguage = language;
  window.localStorage.setItem("telepath-console-language", language);
  applyTranslations();
  populateLanguageOptions();
  if (appState.config) {
    fillDefaults(appState.config);
  }
  refreshAllViews();
}

function populateLanguageOptions() {
  languageSelect.innerHTML = "";
  for (const language of appState.config.supported_languages) {
    const option = document.createElement("option");
    option.value = language.code;
    option.textContent = t(`languages.${language.code}`);
    languageSelect.appendChild(option);
  }
  languageSelect.value = appState.currentLanguage;
}

function fillDefaults(config) {
  workloadSelect.innerHTML = "";
  sweepWorkloadSelect.innerHTML = "";
  for (const workload of config.workloads) {
    const firstOption = document.createElement("option");
    firstOption.value = workload;
    firstOption.textContent = workloadLabel(workload);
    workloadSelect.appendChild(firstOption);

    const secondOption = document.createElement("option");
    secondOption.value = workload;
    secondOption.textContent = workloadLabel(workload);
    sweepWorkloadSelect.appendChild(secondOption);
  }

  workloadSelect.value = config.defaults.workload;
  sweepWorkloadSelect.value = config.defaults.workload;
  for (const field of singleFields) {
    const input = document.getElementById(field);
    input.value = config.defaults[field];
  }

  document.getElementById("thread_candidates").value =
    config.sweep_defaults.thread_candidates.join(",");
  document.getElementById("sweep_pool_size").value = config.sweep_defaults.pool_size;
  document.getElementById("sweep_block_count").value =
    config.sweep_defaults.block_count;
  document.getElementById("sweep_ops_per_thread").value =
    config.sweep_defaults.ops_per_thread;
  document.getElementById("sweep_hotset_size").value =
    config.sweep_defaults.hotset_size;
  document.getElementById("sweep_hot_access_percent").value =
    config.sweep_defaults.hot_access_percent;
}

function setHealth(status, ready) {
  healthStatus.textContent =
    status === "ok" ? t("status.ready") : t("status.error");
  binaryStatus.textContent = ready
    ? t("status.ready")
    : t("status.build_on_first_run");
}

function setButtonLoading(button, isLoading, idleLabelKey, loadingLabelKey) {
  button.disabled = isLoading;
  button.textContent = isLoading ? t(loadingLabelKey) : t(idleLabelKey);
}

function renderSingleRun(payload) {
  appState.lastSingleRun = payload;
  appState.lastRawPayload = payload;
  const metrics = payload.metrics;
  const total = metrics.buffer_hits + metrics.buffer_misses;
  const hitsRatio = total > 0 ? (metrics.buffer_hits / total) * 100 : 0;
  const missesRatio = total > 0 ? (metrics.buffer_misses / total) * 100 : 0;

  document.getElementById("throughputValue").textContent = formatNumber(
    metrics.throughput_ops_per_sec,
    0
  );
  document.getElementById("hitRateValue").textContent = formatPercent(metrics.hit_rate);
  document.getElementById("hitRateBar").style.width = `${metrics.hit_rate * 100}%`;
  document.getElementById("hitsValue").textContent = formatNumber(metrics.buffer_hits);
  document.getElementById("missesValue").textContent = formatNumber(
    metrics.buffer_misses
  );
  document.getElementById("hitsMeta").textContent = formatNumber(metrics.buffer_hits);
  document.getElementById("missesMeta").textContent = formatNumber(
    metrics.buffer_misses
  );
  document.getElementById("hitsBar").style.width = `${hitsRatio}%`;
  document.getElementById("missesBar").style.width = `${missesRatio}%`;
  document.getElementById("backendValue").textContent = backendLabel(
    metrics.disk_backend
  );
  document.getElementById("workloadValue").textContent = workloadLabel(
    metrics.workload
  );
  document.getElementById("opsValue").textContent = formatNumber(metrics.total_ops);
  document.getElementById("secondsValue").textContent = `${formatNumber(
    metrics.seconds,
    4
  )} ${t("units.seconds_suffix")}`;
  latestThroughput.textContent = formatNumber(metrics.throughput_ops_per_sec, 0);
  latestHitRate.textContent = formatPercent(metrics.hit_rate);
  rawJson.textContent = JSON.stringify(payload, null, 2);

  resultEmpty.classList.add("hidden");
  resultContent.classList.remove("hidden");
}

function renderSweep(payload) {
  appState.lastSweep = payload;
  appState.lastRawPayload = payload;
  const summary = payload.summary;
  const samples = payload.samples;
  document.getElementById("bestThroughputValue").textContent = formatNumber(
    summary.best_throughput_ops_per_sec,
    0
  );
  document.getElementById("bestThroughputThreads").textContent = t(
    "units.threads_template",
    { threads: summary.best_throughput_threads }
  );
  document.getElementById("bestHitRateValue").textContent = formatPercent(
    summary.best_hit_rate
  );
  document.getElementById("bestHitRateThreads").textContent = t(
    "units.threads_template",
    { threads: summary.best_hit_rate_threads }
  );

  const maxThroughput = Math.max(
    ...samples.map((sample) => sample.throughput_ops_per_sec),
    1
  );
  const bars = document.getElementById("sweepBars");
  bars.innerHTML = "";
  for (const sample of samples) {
    const ratio = (sample.throughput_ops_per_sec / maxThroughput) * 100;
    const row = document.createElement("div");
    row.className = "sweep-bar-row";
    row.innerHTML = `
      <div class="sweep-bar-meta">
        <span>${t("units.threads_template", { threads: sample.threads })}</span>
        <span>${formatNumber(sample.throughput_ops_per_sec, 0)} ${t(
          "units.ops_per_sec"
        )}</span>
      </div>
      <div class="bar-track">
        <div class="bar-fill sweep" style="width:${ratio}%"></div>
      </div>
    `;
    bars.appendChild(row);
  }

  const tableBody = document.getElementById("sweepTableBody");
  tableBody.innerHTML = "";
  for (const sample of samples) {
    const row = document.createElement("tr");
    row.innerHTML = `
      <td>${sample.threads}</td>
      <td>${formatNumber(sample.throughput_ops_per_sec, 0)}</td>
      <td>${formatPercent(sample.hit_rate)}</td>
      <td>${formatNumber(sample.buffer_hits)}</td>
      <td>${formatNumber(sample.buffer_misses)}</td>
    `;
    tableBody.appendChild(row);
  }

  rawJson.textContent = JSON.stringify(payload, null, 2);
  sweepEmpty.classList.add("hidden");
  sweepContent.classList.remove("hidden");
}

function createHistoryCard(title, subtitle, detail) {
  const card = document.createElement("article");
  card.className = "history-card";
  card.innerHTML = `
    <p class="history-card-title">${title}</p>
    <p class="history-card-subtitle">${subtitle}</p>
    <p class="history-card-detail">${detail}</p>
  `;
  return card;
}

function renderSession(payload) {
  appState.lastSession = payload;
  const overview = payload.overview;
  latestThroughput.textContent = overview.latest_throughput_ops_per_sec
    ? formatNumber(overview.latest_throughput_ops_per_sec, 0)
    : "-";
  latestHitRate.textContent =
    overview.latest_hit_rate != null ? formatPercent(overview.latest_hit_rate) : "-";

  recentRuns.innerHTML = "";
  for (const run of payload.recent_runs) {
    recentRuns.appendChild(
      createHistoryCard(
        t("history.run_title", {
          workload: workloadLabel(run.request.workload),
          threads: t("units.threads_template", { threads: run.request.threads }),
        }),
        t("history.run_subtitle", {
          pool_size: run.request.pool_size,
          block_count: run.request.block_count,
        }),
        t("history.run_detail", {
          throughput: formatNumber(run.metrics.throughput_ops_per_sec, 0),
          hit_rate: formatPercent(run.metrics.hit_rate),
        })
      )
    );
  }
  if (payload.recent_runs.length === 0) {
    recentRuns.innerHTML = `<p class="history-empty">${t("history.empty_runs")}</p>`;
  }

  recentSweeps.innerHTML = "";
  for (const sweep of payload.recent_sweeps) {
    recentSweeps.appendChild(
      createHistoryCard(
        t("history.sweep_title", {
          workload: workloadLabel(sweep.request.workload),
        }),
        t("history.sweep_subtitle", {
          thread_candidates: sweep.request.thread_candidates.join(", "),
        }),
        t("history.sweep_detail", {
          sample_count: sweep.summary.sample_count,
          throughput: formatNumber(sweep.summary.best_throughput_ops_per_sec, 0),
        })
      )
    );
  }
  if (payload.recent_sweeps.length === 0) {
    recentSweeps.innerHTML = `<p class="history-empty">${t(
      "history.empty_sweeps"
    )}</p>`;
  }
}

function refreshAllViews() {
  if (appState.lastHealth) {
    setHealth(appState.lastHealth.status, appState.lastHealth.benchmark_ready);
  } else {
    healthStatus.textContent = t("status.checking");
    binaryStatus.textContent = t("status.unknown");
  }

  latestThroughput.textContent = appState.lastSession?.overview
    ?.latest_throughput_ops_per_sec
    ? formatNumber(appState.lastSession.overview.latest_throughput_ops_per_sec, 0)
    : "-";
  latestHitRate.textContent =
    appState.lastSession?.overview?.latest_hit_rate != null
      ? formatPercent(appState.lastSession.overview.latest_hit_rate)
      : "-";

  if (appState.lastSession) {
    renderSession(appState.lastSession);
  }
  if (appState.lastSingleRun) {
    renderSingleRun(appState.lastSingleRun);
  }
  if (appState.lastSweep) {
    renderSweep(appState.lastSweep);
  }

  if (appState.lastRawPayload) {
    rawJson.textContent = JSON.stringify(appState.lastRawPayload, null, 2);
  } else {
    rawJson.textContent = "";
  }

  setButtonLoading(runButton, false, "buttons.run_idle", "buttons.run_loading");
  setButtonLoading(
    sweepButton,
    false,
    "buttons.sweep_idle",
    "buttons.sweep_loading"
  );
}

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
  appState.lastSession = session;
  renderSession(session);
}

async function bootstrap() {
  try {
    const config = await fetchJson("/api/config");
    appState.config = config;
    const preferred = preferredLanguage();
    const supported = new Set(
      config.supported_languages.map((language) => language.code)
    );
    const selected = supported.has(preferred)
      ? preferred
      : config.default_language;
    await loadLanguage(selected);
    const [health, session] = await Promise.all([
      fetchJson("/api/health"),
      fetchJson("/api/session"),
    ]);
    appState.lastHealth = health;
    appState.lastSession = session;
    setHealth(health.status, health.benchmark_ready);
    renderSession(session);
  } catch (error) {
    healthStatus.textContent = t("status.error");
    binaryStatus.textContent = t("status.unknown");
    rawJson.textContent = String(error);
  }
}

languageSelect.addEventListener("change", async (event) => {
  const nextLanguage = event.target.value;
  await loadLanguage(nextLanguage);
});

singleForm.addEventListener("submit", async (event) => {
  event.preventDefault();
  const payload = {
    workload: workloadSelect.value,
  };
  for (const field of singleFields) {
    payload[field] = Number(document.getElementById(field).value);
  }

  setButtonLoading(runButton, true, "buttons.run_idle", "buttons.run_loading");
  try {
    const result = await fetchJson("/api/benchmark/run", {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
      },
      body: JSON.stringify(payload),
    });
    renderSingleRun(result);
    await refreshSession();
  } catch (error) {
    resultEmpty.classList.add("hidden");
    resultContent.classList.remove("hidden");
    rawJson.textContent = String(error);
  } finally {
    setButtonLoading(runButton, false, "buttons.run_idle", "buttons.run_loading");
  }
});

sweepForm.addEventListener("submit", async (event) => {
  event.preventDefault();
  const payload = {
    workload: sweepWorkloadSelect.value,
    thread_candidates: document.getElementById("thread_candidates").value,
    pool_size: Number(document.getElementById("sweep_pool_size").value),
    block_count: Number(document.getElementById("sweep_block_count").value),
    ops_per_thread: Number(document.getElementById("sweep_ops_per_thread").value),
    hotset_size: Number(document.getElementById("sweep_hotset_size").value),
    hot_access_percent: Number(
      document.getElementById("sweep_hot_access_percent").value
    ),
  };

  setButtonLoading(
    sweepButton,
    true,
    "buttons.sweep_idle",
    "buttons.sweep_loading"
  );
  try {
    const result = await fetchJson("/api/benchmark/sweep", {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
      },
      body: JSON.stringify(payload),
    });
    renderSweep(result);
    await refreshSession();
  } catch (error) {
    sweepEmpty.classList.add("hidden");
    sweepContent.classList.remove("hidden");
    rawJson.textContent = String(error);
  } finally {
    setButtonLoading(
      sweepButton,
      false,
      "buttons.sweep_idle",
      "buttons.sweep_loading"
    );
  }
});

bootstrap();
