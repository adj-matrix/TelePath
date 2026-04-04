(function initTelePathState(global) {
  const elements = {
    singleForm: document.getElementById("benchmarkForm"),
    sweepForm: document.getElementById("sweepForm"),
    workloadSelect: document.getElementById("workload"),
    sweepWorkloadSelect: document.getElementById("sweep_workload"),
    runButton: document.getElementById("runButton"),
    sweepButton: document.getElementById("sweepButton"),
    languageSelect: document.getElementById("languageSelect"),
    healthStatus: document.getElementById("healthStatus"),
    binaryStatus: document.getElementById("binaryStatus"),
    latestThroughput: document.getElementById("latestThroughput"),
    latestHitRate: document.getElementById("latestHitRate"),
    resultEmpty: document.getElementById("resultEmpty"),
    resultContent: document.getElementById("resultContent"),
    sweepEmpty: document.getElementById("sweepEmpty"),
    sweepContent: document.getElementById("sweepContent"),
    blockMapEmpty: document.getElementById("blockMapEmpty"),
    blockMapContent: document.getElementById("blockMapContent"),
    rawJson: document.getElementById("rawJson"),
    recentRuns: document.getElementById("recentRuns"),
    recentSweeps: document.getElementById("recentSweeps"),
    blockMapLegend: document.getElementById("blockMapLegend"),
    blockMapGrid: document.getElementById("blockMapGrid"),
    blockMapMode: document.getElementById("blockMapMode"),
    blockMapBlocks: document.getElementById("blockMapBlocks"),
    blockMapPool: document.getElementById("blockMapPool"),
    blockMapFocus: document.getElementById("blockMapFocus"),
    blockMapDetailTitle: document.getElementById("blockMapDetailTitle"),
    blockMapDetailCategory: document.getElementById("blockMapDetailCategory"),
    blockMapDetailAccesses: document.getElementById("blockMapDetailAccesses"),
    blockMapDetailShare: document.getElementById("blockMapDetailShare"),
    blockMapDetailResidency: document.getElementById("blockMapDetailResidency"),
    blockMapDetailPinCount: document.getElementById("blockMapDetailPinCount"),
    blockMapDetailFlags: document.getElementById("blockMapDetailFlags"),
    blockMapDetailNote: document.getElementById("blockMapDetailNote"),
  };

  const singleFields = [
    "threads",
    "pool_size",
    "block_count",
    "ops_per_thread",
    "hotset_size",
    "hot_access_percent",
  ];

  const state = {
    config: null,
    translations: null,
    currentLanguage: "en",
    lastHealth: null,
    lastSession: null,
    lastSingleRun: null,
    lastSweep: null,
    lastRawPayload: null,
    activeHistoryKey: null,
    blockMapModel: null,
    selectedFrameId: null,
  };

  global.TelePath = {
    ...(global.TelePath || {}),
    elements,
    singleFields,
    state,
  };
})(window);
