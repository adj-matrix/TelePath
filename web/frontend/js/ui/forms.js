(function initTelePathForms(global) {
  const app = global.TelePath;
  const { elements, singleFields } = app;

  function historyKey(kind, payload) {
    return `${kind}:${payload.timestamp_ms}`;
  }

  function populateLanguageOptions() {
    elements.languageSelect.innerHTML = "";
    for (const language of app.state.config.supported_languages) {
      const option = document.createElement("option");
      option.value = language.code;
      option.textContent = app.t(`languages.${language.code}`);
      elements.languageSelect.appendChild(option);
    }
    elements.languageSelect.value = app.state.currentLanguage;
  }

  function fillDefaults(config) {
    elements.workloadSelect.innerHTML = "";
    elements.sweepWorkloadSelect.innerHTML = "";
    for (const workload of config.workloads) {
      const firstOption = document.createElement("option");
      firstOption.value = workload;
      firstOption.textContent = app.workloadLabel(workload);
      elements.workloadSelect.appendChild(firstOption);

      const secondOption = document.createElement("option");
      secondOption.value = workload;
      secondOption.textContent = app.workloadLabel(workload);
      elements.sweepWorkloadSelect.appendChild(secondOption);
    }

    elements.workloadSelect.value = config.defaults.workload;
    elements.sweepWorkloadSelect.value = config.defaults.workload;
    for (const field of singleFields) {
      document.getElementById(field).value = config.defaults[field];
    }

    document.getElementById("thread_candidates").value =
      config.sweep_defaults.thread_candidates.join(",");
    document.getElementById("sweep_pool_size").value =
      config.sweep_defaults.pool_size;
    document.getElementById("sweep_block_count").value =
      config.sweep_defaults.block_count;
    document.getElementById("sweep_ops_per_thread").value =
      config.sweep_defaults.ops_per_thread;
    document.getElementById("sweep_hotset_size").value =
      config.sweep_defaults.hotset_size;
    document.getElementById("sweep_hot_access_percent").value =
      config.sweep_defaults.hot_access_percent;
  }

  function fillSingleFormFromRequest(request) {
    elements.workloadSelect.value = request.workload;
    for (const field of singleFields) {
      document.getElementById(field).value = request[field];
    }
  }

  function fillSweepFormFromRequest(request) {
    elements.sweepWorkloadSelect.value = request.workload;
    document.getElementById("thread_candidates").value =
      request.thread_candidates.join(",");
    document.getElementById("sweep_pool_size").value = request.pool_size;
    document.getElementById("sweep_block_count").value = request.block_count;
    document.getElementById("sweep_ops_per_thread").value = request.ops_per_thread;
    document.getElementById("sweep_hotset_size").value = request.hotset_size;
    document.getElementById("sweep_hot_access_percent").value =
      request.hot_access_percent;
  }

  function setHealth(status, ready) {
    elements.healthStatus.textContent =
      status === "ok" ? app.t("status.ready") : app.t("status.error");
    elements.binaryStatus.textContent = ready
      ? app.t("status.ready")
      : app.t("status.build_on_first_run");
  }

  function setButtonLoading(button, isLoading, idleLabelKey, loadingLabelKey) {
    button.disabled = isLoading;
    button.classList.toggle("is-loading", isLoading);
    button.setAttribute("aria-busy", isLoading ? "true" : "false");
    button.textContent = isLoading
      ? app.t(loadingLabelKey)
      : app.t(idleLabelKey);
  }

  Object.assign(app, {
    historyKey,
    populateLanguageOptions,
    fillDefaults,
    fillSingleFormFromRequest,
    fillSweepFormFromRequest,
    setHealth,
    setButtonLoading,
  });
})(window);
