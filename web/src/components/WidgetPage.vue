<script setup>
// The `scope` widget (SPECIFICATION §9.3): /widget/scope[?controls=][&measurements=][&theme=], the
// scope picture filling the frame, without the app around it. With `controls` a slim bar with the
// mode and the update rate; with `measurements` one line of luma min/max and out-of-range share.
// Posts {type: "widget-ready"} at its first picture and {type: "widget-size", w, h} then and on every
// resize. Adapted from mxl-multiviewer web/src/components/WidgetPage.vue (Apache-2.0, see NOTICE).
import { computed, onMounted, onUnmounted, ref } from "vue";
import { live, MODES, patchScope, pct, startLive, stopLive } from "../api.js";
import ScopeView from "./ScopeView.vue";

const params = new URLSearchParams(location.search);
const flag = (name) => ["true", "1"].includes(params.get(name) || "");
const controls = flag("controls");
const measurements = flag("measurements");
const theme = params.get("theme");
if (theme) document.documentElement.dataset.theme = theme;
// A frame is see-through only when its colour scheme matches its parent's; without the meta it is the default.
if (theme === "transparent") document.querySelector('meta[name="color-scheme"]')?.remove();

const root = ref(null);
const error = ref("");
let ready = false;
let observer = null;

const line = computed(() => {
  const m = live.measurements;
  if (!m) return "–";
  const parts = [];
  if (m.luma) parts.push(`Y ${m.luma.min_code}–${m.luma.max_code}`, `out ${pct(m.luma.below_64_ratio + m.luma.above_940_ratio)}`);
  if (m.chroma) parts.push(`C out ${pct(m.chroma.out_of_range_ratio)}`);
  if (m.rgb) parts.push(`gamut ${pct(m.rgb.out_of_gamut_ratio)}`);
  return parts.join(" · ") || "no measurement";
});

function post(message) {
  if (window.parent !== window) window.parent.postMessage(message, "*");
}
function postSize() {
  const rect = root.value.getBoundingClientRect();
  post({ type: "widget-size", w: Math.round(rect.width), h: Math.round(rect.height) });
}
function shown() {
  if (ready) return;
  ready = true;
  post({ type: "widget-ready" });
  postSize();
}
async function set(change) {
  try {
    error.value = "";
    await patchScope(change);
  } catch (e) {
    error.value = e.message;
  }
}

onMounted(() => {
  startLive();
  observer = new ResizeObserver(() => ready && postSize());
  observer.observe(root.value);
});
onUnmounted(() => {
  observer?.disconnect();
  stopLive();
});
</script>

<template>
  <div ref="root" class="widget-root">
    <div v-if="controls && live.scope" class="widget-bar">
      <select :value="live.scope.mode" aria-label="Mode" @change="set({ mode: $event.target.value })">
        <option v-for="mode in MODES" :key="mode.id" :value="mode.id">{{ mode.title }}</option>
      </select>
      <select :value="String(live.scope.update_rate)" aria-label="Update rate" @change="set({ update_rate: $event.target.value })">
        <option value="frame">every frame</option>
        <option v-for="hz in ['50', '25', '12.5', '10', '5', '1']" :key="hz" :value="hz">{{ hz }} Hz</option>
      </select>
      <span v-if="error" class="msg err">{{ error }}</span>
    </div>
    <div class="widget-picture">
      <ScopeView fill :snapshot-ms="1000" @shown="shown" />
    </div>
    <div v-if="measurements" class="widget-line">{{ line }}</div>
  </div>
</template>
