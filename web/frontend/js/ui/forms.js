(function initTelePathForms(global) {
  const app = global.TelePath;
  const { elements, singleFields, sweepFields } = app;

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
    elements.replacerSelect.innerHTML = "";
    elements.sweepReplacerSelect.innerHTML = "";
    elements.diskBackendSelect.innerHTML = "";
    elements.sweepDiskBackendSelect.innerHTML = "";
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
    for (const replacer of config.replacers) {
      const firstOption = document.createElement("option");
      firstOption.value = replacer;
      firstOption.textContent = app.replacerLabel(replacer);
      elements.replacerSelect.appendChild(firstOption);

      const secondOption = document.createElement("option");
      secondOption.value = replacer;
      secondOption.textContent = app.replacerLabel(replacer);
      elements.sweepReplacerSelect.appendChild(secondOption);
    }
    for (const backend of config.disk_backends) {
      const firstOption = document.createElement("option");
      firstOption.value = backend;
      firstOption.textContent = app.backendLabel(backend);
      elements.diskBackendSelect.appendChild(firstOption);

      const secondOption = document.createElement("option");
      secondOption.value = backend;
      secondOption.textContent = app.backendLabel(backend);
      elements.sweepDiskBackendSelect.appendChild(secondOption);
    }

    elements.workloadSelect.value = config.defaults.workload;
    elements.sweepWorkloadSelect.value = config.defaults.workload;
    elements.replacerSelect.value = config.defaults.replacer;
    elements.sweepReplacerSelect.value = config.sweep_defaults.replacer;
    elements.diskBackendSelect.value = config.defaults.disk_backend;
    elements.sweepDiskBackendSelect.value = config.sweep_defaults.disk_backend;
    for (const field of singleFields) {
      document.getElementById(field).value = config.defaults[field];
    }
    document.getElementById("background_cleaner").checked =
      config.defaults.background_cleaner;

    document.getElementById("thread_candidates").value =
      config.sweep_defaults.thread_candidates.join(",");
    for (const field of sweepFields) {
      document.getElementById(`sweep_${field}`).value =
        config.sweep_defaults[field];
    }
    document.getElementById("sweep_background_cleaner").checked =
      config.sweep_defaults.background_cleaner;
  }

  function fillSingleFormFromRequest(request) {
    elements.workloadSelect.value = request.workload;
    elements.replacerSelect.value = request.replacer;
    elements.diskBackendSelect.value = request.disk_backend;
    for (const field of singleFields) {
      document.getElementById(field).value = request[field];
    }
    document.getElementById("background_cleaner").checked =
      request.background_cleaner;
  }

  function fillSweepFormFromRequest(request) {
    elements.sweepWorkloadSelect.value = request.workload;
    elements.sweepReplacerSelect.value = request.replacer;
    elements.sweepDiskBackendSelect.value = request.disk_backend;
    document.getElementById("thread_candidates").value =
      request.thread_candidates.join(",");
    for (const field of sweepFields) {
      document.getElementById(`sweep_${field}`).value = request[field];
    }
    document.getElementById("sweep_background_cleaner").checked =
      request.background_cleaner;
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
