(function initTelePathBlockMap(global) {
  const app = global.TelePath;
  const { elements } = app;

  function frameTone(frame) {
    if (frame.state === "loading" || frame.io_in_flight || frame.flush_in_flight) {
      return "loading";
    }
    if (frame.pin_count > 0) {
      return "pinned";
    }
    if (frame.is_valid && frame.is_dirty) {
      return "dirty";
    }
    if (frame.is_valid) {
      return "resident";
    }
    return "free";
  }

  function frameFlags(frame) {
    const flags = [];
    if (frame.is_dirty) {
      flags.push(app.t("block_map.flags.dirty"));
    }
    if (frame.flush_queued) {
      flags.push(app.t("block_map.flags.flush_queued"));
    }
    if (frame.flush_in_flight) {
      flags.push(app.t("block_map.flags.flush_in_flight"));
    }
    if (frame.io_in_flight) {
      flags.push(app.t("block_map.flags.io_in_flight"));
    }
    if (flags.length === 0) {
      flags.push(app.t("block_map.flags.none"));
    }
    return flags;
  }

  function buildAccessProfile(payload) {
    if (payload?.access_profile && Array.isArray(payload.access_profile.blocks)) {
      const byBlockId = new Map();
      for (const block of payload.access_profile.blocks) {
        byBlockId.set(block.block_id, {
          accesses: Number(block.accesses ?? 0),
          share: Number(block.share ?? 0),
        });
      }
      return {
        byBlockId,
        focus: app.t("block_map.profile.observed"),
        note: app.t("block_map.profile.observed"),
      };
    }

    if (!payload?.request || !payload?.metrics) {
      return {
        byBlockId: new Map(),
        focus: app.t("block_map.profile.none"),
        note: app.t("block_map.profile.expected"),
      };
    }

    const request = payload.request;
    const metrics = payload.metrics;
    const workload = request.workload;
    const blockCount = request.block_count;
    const poolSize = Math.min(request.pool_size, blockCount);
    const totalOps = metrics.total_ops;
    const byBlockId = new Map();
    let focus = app.t("block_map.profile.none");
    let note = app.t("block_map.profile.expected");

    for (let blockId = 0; blockId < blockCount; blockId += 1) {
      byBlockId.set(blockId, {
        accesses: 0,
        share: 0,
      });
    }

    if (workload === "sequential_shared") {
      note = app.t("block_map.profile.exact");
      focus = app.t("block_map.profile.recent_window", { count: poolSize });
      const fullCycles = Math.floor(request.ops_per_thread / blockCount);
      const remainder = request.ops_per_thread % blockCount;
      for (let blockId = 0; blockId < blockCount; blockId += 1) {
        const perThread = fullCycles + (blockId < remainder ? 1 : 0);
        byBlockId.get(blockId).accesses = perThread * request.threads;
      }
    } else if (workload === "sequential_disjoint") {
      note = app.t("block_map.profile.exact");
      focus = app.t("block_map.profile.recent_window", { count: poolSize });
      const fullCycles = Math.floor(totalOps / blockCount);
      const remainder = totalOps % blockCount;
      for (let blockId = 0; blockId < blockCount; blockId += 1) {
        byBlockId.get(blockId).accesses = fullCycles + (blockId < remainder ? 1 : 0);
      }
    } else if (workload === "hotspot") {
      focus = app.t("block_map.profile.hotset", { count: request.hotset_size });
      const hotProb = request.hot_access_percent / 100;
      const hotsetSize = Math.max(0, Math.min(request.hotset_size, blockCount));
      const coldProb = 1 - hotProb;
      const coldPerBlock = blockCount > 0 ? (totalOps * coldProb) / blockCount : 0;
      const hotPerBlock =
        hotsetSize > 0
          ? (totalOps * hotProb) / hotsetSize + coldPerBlock
          : coldPerBlock;
      for (let blockId = 0; blockId < blockCount; blockId += 1) {
        byBlockId.get(blockId).accesses = blockId < hotsetSize ? hotPerBlock : coldPerBlock;
      }
    } else {
      focus = app.t("block_map.profile.uniform");
      const perBlock = blockCount > 0 ? totalOps / blockCount : 0;
      for (let blockId = 0; blockId < blockCount; blockId += 1) {
        byBlockId.get(blockId).accesses = perBlock;
      }
    }

    for (const stats of byBlockId.values()) {
      stats.share = totalOps > 0 ? stats.accesses / totalOps : 0;
    }

    return { byBlockId, focus, note };
  }

  function buildBlockMapModel(payload) {
    const snapshot = payload?.snapshot;
    if (!snapshot || !Array.isArray(snapshot.frames)) {
      return null;
    }

    const accessProfile = buildAccessProfile(payload);
    const frames = snapshot.frames.map((frame) => {
      const access = frame.is_valid
        ? accessProfile.byBlockId.get(frame.block_id) ?? { accesses: 0, share: 0 }
        : { accesses: 0, share: 0 };
      return {
        ...frame,
        tone: frameTone(frame),
        flags: frameFlags(frame),
        accessCount: access.accesses,
        accessShare: access.share,
      };
    });

    const residentCount = frames.filter((frame) => frame.is_valid).length;
    const dirtyCount = frames.filter((frame) => frame.is_dirty).length;
    const pinnedCount = frames.filter((frame) => frame.pin_count > 0).length;

    return {
      poolSize: snapshot.pool_size ?? frames.length,
      pageSize: snapshot.page_size ?? 0,
      frames,
      residentCount,
      dirtyCount,
      pinnedCount,
      profileFocus: accessProfile.focus,
      profileNote: accessProfile.note,
    };
  }

  function renderFrameDetail(frame) {
    if (!frame) {
      return;
    }
    elements.blockMapDetailTitle.textContent = app.t("block_map.detail.title", {
      frame_id: frame.frame_id,
    });
    elements.blockMapDetailCategory.textContent = app.t(
      `block_map.states.${frame.state}`
    );
    elements.blockMapDetailAccesses.textContent = frame.is_valid
      ? app.t("block_map.detail.tag_template", {
          file_id: frame.file_id,
          block_id: frame.block_id,
        })
      : app.t("block_map.detail.free_tag");
    elements.blockMapDetailShare.textContent = frame.is_valid
      ? app.formatNumber(frame.accessCount, 0)
      : "-";
    elements.blockMapDetailResidency.textContent = frame.is_valid
      ? app.formatPercent(frame.accessShare)
      : "-";
    elements.blockMapDetailPinCount.textContent = app.formatNumber(frame.pin_count, 0);
    elements.blockMapDetailFlags.textContent = frame.flags.join(" / ");
    elements.blockMapDetailNote.textContent = frame.is_valid
      ? app.t("block_map.detail.note_resident", {
          mode: app.state.blockMapModel.profileNote,
          generation: app.formatNumber(frame.dirty_generation, 0),
        })
      : app.t("block_map.detail.note_free");
  }

  function selectFrame(frameId) {
    app.state.selectedFrameId = frameId;
    for (const cell of elements.blockMapGrid.querySelectorAll(".block-cell")) {
      cell.classList.toggle("selected", Number(cell.dataset.frameId) === frameId);
    }
    const frame = app.state.blockMapModel?.frames.find(
      (candidate) => candidate.frame_id === frameId
    );
    renderFrameDetail(frame);
  }

  function renderBlockMap(payload) {
    const model = buildBlockMapModel(payload);
    app.state.blockMapModel = model;
    if (!model) {
      elements.blockMapEmpty.classList.remove("hidden");
      elements.blockMapContent.classList.add("hidden");
      return;
    }

    elements.blockMapMode.textContent = app.t("block_map.summary.point_in_time");
    elements.blockMapBlocks.textContent = app.t("block_map.summary.resident_template", {
      resident: app.formatNumber(model.residentCount, 0),
      total: app.formatNumber(model.frames.length, 0),
    });
    elements.blockMapPool.textContent = app.t("block_map.summary.dirty_template", {
      dirty: app.formatNumber(model.dirtyCount, 0),
      total: app.formatNumber(model.frames.length, 0),
    });
    elements.blockMapFocus.textContent = model.profileFocus;

    const legendItems = [
      ["resident", app.t("block_map.tones.resident")],
      ["dirty", app.t("block_map.tones.dirty")],
      ["pinned", app.t("block_map.tones.pinned")],
      ["loading", app.t("block_map.tones.loading")],
      ["free", app.t("block_map.tones.free")],
    ];
    elements.blockMapLegend.innerHTML = legendItems
      .map(
        ([key, label]) =>
          `<span class="blockmap-legend-item"><span class="blockmap-swatch tone-${key}"></span>${label}</span>`
      )
      .join("");

    const maxShare = Math.max(...model.frames.map((frame) => frame.accessShare), 0.00001);
    const columns = Math.ceil(Math.sqrt(model.frames.length || 1));
    elements.blockMapGrid.style.gridTemplateColumns = `repeat(${columns}, minmax(0, 1fr))`;
    elements.blockMapGrid.innerHTML = "";
    for (const frame of model.frames) {
      const cell = document.createElement("button");
      cell.type = "button";
      cell.className = `block-cell tone-${frame.tone}`;
      cell.dataset.frameId = String(frame.frame_id);
      const intensity = frame.is_valid
        ? Math.max(0.34, frame.accessShare / maxShare)
        : 0.22;
      cell.style.opacity = String(intensity);
      cell.title = app.t("block_map.detail.title", { frame_id: frame.frame_id });
      cell.addEventListener("click", () => selectFrame(frame.frame_id));
      elements.blockMapGrid.appendChild(cell);
    }

    elements.blockMapEmpty.classList.add("hidden");
    elements.blockMapContent.classList.remove("hidden");
    selectFrame(
      app.state.selectedFrameId != null &&
        model.frames.some((frame) => frame.frame_id === app.state.selectedFrameId)
        ? app.state.selectedFrameId
        : model.frames[0]?.frame_id ?? 0
    );
  }

  Object.assign(app, {
    buildBlockMapModel,
    renderBlockMap,
  });
})(window);
