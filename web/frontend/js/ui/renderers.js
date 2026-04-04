(function initTelePathRenderers(global) {
  const app = global.TelePath;
  const { elements } = app;

  function renderSingleRun(payload) {
    app.state.lastSingleRun = payload;
    app.state.lastRawPayload = payload;
    app.state.activeHistoryKey = app.historyKey("run", payload);
    const metrics = payload.metrics;
    const total = metrics.buffer_hits + metrics.buffer_misses;
    const hitsRatio = total > 0 ? (metrics.buffer_hits / total) * 100 : 0;
    const missesRatio = total > 0 ? (metrics.buffer_misses / total) * 100 : 0;

    document.getElementById("throughputValue").textContent = app.formatNumber(
      metrics.throughput_ops_per_sec,
      0
    );
    document.getElementById("hitRateValue").textContent = app.formatPercent(
      metrics.hit_rate
    );
    document.getElementById("hitRateBar").style.width = `${metrics.hit_rate * 100}%`;
    document.getElementById("hitsValue").textContent = app.formatNumber(
      metrics.buffer_hits
    );
    document.getElementById("missesValue").textContent = app.formatNumber(
      metrics.buffer_misses
    );
    document.getElementById("hitsMeta").textContent = app.formatNumber(
      metrics.buffer_hits
    );
    document.getElementById("missesMeta").textContent = app.formatNumber(
      metrics.buffer_misses
    );
    document.getElementById("hitsBar").style.width = `${hitsRatio}%`;
    document.getElementById("missesBar").style.width = `${missesRatio}%`;
    document.getElementById("backendValue").textContent = app.backendLabel(
      metrics.disk_backend
    );
    document.getElementById("workloadValue").textContent = app.workloadLabel(
      metrics.workload
    );
    document.getElementById("opsValue").textContent = app.formatNumber(
      metrics.total_ops
    );
    document.getElementById("secondsValue").textContent = `${app.formatNumber(
      metrics.seconds,
      4
    )} ${app.t("units.seconds_suffix")}`;
    elements.latestThroughput.textContent = app.formatNumber(
      metrics.throughput_ops_per_sec,
      0
    );
    elements.latestHitRate.textContent = app.formatPercent(metrics.hit_rate);
    elements.rawJson.textContent = JSON.stringify(payload, null, 2);
    app.renderBlockMap(payload);

    elements.resultEmpty.classList.add("hidden");
    elements.resultContent.classList.remove("hidden");
  }

  function renderSweep(payload) {
    app.state.lastSweep = payload;
    app.state.lastRawPayload = payload;
    app.state.activeHistoryKey = app.historyKey("sweep", payload);
    const summary = payload.summary;
    const samples = payload.samples;
    const baseline = samples.reduce(
      (best, sample) =>
        best == null || sample.threads < best.threads ? sample : best,
      null
    );

    document.getElementById("bestThroughputValue").textContent = app.formatNumber(
      summary.best_throughput_ops_per_sec,
      0
    );
    document.getElementById("bestThroughputThreads").textContent = app.t(
      "units.threads_template",
      { threads: summary.best_throughput_threads }
    );
    document.getElementById("bestHitRateValue").textContent = app.formatPercent(
      summary.best_hit_rate
    );
    document.getElementById("bestHitRateThreads").textContent = app.t(
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
      const badges = [];
      if (sample.threads === summary.best_throughput_threads) {
        badges.push(app.t("sweep.badges.best_throughput"));
      }
      if (sample.threads === summary.best_hit_rate_threads) {
        badges.push(app.t("sweep.badges.best_hit_rate"));
      }
      const row = document.createElement("div");
      row.className = "sweep-bar-row";
      row.innerHTML = `
        <div class="sweep-bar-meta">
          <span>${app.t("units.threads_template", { threads: sample.threads })}</span>
          <span>${app.formatNumber(sample.throughput_ops_per_sec, 0)} ${app.t(
            "units.ops_per_sec"
          )}${badges.length > 0 ? ` · ${badges.join(" / ")}` : ""}</span>
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
      const badges = [];
      if (sample.threads === summary.best_throughput_threads) {
        badges.push(app.t("sweep.badges.best_throughput"));
      }
      if (sample.threads === summary.best_hit_rate_threads) {
        badges.push(app.t("sweep.badges.best_hit_rate"));
      }
      const scale =
        baseline && baseline.throughput_ops_per_sec > 0
          ? sample.throughput_ops_per_sec / baseline.throughput_ops_per_sec
          : 0;
      const row = document.createElement("tr");
      if (badges.length > 0) {
        row.classList.add("best-row");
      }
      row.innerHTML = `
        <td>${sample.threads}</td>
        <td>${app.formatNumber(sample.throughput_ops_per_sec, 0)}</td>
        <td>${app.formatNumber(scale, 2)}x</td>
        <td>${app.formatPercent(sample.hit_rate)}</td>
        <td>${app.formatNumber(sample.buffer_hits)}</td>
        <td>${app.formatNumber(sample.buffer_misses)}</td>
        <td>${badges.join(" / ") || "-"}</td>
      `;
      tableBody.appendChild(row);
    }

    elements.rawJson.textContent = JSON.stringify(payload, null, 2);
    elements.sweepEmpty.classList.add("hidden");
    elements.sweepContent.classList.remove("hidden");
  }

  function createHistoryCard(kind, payload, title, subtitle, detail) {
    const card = document.createElement("button");
    const key = app.historyKey(kind, payload);
    card.className = "history-card";
    card.type = "button";
    if (app.state.activeHistoryKey === key) {
      card.classList.add("selected");
    }
    card.innerHTML = `
      <p class="history-card-title">${title}</p>
      <p class="history-card-subtitle">${subtitle}</p>
      <p class="history-card-detail">${detail}</p>
      <p class="history-card-hint">${app.t("history.click_hint")}</p>
    `;
    card.addEventListener("click", () => {
      if (kind === "run") {
        app.fillSingleFormFromRequest(payload.request);
        renderSingleRun(payload);
      } else {
        app.fillSweepFormFromRequest(payload.request);
        renderSweep(payload);
      }
      renderSession(app.state.lastSession);
    });
    return card;
  }

  function renderSession(payload) {
    app.state.lastSession = payload;
    const overview = payload.overview;
    elements.latestThroughput.textContent = overview.latest_throughput_ops_per_sec
      ? app.formatNumber(overview.latest_throughput_ops_per_sec, 0)
      : "-";
    elements.latestHitRate.textContent =
      overview.latest_hit_rate != null
        ? app.formatPercent(overview.latest_hit_rate)
        : "-";

    elements.recentRuns.innerHTML = "";
    for (const run of payload.recent_runs) {
      elements.recentRuns.appendChild(
        createHistoryCard(
          "run",
          run,
          app.t("history.run_title", {
            workload: app.workloadLabel(run.request.workload),
            threads: app.t("units.threads_template", {
              threads: run.request.threads,
            }),
          }),
          app.t("history.run_subtitle", {
            pool_size: run.request.pool_size,
            block_count: run.request.block_count,
          }),
          app.t("history.run_detail", {
            throughput: app.formatNumber(run.metrics.throughput_ops_per_sec, 0),
            hit_rate: app.formatPercent(run.metrics.hit_rate),
          })
        )
      );
    }
    if (payload.recent_runs.length === 0) {
      elements.recentRuns.innerHTML = `<p class="history-empty">${app.t(
        "history.empty_runs"
      )}</p>`;
    }

    elements.recentSweeps.innerHTML = "";
    for (const sweep of payload.recent_sweeps) {
      elements.recentSweeps.appendChild(
        createHistoryCard(
          "sweep",
          sweep,
          app.t("history.sweep_title", {
            workload: app.workloadLabel(sweep.request.workload),
          }),
          app.t("history.sweep_subtitle", {
            thread_candidates: sweep.request.thread_candidates.join(", "),
          }),
          app.t("history.sweep_detail", {
            sample_count: sweep.summary.sample_count,
            throughput: app.formatNumber(
              sweep.summary.best_throughput_ops_per_sec,
              0
            ),
          })
        )
      );
    }
    if (payload.recent_sweeps.length === 0) {
      elements.recentSweeps.innerHTML = `<p class="history-empty">${app.t(
        "history.empty_sweeps"
      )}</p>`;
    }
  }

  function refreshAllViews() {
    if (app.state.lastHealth) {
      app.setHealth(
        app.state.lastHealth.status,
        app.state.lastHealth.benchmark_ready
      );
    } else {
      elements.healthStatus.textContent = app.t("status.checking");
      elements.binaryStatus.textContent = app.t("status.unknown");
    }

    elements.latestThroughput.textContent = app.state.lastSession?.overview
      ?.latest_throughput_ops_per_sec
      ? app.formatNumber(
          app.state.lastSession.overview.latest_throughput_ops_per_sec,
          0
        )
      : "-";
    elements.latestHitRate.textContent =
      app.state.lastSession?.overview?.latest_hit_rate != null
        ? app.formatPercent(app.state.lastSession.overview.latest_hit_rate)
        : "-";

    if (app.state.lastSession) {
      renderSession(app.state.lastSession);
    }
    if (app.state.lastSingleRun) {
      renderSingleRun(app.state.lastSingleRun);
    }
    if (app.state.lastSweep) {
      renderSweep(app.state.lastSweep);
    }
    if (app.state.lastSingleRun) {
      app.renderBlockMap(app.state.lastSingleRun);
    } else {
      elements.blockMapEmpty.classList.remove("hidden");
      elements.blockMapContent.classList.add("hidden");
    }

    elements.rawJson.textContent = app.state.lastRawPayload
      ? JSON.stringify(app.state.lastRawPayload, null, 2)
      : "";

    app.setButtonLoading(
      elements.runButton,
      false,
      "buttons.run_idle",
      "buttons.run_loading"
    );
    app.setButtonLoading(
      elements.sweepButton,
      false,
      "buttons.sweep_idle",
      "buttons.sweep_loading"
    );
  }

  Object.assign(app, {
    renderSingleRun,
    renderSweep,
    renderSession,
    refreshAllViews,
  });
})(window);
