"use strict";

const CHANNELS = {
  r: { label: "R", color: "#ff5e72" },
  g: { label: "G", color: "#4fe39a" },
  b: { label: "B", color: "#5897ff" },
  w: { label: "W", color: "#fff1cd" },
};
const CHANNEL_KEYS = Object.keys(CHANNELS);

const boardElements = new Map();
const pendingTimers = new Map();
const masterPowerControls = new Map(); // relay id -> {render, card} for relays embedded as a light's master power.
let appConfig = null;

let accessToken = ""; // Keep the credential in this tab's memory, never in persistent browser storage.
let accessPrompt = null;

function askForAccess() {
  if (accessPrompt) return accessPrompt; // Concurrent polls share a single sign-in dialog.
  accessPrompt = new Promise((resolve, reject) => {
    const dialog = document.querySelector("#access-dialog");
    const input = document.querySelector("#access-token");
    input.value = "";
    dialog.addEventListener("close", () => {
      const token = input.value.trim();
      input.value = "";
      accessPrompt = null;
      if (dialog.returnValue !== "connect" || !token) {
        reject(new Error("Access cancelled. Reload the page to sign in."));
        return;
      }
      accessToken = token;
      resolve();
    }, { once: true });
    dialog.showModal();
  });
  return accessPrompt;
}

async function request(path, options = {}) {
  let response;
  do {
    response = await fetch(path, {
      ...options,
      headers: { "Content-Type": "application/json", ...options.headers,
        ...(accessToken ? { Authorization: `Bearer ${accessToken}` } : {}) },
    });
    if (response.status === 401) await askForAccess(); // The server also protects the original UI routes.
  } while (response.status === 401);
  const payload = await response.json();
  if (!response.ok) {
    throw new Error(payload.error || `Request failed (${response.status})`);
  }
  return payload;
}

function toHex(red, green, blue) {
  return `#${[red, green, blue]
    .map((value) => Number(value).toString(16).padStart(2, "0"))
    .join("")}`;
}

function hexToRgb(hex) {
  const packed = Number.parseInt(hex.slice(1), 16);
  return {
    r: (packed >> 16) & 255,
    g: (packed >> 8) & 255,
    b: packed & 255,
  };
}

function pickChannels(source) {
  return Object.fromEntries(CHANNEL_KEYS.map((channel) => [channel, Number(source[channel]) || 0]));
} // Completes a clean {r,g,b,w} copy so zone records never share references.

function glowColor(values) {
  return `rgb(${values.r + values.w}, ${values.g + values.w}, ${values.b + values.w})`;
} // Completes an on-screen approximation where white brightens all three colors.

function currentValues(card) {
  return Object.fromEntries(
    CHANNEL_KEYS.map((channel) => [
      channel,
      Number(card.querySelector(`[data-channel="${channel}"]`).value),
    ]),
  );
}

// --- Zones -------------------------------------------------------------------
// card.zoneValues holds the last known {r,g,b,w} for every zone, in protocol order.
// card.target is null when the controls act on every zone together, or a 1-based zone number.

function targetIndexes(card) {
  return card.target === null ? card.zoneValues.map((_, index) => index) : [card.target - 1];
} // Completes the list of zone indexes the sliders, picker and presets will change.

function zonesMatch(card) {
  const [first, ...rest] = card.zoneValues;
  return rest.every((zone) => CHANNEL_KEYS.every((channel) => zone[channel] === first[channel]));
} // Completes the check used to warn that "together" is showing zone 1 of differing zones.

function zoneLabel(card, zone) {
  if (card.zoneNames.length < 2) return ""; // Single-zone boards keep their original messages.
  return zone === null ? "ALL ZONES " : `${card.zoneNames[zone - 1].toUpperCase()} `;
} // Completes the feedback prefix that says where a command went.

function setTarget(card, zone) {
  card.target = zone;
  card.querySelectorAll(".zone-tabs button").forEach((button) => {
    const buttonZone = button.dataset.zone === "all" ? null : Number(button.dataset.zone);
    button.setAttribute("aria-pressed", String(buttonZone === zone));
  });
  syncSliders(card, true); // Shows the newly selected zone's levels immediately.
} // Completes switching the card between "together" and one zone.

function buildZoneControls(card, zoneNames) {
  const tabs = card.querySelector(".zone-tabs");
  const swatches = card.querySelector(".zone-swatches");
  if (zoneNames.length < 2) return; // One zone needs no selector; the card looks as it always did.

  const allButton = document.createElement("button");
  allButton.type = "button";
  allButton.dataset.zone = "all";
  allButton.textContent = zoneNames.length === 2 ? "[ BOTH ]" : "[ ALL ZONES ]";
  tabs.append(allButton);

  zoneNames.forEach((name, index) => {
    const tab = document.createElement("button");
    tab.type = "button";
    tab.dataset.zone = String(index + 1);
    tab.textContent = `[ ${name.toUpperCase()} ]`;
    tabs.append(tab);

    const swatch = document.createElement("button");
    swatch.type = "button";
    swatch.className = "zone-swatch";
    swatch.dataset.zone = String(index + 1);
    swatch.title = `Control only ${name}`;
    swatch.innerHTML = `
      <span class="zone-swatch-color"></span>
      <span class="zone-swatch-name"></span>
      <span class="zone-swatch-values"></span>
    `;
    swatch.querySelector(".zone-swatch-name").textContent = name; // textContent keeps config names inert.
    swatch.addEventListener("click", () => setTarget(card, index + 1));
    swatches.append(swatch);
  });

  tabs.querySelectorAll("button").forEach((button) => {
    button.addEventListener("click", () => {
      setTarget(card, button.dataset.zone === "all" ? null : Number(button.dataset.zone));
    });
  });
  tabs.hidden = false;
  swatches.hidden = false;
} // Completes the zone tabs (BOTH / LEFT / RIGHT) and the per-zone swatches.

// --- Rendering ---------------------------------------------------------------

function renderColor(card) {
  const values = currentValues(card);
  const shown = targetIndexes(card).map((index) => glowColor(card.zoneValues[index]));
  card.querySelector(".color-glow").style.background = shown.length > 1
    ? `linear-gradient(90deg, ${shown.join(", ")})` // Together mode shows every zone side by side.
    : shown[0];
  card.querySelector(".numeric-color").textContent = card.target === null && !zonesMatch(card)
    ? "MIXED · a slider sets that channel on every zone"
    : `R ${values.r} · G ${values.g} · B ${values.b} · W ${values.w}`;
  card.querySelector(".color-picker").value = toHex(values.r, values.g, values.b);
  Object.entries(values).forEach(([channel, value]) => {
    card.querySelector(`[data-value="${channel}"]`).textContent = value;
  });

  card.querySelectorAll(".zone-swatch").forEach((swatch) => {
    const zone = Number(swatch.dataset.zone);
    const zoneValues = card.zoneValues[zone - 1];
    swatch.querySelector(".zone-swatch-color").style.background = glowColor(zoneValues);
    swatch.querySelector(".zone-swatch-values").textContent =
      `R${zoneValues.r} G${zoneValues.g} B${zoneValues.b} W${zoneValues.w}`;
    swatch.classList.toggle("selected", card.target === null || card.target === zone);
  });
} // Completes the glow, readouts, picker and swatches for the card's current target.

function syncSliders(card, syncFocused = false) {
  const values = card.zoneValues[card.target === null ? 0 : card.target - 1]; // Together shows zone 1.
  Object.entries(values).forEach(([channel, value]) => {
    const slider = card.querySelector(`[data-channel="${channel}"]`);
    if (slider && (syncFocused || document.activeElement !== slider)) {
      slider.value = value;
    }
  });
  renderColor(card);
} // Completes moving the sliders to the target's known levels without fighting a dragged slider.

function setFeedback(card, message, isError = false) {
  const feedback = card.querySelector(".board-feedback");
  feedback.textContent = message;
  feedback.classList.toggle("error", isError);
}

// --- Commands ----------------------------------------------------------------

function zoneField(zone) {
  return zone === null ? {} : { zone }; // Leaving zone out asks the server for every zone.
}

async function sendChannel(boardId, channel, value, zone, card) {
  try {
    await request(`/api/boards/${boardId}/channel`, {
      method: "POST",
      body: JSON.stringify({ channel, value, ...zoneField(zone) }),
    });
    setFeedback(card, `${zoneLabel(card, zone)}${CHANNELS[channel].label} ${value} sent`);
  } catch (error) {
    setFeedback(card, error.message, true);
  }
}

function scheduleChannel(boardId, channel, value, zone, card) {
  const timerKey = `${boardId}:${zone ?? "all"}:${channel}`; // Debounces each zone and channel separately.
  clearTimeout(pendingTimers.get(timerKey));
  pendingTimers.set(
    timerKey,
    setTimeout(() => sendChannel(boardId, channel, value, zone, card), 90),
  );
}

async function sendColor(boardId, values, zone, card) {
  try {
    await request(`/api/boards/${boardId}/color`, {
      method: "POST",
      body: JSON.stringify({ ...values, ...zoneField(zone) }),
    });
    setFeedback(card, `${zoneLabel(card, zone)}Color sent`);
  } catch (error) {
    setFeedback(card, error.message, true);
  }
}

function applyColorToTargets(card, values) {
  targetIndexes(card).forEach((index) => { card.zoneValues[index] = pickChannels(values); });
  syncSliders(card, true);
  sendColor(Number(card.dataset.boardId), values, card.target, card);
} // Completes a full-color change (picker or preset) on the selected zone or zones.

function renderRelayPower(card, power) {
  const knownPower = typeof power === "boolean";
  card.classList.toggle("power-on", power === true);
  card.classList.toggle("power-off", power === false);
  card.querySelector(".relay-state").textContent = knownPower
    ? `LAST COMMAND: ${power ? "ON" : "OFF"}`
    : "UNKNOWN";
  card.querySelectorAll("[data-power]").forEach((button) => {
    const selected = knownPower && power === (button.dataset.power === "true");
    button.setAttribute("aria-pressed", String(selected));
  });
}

async function sendPower(boardId, power, card) {
  const buttons = card.querySelectorAll("[data-power]");
  buttons.forEach((button) => { button.disabled = true; });
  try {
    await request(`/api/boards/${boardId}/power`, {
      method: "POST",
      body: JSON.stringify({ power }),
    });
    renderRelayPower(card, power);
    setFeedback(card, `Lights ${power ? "ON" : "OFF"} command sent`);
  } catch (error) {
    setFeedback(card, error.message, true);
  } finally {
    buttons.forEach((button) => { button.disabled = false; });
  }
}

function renderMasterPower(control, power) {
  const known = typeof power === "boolean";
  control.el.classList.toggle("on", power === true);
  control.el.classList.toggle("off", power === false);
  control.el.querySelector(".power-master-value").textContent = known ? (power ? "ON" : "OFF") : "—";
  control.el.querySelectorAll("[data-power]").forEach((button) => {
    button.setAttribute("aria-pressed", String(known && power === (button.dataset.power === "true")));
  });
  // Cutting the relay kills all power to the board, so dim its light controls while it is off.
  control.card.classList.toggle("powered-off", power === false);
} // Completes the master-power visual for one embedded relay.

async function sendMasterPower(control, power) {
  const buttons = control.el.querySelectorAll("[data-power]");
  buttons.forEach((button) => { button.disabled = true; });
  try {
    await request(`/api/boards/${control.relayId}/power`, {
      method: "POST",
      body: JSON.stringify({ power }),
    });
    renderMasterPower(control, power);
    setFeedback(control.card, `Power ${power ? "ON" : "OFF"} sent (relay ${control.relayId})`);
  } catch (error) {
    setFeedback(control.card, error.message, true);
  } finally {
    buttons.forEach((button) => { button.disabled = false; });
  }
} // Completes a master-power switch over the existing relay power API.

function buildMasterPower(card, relayBoard) {
  const el = document.createElement("div");
  el.className = "power-master";
  el.dataset.relayId = relayBoard.id;
  el.innerHTML = `
    <span class="power-master-title">&#9211; POWER</span>
    <div class="power-master-indicator"><span class="power-master-dot"></span><span class="power-master-value">&mdash;</span></div>
    <div class="power-master-buttons">
      <button type="button" data-power="true">ON</button>
      <button type="button" data-power="false">OFF</button>
    </div>
    <small class="power-master-hint">Master supply</small>
  `;
  const control = { el, card, relayId: relayBoard.id };
  el.querySelectorAll("[data-power]").forEach((button) => {
    button.addEventListener("click", () => sendMasterPower(control, button.dataset.power === "true"));
  });
  masterPowerControls.set(relayBoard.id, { render: (power) => renderMasterPower(control, power), card });
  renderMasterPower(control, null);
  return el;
} // Completes an embedded relay control, pinned right of the board's light sliders.

function createBoardCard(board) {
  const fragment = document.querySelector("#board-template").content.cloneNode(true);
  const card = fragment.querySelector(".board-card");
  const zoneNames = Array.isArray(board.zones) && board.zones.length ? board.zones : ["Main"];
  card.dataset.boardId = board.id;
  card.dataset.boardKind = "rgbw";
  card.zoneNames = zoneNames;
  card.zoneValues = zoneNames.map(() => pickChannels({}));
  card.target = null; // Starts in "together" mode.
  card.querySelector(".board-id").textContent = `> BOARD/${board.id}`;
  card.querySelector(".board-name").textContent = board.name;
  buildZoneControls(card, zoneNames);

  const sliderContainer = card.querySelector(".sliders");
  Object.entries(CHANNELS).forEach(([channel, details]) => {
    const row = document.createElement("label");
    row.className = "slider-row";
    row.style.setProperty("--channel-color", details.color);
    row.innerHTML = `
      <span class="channel-name">${details.label}</span>
      <input type="range" min="0" max="255" value="0" data-channel="${channel}">
      <output class="channel-value" data-value="${channel}">0</output>
    `;
    const slider = row.querySelector("input");
    slider.addEventListener("input", () => {
      const value = Number(slider.value);
      targetIndexes(card).forEach((index) => { card.zoneValues[index][channel] = value; });
      renderColor(card);
      scheduleChannel(board.id, channel, value, card.target, card);
    });
    sliderContainer.append(row);
  });

  card.querySelector(".color-picker").addEventListener("change", (event) => {
    applyColorToTargets(card, { ...currentValues(card), ...hexToRgb(event.target.value) });
  });

  card.querySelectorAll(".presets button").forEach((button) => {
    button.addEventListener("click", () => {
      const [r, g, b, w] = button.dataset.color.split(",").map(Number);
      applyColorToTargets(card, { r, g, b, w });
    });
  });

  // A board may name a relay that switches its whole power supply. Pin that relay's
  // control to the right of the light sliders; the LED "off" only paints black, while
  // this master switch cuts all power (so the board itself goes offline when off).
  if (board.power_relay) {
    const relayBoard = (appConfig?.boards || []).find((candidate) => candidate.id === board.power_relay);
    if (relayBoard) {
      const sliders = card.querySelector(".sliders");
      const lightBody = document.createElement("div");
      lightBody.className = "light-body";
      sliders.replaceWith(lightBody);           // Put the flex row where the sliders were...
      lightBody.append(sliders, buildMasterPower(card, relayBoard)); // ...then sliders on the left, power on the right.
      card.dataset.powerRelay = relayBoard.id;
    }
  }

  // A board may group other lights in the same room, shown as sub-cards at the bottom
  // (e.g. the office server LEDs and the Cync strip grouped under the office lights).
  if (Array.isArray(board.group) && board.group.length) {
    const room = document.createElement("section");
    room.className = "room-group";
    room.innerHTML = '<p class="room-group-title">&gt; ALSO IN THIS ROOM</p>';
    board.group.forEach((memberId) => {
      const memberBoard = (appConfig?.boards || []).find((candidate) => candidate.id === memberId);
      if (!memberBoard) return; // A missing or mistyped member id is skipped, not fatal.
      const sub = createConfiguredBoard(memberBoard); // Full controls, registered for status like any card.
      sub.classList.add("sub-card");
      room.append(sub);
    });
    card.append(room);
  }

  boardElements.set(board.id, card);
  setTarget(card, null);
  return card;
} // Completes an RGBW card, with zone tabs and swatches when the board has more than one zone.

function createRelayCard(board) {
  const fragment = document.querySelector("#relay-template").content.cloneNode(true);
  const card = fragment.querySelector(".board-card");
  card.dataset.boardId = board.id;
  card.dataset.boardKind = "relay";
  card.querySelector(".board-id").textContent = `> RELAY/${board.id}`;
  card.querySelector(".board-name").textContent = board.name;

  card.querySelectorAll("[data-power]").forEach((button) => {
    button.addEventListener("click", () => {
      sendPower(board.id, button.dataset.power === "true", card);
    });
  });

  boardElements.set(board.id, card);
  renderRelayPower(card, null);
  return card;
}

function createConfiguredBoard(board) {
  // Selects the controls appropriate for the configured firmware type.
  return board.kind === "relay" ? createRelayCard(board) : createBoardCard(board);
}

function applyReportedZones(card, state) {
  const reported = Array.isArray(state.zones) && state.zones.length ? state.zones : [state];
  const dragged = card.contains(document.activeElement) ? document.activeElement.dataset.channel : undefined;
  const targets = new Set(targetIndexes(card));
  card.zoneValues = card.zoneValues.map((known, index) => {
    if (!reported[index]) return known;
    const next = pickChannels(reported[index]);
    if (dragged && targets.has(index)) next[dragged] = known[dragged]; // Keeps the value being dragged.
    return next;
  });
  syncSliders(card);
} // Completes merging the server's per-zone state into the card.

function updateStatus(status) {
  const connectionDot = document.querySelector("#connection-dot");
  const connectionLabel = document.querySelector("#connection-label");
  connectionDot.classList.toggle("online", status.mqtt_connected);
  connectionLabel.textContent = status.mqtt_connected ? "MQTT online" : "MQTT offline";
  if (!status.mqtt_connected && status.mqtt_error) {
    connectionLabel.title = status.mqtt_error;
  }

  Object.entries(status.boards).forEach(([boardId, state]) => {
    const master = masterPowerControls.get(Number(boardId));
    if (master) master.render(state.power); // Embedded relay: refresh its master-power control.

    const card = boardElements.get(Number(boardId));
    if (!card) return;

    if (card.dataset.boardKind === "relay") {
      renderRelayPower(card, state.power);
      card.querySelector(".board-status-text").textContent = typeof state.power === "boolean"
        ? `Last command: ${state.power ? "ON" : "OFF"}`
        : "No command sent";
      return;
    }

    const stateDot = card.querySelector(".board-status-dot");
    const stateText = card.querySelector(".board-status-text");
    const isOnline = state.status === "online" || state.status === "ready";
    stateDot.classList.toggle("online", isOnline);
    stateText.textContent = state.status || "unknown";
    applyReportedZones(card, state);
  });
}

async function pollStatus() {
  try {
    updateStatus(await request("/api/status"));
  } catch (error) {
    document.querySelector("#connection-dot").classList.remove("online");
    document.querySelector("#connection-label").textContent = "Web app offline";
  }
}

async function initialize() {
  try {
    appConfig = await request("/api/config");
    document.querySelector("#broker-label").textContent = appConfig.broker;
    document.querySelector("#board-count").textContent =
      `${appConfig.boards.length} configured board${appConfig.boards.length === 1 ? "" : "s"}`;
    const commandModes = new Set(
      appConfig.boards.map((board) => board.command_mode || appConfig.command_mode),
    );
    document.querySelector("#command-mode").textContent =
      `${commandModes.size > 1 ? "mixed" : [...commandModes][0]} MQTT commands / ${appConfig.base_topic}`;

    const boardGrid = document.querySelector("#boards");
    const embeddedRelays = new Set(appConfig.boards.map((board) => board.power_relay).filter(Boolean));
    const groupedMembers = new Set(
      appConfig.boards.flatMap((board) => (Array.isArray(board.group) ? board.group : [])),
    );
    appConfig.boards.forEach((board) => {
      if (embeddedRelays.has(board.id) || groupedMembers.has(board.id)) return; // Shown embedded in another card.
      boardGrid.append(createConfiguredBoard(board));
    });
    await pollStatus();
    setInterval(pollStatus, 1200);
  } catch (error) {
    const errorMessage = document.createElement("p");
    errorMessage.className = "board-feedback error";
    errorMessage.textContent = `Unable to initialize: ${error.message}`;
    document.querySelector("#boards").replaceChildren(errorMessage);
  }
}

document.querySelector("#all-off").addEventListener("click", async () => {
  try {
    await request("/api/all/off", { method: "POST", body: "{}" });
    boardElements.forEach((card) => {
      if (card.dataset.boardKind === "relay") {
        renderRelayPower(card, false);
      } else {
        card.zoneValues = card.zoneValues.map(() => pickChannels({})); // Firmware "0" turns every zone off.
        syncSliders(card, true);
      }
    });
    masterPowerControls.forEach((master) => master.render(false)); // Embedded relays are cut by ALL OFF too.
  } catch (error) {
    window.alert(error.message);
  }
});

initialize();
