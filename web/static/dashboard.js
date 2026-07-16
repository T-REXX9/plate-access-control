(() => {
  "use strict";

  const POLL_INTERVAL_MS = 2000;
  let timer = null;
  let syncing = false;

  const byId = (id) => document.getElementById(id);
  const text = (id, value) => {
    const element = byId(id);
    if (element) element.textContent = value;
  };
  const titleCase = (value) => {
    const clean = String(value || "");
    return clean ? clean.charAt(0).toUpperCase() + clean.slice(1) : "";
  };
  const indicator = (id, state) => {
    const element = byId(id);
    if (element) element.className = `indicator ${state}`;
  };
  const statusBadge = (value) => {
    const badge = document.createElement("span");
    badge.className = `status ${String(value || "").toLowerCase()}`;
    badge.textContent = value || "—";
    return badge;
  };

  function updateCameraControl(running) {
    const form = byId("camera-control-form");
    const button = byId("camera-control-button");
    if (!form || !button) return;
    form.action = running ? form.dataset.stopUrl : form.dataset.startUrl;
    button.className = `camera-toggle ${running ? "stop" : "start"}`;
    button.textContent = running ? "Stop recognition" : "Start recognition";
  }

  function updateLatest(event) {
    const frame = byId("latest-photo-frame");
    const image = byId("latest-photo");
    const details = byId("latest-photo-details");
    const placeholder = byId("latest-photo-placeholder");
    const decision = byId("latest-decision");
    if (!frame || !image || !details || !placeholder || !decision) return;

    if (!event) {
      frame.hidden = true;
      details.hidden = true;
      decision.hidden = true;
      placeholder.hidden = false;
      return;
    }

    const eventId = String(event.id);
    if (frame.dataset.eventId !== eventId) {
      frame.dataset.eventId = eventId;
      image.src = `${event.image_url}?v=${encodeURIComponent(eventId)}`;
    }
    image.alt = `Annotated capture of plate ${event.plate_number}`;
    text("latest-plate", event.plate_number);
    text("latest-owner", event.owner_name || "Unregistered vehicle");
    text("latest-time", event.local_time);
    decision.className = `status ${String(event.decision || "").toLowerCase()}`;
    decision.textContent = event.decision;
    decision.hidden = false;
    frame.hidden = false;
    details.hidden = false;
    placeholder.hidden = true;
  }

  function updateRecent(events) {
    const body = byId("recent-events-body");
    if (!body) return;
    body.replaceChildren();

    if (!events.length) {
      const row = document.createElement("tr");
      const cell = document.createElement("td");
      const empty = document.createElement("div");
      const heading = document.createElement("strong");
      const note = document.createElement("span");
      cell.colSpan = 5;
      empty.className = "empty-state";
      heading.textContent = "No access events yet";
      note.textContent = "Recognition events will appear here automatically.";
      empty.append(heading, note);
      cell.append(empty);
      row.append(cell);
      body.append(row);
      return;
    }

    events.forEach((event) => {
      const row = document.createElement("tr");
      const timeCell = document.createElement("td");
      const plateCell = document.createElement("td");
      const ownerCell = document.createElement("td");
      const vehicleCell = document.createElement("td");
      const decisionCell = document.createElement("td");
      const plate = document.createElement("span");
      timeCell.className = "nowrap";
      timeCell.textContent = event.local_time;
      plate.className = "plate";
      plate.textContent = event.plate_number;
      plateCell.append(plate);
      ownerCell.textContent = event.owner_name || "Unknown vehicle";
      vehicleCell.textContent = event.vehicle || "—";
      decisionCell.append(statusBadge(event.decision));
      row.append(timeCell, plateCell, ownerCell, vehicleCell, decisionCell);
      body.append(row);
    });
  }

  function updateDaily(days) {
    const grid = byId("daily-activity");
    if (!grid) return;
    grid.replaceChildren();
    if (!days.length) {
      const empty = document.createElement("p");
      empty.className = "muted";
      empty.textContent = "Daily summaries will appear after the first recognition event.";
      grid.append(empty);
      return;
    }
    days.forEach((day) => {
      const card = document.createElement("div");
      const date = document.createElement("strong");
      const total = document.createElement("span");
      const detail = document.createElement("small");
      card.className = "day-card";
      date.textContent = day.event_date;
      total.textContent = `${day.total_events} total`;
      detail.textContent = `${day.authorized_count} authorized · ${day.denied_count} denied`;
      card.append(date, total, detail);
      grid.append(card);
    });
  }

  function render(data) {
    text("metric-active-vehicles", data.summary.active_vehicles);
    text("metric-events-today", data.summary.events_today);
    text("metric-authorized-today", data.summary.authorized_today);
    text("metric-denied-today", data.summary.denied_today);
    text("system-camera", titleCase(data.system.camera_state));
    text("system-detector", titleCase(data.system.detector_state));
    text("system-gate", titleCase(data.system.gate_state));
    text("system-last-plate", data.system.last_plate || "None");
    text("system-heartbeat", data.system.last_heartbeat || "Not connected");
    indicator("camera-indicator", data.system.camera_running ? "online" : "offline");
    indicator(
      "detector-indicator",
      data.system.camera_running && ["active", "idle", "calibrating"].includes(data.system.detector_state)
        ? "online"
        : "offline",
    );
    indicator("gate-indicator", data.system.gate_state === "open" ? "online" : "neutral");
    updateCameraControl(data.system.camera_running);
    updateLatest(data.latest_event);
    updateRecent(data.recent_events);
    updateDaily(data.daily);
  }

  function schedule() {
    window.clearTimeout(timer);
    if (!document.hidden) timer = window.setTimeout(sync, POLL_INTERVAL_MS);
  }

  async function sync() {
    if (syncing || document.hidden) return schedule();
    syncing = true;
    try {
      const response = await fetch("/api/dashboard", {
        cache: "no-store",
        headers: { Accept: "application/json" },
      });
      if (response.redirected || response.status === 401) {
        window.location.assign(response.url || "/login");
        return;
      }
      if (!response.ok) throw new Error(`Dashboard sync failed: ${response.status}`);
      render(await response.json());
      text("sync-status", "Connected");
      byId("sync-status")?.classList.remove("error");
      document.body.dataset.lastSyncAt = String(Date.now());
    } catch (error) {
      text("sync-status", "Reconnecting…");
      byId("sync-status")?.classList.add("error");
      console.warn(error);
    } finally {
      syncing = false;
      schedule();
    }
  }

  document.addEventListener("visibilitychange", () => {
    window.clearTimeout(timer);
    if (!document.hidden) sync();
  });
  sync();
})();
