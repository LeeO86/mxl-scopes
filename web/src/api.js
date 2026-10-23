// REST client and the live state from /api/v1/events (SPECIFICATION §9). Adapted from mxl-multiviewer
// web/src/api.js (Apache-2.0, see NOTICE).
import { reactive } from "vue";

async function request(path, { method = "GET", body, text = false } = {}) {
  const headers = {};
  if (body !== undefined) headers["Content-Type"] = "application/json";
  const resp = await fetch(path, {
    method,
    headers,
    body: body === undefined ? undefined : typeof body === "string" ? body : JSON.stringify(body),
    cache: "no-store",
  });
  const raw = await resp.text();
  let data = raw;
  if (!text) {
    try {
      data = raw ? JSON.parse(raw) : null;
    } catch {
      data = raw;
    }
  }
  if (!resp.ok) {
    const reason = data && typeof data === "object" ? data.error : String(data || "").slice(0, 200);
    throw new Error(reason || `HTTP ${resp.status}`);
  }
  return data;
}

export const api = {
  get: (path) => request(path),
  text: (path) => request(path, { text: true }),
  put: (path, body) => request(path, { method: "PUT", body }),
  patch: (path, body) => request(path, { method: "PATCH", body }),
  post: (path, body) => request(path, { method: "POST", body: body ?? {} }),
};

/** Live state shared by every tab: /api/v1/info once, then status, scope and measurements from the WebSocket. */
export const live = reactive({
  info: null,
  status: null,
  scope: null,
  measurements: null,
  connected: false,
  everConnected: false,
  error: "",
  // Keys a settings change marked restart-required (until the page reloads).
  restartKeys: [],
  // Fields the last PATCH applied but could not save (the environment sets them).
  notPersisted: [],
});

let socket = null;
let retry = 0;
let poll = 0;
let users = 0;

async function pollOnce() {
  try {
    const [status, scope, measurements] = await Promise.all([api.get("/api/v1/status"), api.get("/api/v1/scope"), api.get("/api/v1/measurements")]);
    live.status = status;
    live.scope = scope;
    live.measurements = measurements;
    live.error = "";
  } catch (e) {
    live.error = `API unreachable: ${e.message}`;
  }
}

function connect() {
  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  socket = new WebSocket(`${proto}//${location.host}/api/v1/events`);
  socket.onopen = () => {
    live.connected = true;
    live.everConnected = true;
    live.error = "";
  };
  socket.onmessage = (ev) => {
    try {
      const msg = JSON.parse(ev.data);
      if (msg.status) live.status = msg.status;
      // A control being changed keeps its value until the PATCH answered (no flicker back).
      if (msg.scope && Date.now() > editingUntil) live.scope = msg.scope;
      if (msg.measurements) live.measurements = msg.measurements;
    } catch {
      /* a broken frame is dropped; the next one replaces it */
    }
  };
  socket.onclose = () => {
    live.connected = false;
    retry = setTimeout(connect, 2000);
  };
}

/** Loads /api/v1/info, then follows the WebSocket; polls while it is down. Shared by every user. */
export async function startLive() {
  if (++users > 1) return;
  try {
    live.info = await api.get("/api/v1/info");
  } catch (e) {
    live.error = `API unreachable: ${e.message}`;
  }
  await pollOnce();
  connect();
  poll = setInterval(() => {
    if (!live.connected) pollOnce();
  }, 2000);
}

export function stopLive() {
  if (--users > 0) return;
  clearInterval(poll);
  clearTimeout(retry);
  if (socket) {
    socket.onclose = null;
    socket.close();
  }
}

/** Sends a change of the live display settings (PATCH /api/v1/scope); at most 10 a second. */
let pending = {};
let sendTimer = 0;
let lastSend = 0;
let editingUntil = 0;
export function patchScope(change) {
  Object.assign(pending, change);
  editingUntil = Date.now() + 1500;
  if (live.scope) Object.assign(live.scope, change);
  const wait = Math.max(0, 100 - (Date.now() - lastSend));
  clearTimeout(sendTimer);
  return new Promise((resolve, reject) => {
    sendTimer = setTimeout(async () => {
      const body = pending;
      pending = {};
      lastSend = Date.now();
      try {
        const result = await api.patch("/api/v1/scope", body);
        live.scope = result.scope || result;
        live.notPersisted = result.not_persisted || [];
        editingUntil = 0;
        resolve();
      } catch (e) {
        reject(e);
      }
    }, wait);
  });
}

// ---- helpers ---------------------------------------------------------------

/** Pill kind for an input state (§11). */
export function stateKind(state) {
  return { running: "ok", holding: "warn", waiting: "warn", no_signal: "bad", format_mismatch: "bad", not_routed: "neutral" }[state] || "neutral";
}

export const MODES = [
  { id: "luma", title: "Luma waveform" },
  { id: "ycbcr-parade", title: "YCbCr parade" },
  { id: "rgb-parade", title: "RGB parade" },
  { id: "rgb-overlay", title: "RGB overlay" },
  { id: "ycbcr-overlay", title: "YCbCr overlay" },
];

export const shortId = (id) => (id ? String(id).slice(0, 8) : "–");

export const pct = (v) => (typeof v === "number" ? `${(100 * v).toFixed(2)} %` : "–");

/** Copies text; falls back to a hidden textarea on plain http, where navigator.clipboard is missing. */
export async function copyText(text) {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch {
    const area = document.createElement("textarea");
    area.value = text;
    area.style.position = "fixed";
    area.style.opacity = "0";
    document.body.appendChild(area);
    area.select();
    const ok = document.execCommand("copy");
    area.remove();
    return ok;
  }
}

export function download(name, text, type = "application/json") {
  const url = URL.createObjectURL(new Blob([text], { type }));
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
