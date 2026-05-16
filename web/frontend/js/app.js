(function initTelePathApp(global) {
  const app = global.TelePath;
  const { elements, singleFields, sweepFields } = app;

  async function bootstrap() {
    try {
      const config = await app.fetchJson("/api/config");
      app.state.config = config;
      const preferred = app.preferredLanguage();
      const supported = new Set(
        config.supported_languages.map((language) => language.code)
      );
      const selected = supported.has(preferred)
        ? preferred
        : config.default_language;
      await app.loadLanguage(selected);
      const [health, session] = await Promise.all([
        app.fetchJson("/api/health"),
        app.fetchJson("/api/session"),
      ]);
      app.state.lastHealth = health;
      app.state.lastSession = session;
      app.setHealth(health.status, health.benchmark_ready);
      app.renderSession(session);
    } catch (error) {
      elements.healthStatus.textContent = app.t("status.error");
      elements.binaryStatus.textContent = app.t("status.unknown");
      elements.rawJson.textContent = String(error);
    }
  }

  elements.languageSelect.addEventListener("change", async (event) => {
    await app.loadLanguage(event.target.value);
  });

  elements.singleForm.addEventListener("submit", async (event) => {
    event.preventDefault();
    const payload = {
      workload: elements.workloadSelect.value,
      replacer: elements.replacerSelect.value,
      disk_backend: elements.diskBackendSelect.value,
      background_cleaner: document.getElementById("background_cleaner").checked,
    };
    for (const field of singleFields) {
      payload[field] = Number(document.getElementById(field).value);
    }

    app.setButtonLoading(
      elements.runButton,
      true,
      "buttons.run_idle",
      "buttons.run_loading"
    );
    try {
      const result = await app.fetchJson("/api/benchmark/run", {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
        },
        body: JSON.stringify(payload),
      });
      app.renderSingleRun(result);
      await app.refreshSession();
    } catch (error) {
      elements.resultEmpty.classList.add("hidden");
      elements.resultContent.classList.remove("hidden");
      elements.rawJson.textContent = String(error);
    } finally {
      app.setButtonLoading(
        elements.runButton,
        false,
        "buttons.run_idle",
        "buttons.run_loading"
      );
    }
  });

  elements.sweepForm.addEventListener("submit", async (event) => {
    event.preventDefault();
    const payload = {
      workload: elements.sweepWorkloadSelect.value,
      replacer: elements.sweepReplacerSelect.value,
      disk_backend: elements.sweepDiskBackendSelect.value,
      thread_candidates: document.getElementById("thread_candidates").value,
      background_cleaner: document.getElementById("sweep_background_cleaner").checked,
    };
    for (const field of sweepFields) {
      payload[field] = Number(document.getElementById(`sweep_${field}`).value);
    }

    app.setButtonLoading(
      elements.sweepButton,
      true,
      "buttons.sweep_idle",
      "buttons.sweep_loading"
    );
    try {
      const result = await app.fetchJson("/api/benchmark/sweep", {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
        },
        body: JSON.stringify(payload),
      });
      app.renderSweep(result);
      await app.refreshSession();
    } catch (error) {
      elements.sweepEmpty.classList.add("hidden");
      elements.sweepContent.classList.remove("hidden");
      elements.rawJson.textContent = String(error);
    } finally {
      app.setButtonLoading(
        elements.sweepButton,
        false,
        "buttons.sweep_idle",
        "buttons.sweep_loading"
      );
    }
  });

  bootstrap();
})(window);
