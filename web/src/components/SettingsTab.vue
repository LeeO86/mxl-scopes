<script setup>
// Settings (§8, §9.1): every setting with its origin, edits to the config file, export (JSON
// document and KEY=value) and import. Adapted from mxl-multiviewer (Apache-2.0, see NOTICE).
import { computed, onMounted, ref } from "vue";
import { api, copyText, download, live } from "../api.js";

const settings = ref([]);
const edits = ref({}); // key -> new string, or null to remove the file value
const msg = ref({ kind: "", text: "" });
const exportDoc = ref("");
const envText = ref("");
const importText = ref("");
const importMsg = ref({ kind: "", text: "" });
const exportMsg = ref("");

const GROUPS = [
  { title: "Scope", test: /^SCOPES_(MODE|UPDATE_RATE|ACCUMULATE|LINE_STEP|PIXEL_STEP|RANGE|GRATICULE|INTENSITY|TRACE|CAPTION|LAYOUT|BG_ALPHA|INPUT_OFFSET|HOLD_MS)/ },
  { title: "Output flow", test: /^SCOPES_OUTPUT_/ },
  { title: "Preview and widgets", test: /^(SCOPES_PREVIEW_|PREVIEW_|MEDIAMTX_|WIDGET_)/ },
  { title: "Engine", test: /^SCOPES_(THREADS|BACKEND|SIMD)/ },
  { title: "NMOS", test: /^NMOS_/ },
  { title: "MXL, web, state and process", test: /./ },
];
// One line per key, from SPECIFICATION §8.
const DESCRIPTIONS = {
  SCOPES_MODE: "luma, ycbcr-parade, rgb-parade, rgb-overlay or ycbcr-overlay.",
  SCOPES_UPDATE_RATE: "frame or 1–60 Hz; the overlays at most 30 Hz.",
  SCOPES_ACCUMULATE: "latest (newest frame per update) or all (every frame between updates).",
  SCOPES_LINE_STEP: "Analyse every n-th line (1–8).",
  SCOPES_PIXEL_STEP: "Analyse every n-th sample of a v210 group (1, 2, 3, 6).",
  SCOPES_RANGE: "limited (BT.709 narrow) or full.",
  SCOPES_GRATICULE_UNITS: "mv, percent or code.",
  SCOPES_GRATICULE_LEVEL: "Graticule brightness, 0–100 % (0 hides it).",
  SCOPES_INTENSITY: "Trace intensity, 1–400 %.",
  SCOPES_TRACE_MIN: "The dimmest visible trace, 0–1023.",
  SCOPES_TRACE_COLOR: "green, white or amber (luma waveform).",
  SCOPES_CAPTION: "The header line with mode, input and rate.",
  SCOPES_LAYOUT: "single (the only layout in v1).",
  SCOPES_BG_ALPHA: "Key of the background with SCOPES_OUTPUT_KEY, 0–1023.",
  SCOPES_INPUT_OFFSET_GRAINS: "Analyse this many grains behind the newest (0–30).",
  SCOPES_HOLD_MS: "NO SIGNAL after this long without a new grain.",
  SCOPES_OUTPUT_FORMAT: "Output raster, e.g. 1920x1080; a change opens a new output flow.",
  SCOPES_OUTPUT_RATE: "input or a rate token (25, 50, 5994, N/D); a change opens a new output flow.",
  SCOPES_OUTPUT_KEY: "true writes video/v210a with a key; a change opens a new output flow.",
  SCOPES_THREADS: "Renderer threads, 1–8 (strip workers).",
  SCOPES_BACKEND: "auto or cpu.",
  SCOPES_SIMD: "auto (AVX2), avx512, avx2 or scalar.",
  SCOPES_PREVIEW_MODE: "webrtc (H.264 via MediaMTX) or off.",
  SCOPES_PREVIEW_FPS: "Preview pictures per second (1–30, at most the update rate).",
  SCOPES_PREVIEW_HEIGHT: "Preview lines (270–1080, even).",
  SCOPES_STATE_DIR: "Folder for config.json, routes.json and mediamtx.yml.",
  SCOPES_CONFIG_FILE: "This configuration file (empty: config.json in the state folder).",
  PREVIEW_PUBLISH_URL: "RTSP base of a shared MediaMTX (empty: the built-in one runs).",
  PREVIEW_PATH_PREFIX: "MediaMTX path prefix; the stream is <prefix>/scope.",
  PREVIEW_WHEP_URL: "Public WHEP base the page plays from (empty: the built-in MediaMTX).",
  PREVIEW_HLS_URL: "Public HLS base (empty: the built-in MediaMTX).",
  WIDGET_FRAME_ANCESTORS: "Pages that may frame the widgets (CSP frame-ancestors); listed origins also read /widgets.",
  MEDIAMTX_RTSP_PORT: "Built-in MediaMTX: RTSP ingest on 127.0.0.1.",
  MEDIAMTX_WHEP_PORT: "Built-in MediaMTX: WHEP (WebRTC) port.",
  MEDIAMTX_HLS_PORT: "Built-in MediaMTX: HLS port.",
  MEDIAMTX_ICE_UDP_PORT: "Built-in MediaMTX: WebRTC ICE port (UDP and TCP).",
  HOST_ID: "Node label when NMOS_LABEL is empty.",
  MXL_DOMAIN_SCAN_PATH: "MXL root: the folder that holds the domains.",
  MXL_OUTPUT_DOMAIN_DIR: "Folder of the output domain this scope writes.",
  MXL_OUTPUT_DOMAIN_ID: "Output domain id when the domain is created (empty: from NMOS_SEED).",
  MXL_HISTORY_DURATION_NS: "Ring length of a new output domain.",
  MXL_CLEANUP_ON_EXIT: "Remove the output domain when the process stops.",
  NMOS_ENABLE: "Run the NMOS node (IS-04 registration, IS-05 routing).",
  NMOS_REGISTRY_ADDRESS: "Registry address.",
  NMOS_REGISTRY_PORT: "Registry registration port.",
  NMOS_QUERY_ADDRESS: "Query API address (empty: the registry).",
  NMOS_QUERY_PORT: "Query API port (empty: registry port + 1).",
  NMOS_DNS_SD: "Find the registry and advertise with DNS-SD.",
  NMOS_PORT: "Node and Connection API port; the WebSocket uses the next port.",
  NMOS_SEED: "Seed of every NMOS id and of the default domain id.",
  NMOS_LABEL: "Node label and device label prefix.",
  NMOS_HOST_ADDRESS: "IPv4 address announced to NMOS.",
  NMOS_TAGS: "Tags on the node and device (JSON object of string lists).",
  WEB_ENABLE: "Serve this UI and changes over the API.",
  WEB_PORT: "Port of this UI and the API.",
  LOG_LEVEL: "trace, debug, info, warn or error.",
  LOG_FORMAT: "json or text.",
  SHUTDOWN_TIMEOUT_S: "Seconds to stop cleanly after SIGTERM.",
};

const describe = (key) => DESCRIPTIONS[key] || "";

const ORIGIN = {
  environment: { text: "ENV", cls: "env" },
  live: { text: "ENV, LIVE", cls: "env" },
  file: { text: "FILE", cls: "file" },
  default: { text: "DEFAULT", cls: "default" },
};

const groups = computed(() => {
  const left = [...settings.value];
  return GROUPS.map((g) => {
    const items = left.filter((s) => g.test.test(s.key));
    items.forEach((s) => left.splice(left.indexOf(s), 1));
    return { title: g.title, items };
  }).filter((g) => g.items.length);
});
const changed = computed(() => Object.keys(edits.value));

function value(s) {
  return s.key in edits.value ? (edits.value[s.key] ?? "") : s.value;
}
function edit(s, v) {
  if (v === s.value) delete edits.value[s.key];
  else edits.value[s.key] = v;
}
function reset(s) {
  edits.value[s.key] = null;
}

async function load() {
  settings.value = (await api.get("/api/v1/config")).settings;
}

async function save() {
  try {
    const result = await api.put("/api/v1/config", edits.value);
    // Live keys apply at once, the output flow keys open a new output flow, restart keys wait.
    live.restartKeys = [...new Set([...live.restartKeys, ...(result.keys || [])])];
    const parts = [`Saved ${changed.value.join(", ")}.`];
    if (result.flow_keys?.length) parts.push(`New output flow for ${result.flow_keys.join(", ")}.`);
    if (result.keys?.length) parts.push(`Restart to apply ${result.keys.join(", ")}.`);
    msg.value = { kind: "ok", text: parts.join(" ") };
    edits.value = {};
    await load();
  } catch (e) {
    msg.value = { kind: "err", text: e.message };
  }
}

async function loadExport() {
  exportDoc.value = JSON.stringify(await api.get("/api/v1/config/export"), null, 2);
  envText.value = await api.text("/api/v1/config/env");
}

async function copy(text, what) {
  exportMsg.value = (await copyText(text)) ? `${what} copied.` : "Copy failed; select the text and copy it by hand.";
}

function onFile(ev) {
  const file = ev.target.files[0];
  if (file) file.text().then((t) => (importText.value = t));
}

async function doImport() {
  try {
    JSON.parse(importText.value);
  } catch (e) {
    importMsg.value = { kind: "err", text: `Not JSON: ${e.message}` };
    return;
  }
  try {
    const result = await api.post("/api/v1/config/import", importText.value);
    const skipped = result.skipped?.length ? ` Skipped (set by the environment): ${result.skipped.join(", ")}.` : "";
    const restart = result.keys?.length ? ` Restart to apply ${result.keys.join(", ")}.` : "";
    const flow = result.flow_keys?.length ? ` New output flow for ${result.flow_keys.join(", ")}.` : "";
    importMsg.value = {
      kind: "ok",
      text: `Imported${result.routes_restart ? "; the route applies after the next restart" : ""}.${flow}${restart}${skipped}`,
    };
    await load();
    await loadExport();
  } catch (e) {
    importMsg.value = { kind: "err", text: e.message };
  }
}

onMounted(async () => {
  try {
    await load();
    await loadExport();
  } catch (e) {
    msg.value = { kind: "err", text: e.message };
  }
});
</script>

<template>
  <div class="panel">
    <h3>Configuration</h3>
    <p class="note">
      Precedence: environment, then the configuration file, then the default. Settings set by an environment variable (ENV) are read-only here. Saved
      values go into the configuration file. LIVE settings apply at once, FLOW settings open a new output flow, RESTART settings apply at the next start.
    </p>
    <div class="actions" style="margin-top: 0">
      <span class="msg" :class="msg.kind" style="margin: 0">{{ msg.text }}</span>
      <span class="spacer"></span>
      <button class="btn secondary" :disabled="!changed.length" @click="edits = {}">Discard</button>
      <button class="btn" :disabled="!changed.length" @click="save">Save {{ changed.length ? `(${changed.length})` : "" }}</button>
    </div>
  </div>

  <div v-for="g in groups" :key="g.title" class="panel">
    <h3>{{ g.title }}</h3>
    <table>
      <thead>
        <tr>
          <th style="width: 30%">Key</th>
          <th>Value</th>
          <th style="width: 7rem">Origin</th>
          <th style="width: 6rem"></th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="s in g.items" :key="s.key">
          <td>
            <code>{{ s.key }}</code>
            <span class="badge" :class="s.apply === 'restart' ? 'restart' : 'file'">{{ s.apply.toUpperCase() }}</span>
            <div class="desc">{{ describe(s.key) }}</div>
          </td>
          <td>
            <input
              :value="value(s)"
              :disabled="!s.editable"
              :class="{ dirty: s.key in edits }"
              :aria-label="s.key"
              :placeholder="s.key in edits && edits[s.key] === null ? '(default)' : ''"
              @input="edit(s, $event.target.value)"
            />
          </td>
          <td><span class="badge" :class="ORIGIN[s.source]?.cls">{{ ORIGIN[s.source]?.text || s.source }}</span></td>
          <td>
            <button v-if="s.source === 'file' && s.editable" class="btn small secondary" title="Remove the file value" @click="reset(s)">Default</button>
          </td>
        </tr>
      </tbody>
    </table>
  </div>

  <div class="grid wide">
    <div class="panel">
      <h3>Export</h3>
      <p class="note">One JSON document with every setting and the route. The scope has no secrets.</p>
      <div class="actions" style="margin-top: 0">
        <span class="msg ok" style="margin: 0">{{ exportMsg }}</span>
        <span class="spacer"></span>
        <button class="btn secondary" :disabled="!exportDoc" @click="copy(exportDoc, 'JSON')">Copy JSON</button>
        <button class="btn" :disabled="!exportDoc" @click="download('mxl-scopes.json', exportDoc)">Download JSON</button>
      </div>
      <pre>{{ exportDoc }}</pre>
      <div class="actions">
        <button class="btn secondary" :disabled="!envText" @click="copy(envText, 'KEY=value list')">Copy KEY=value</button>
        <button class="btn secondary" :disabled="!envText" @click="download('mxl-scopes.env', envText, 'text/plain')">Download KEY=value</button>
      </div>
    </div>
    <div class="panel">
      <h3>Import</h3>
      <p class="note">An exported document. Settings are saved at once; the route applies after the next restart.</p>
      <input type="file" accept="application/json,.json" aria-label="Open an exported JSON file" @change="onFile" />
      <textarea v-model="importText" aria-label="Exported JSON" placeholder='{"version": 1, "settings": {...}, "routes": {...}}'></textarea>
      <div class="actions">
        <span class="msg" :class="importMsg.kind" style="margin: 0">{{ importMsg.text }}</span>
        <span class="spacer"></span>
        <button class="btn" :disabled="!importText.trim()" @click="doImport">Import</button>
      </div>
    </div>
  </div>
</template>
