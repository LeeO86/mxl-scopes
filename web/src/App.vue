<script setup>
// The family look of the siblings (§9.1): header with label, state and registration pills and the
// version; tabs with their own address. Adapted from mxl-multiviewer (Apache-2.0, see NOTICE).
import { computed, onMounted, onUnmounted, ref } from "vue";
import { live, MODES, startLive, stateKind, stopLive } from "./api.js";
import Pill from "./components/Pill.vue";
import ScopeTab from "./components/ScopeTab.vue";
import InputTab from "./components/InputTab.vue";
import NmosTab from "./components/NmosTab.vue";
import StatusTab from "./components/StatusTab.vue";
import SettingsTab from "./components/SettingsTab.vue";

const tabs = [
  { id: "scope", label: "Scope", component: ScopeTab, full: true },
  { id: "input", label: "Input", component: InputTab },
  { id: "nmos", label: "NMOS", component: NmosTab },
  { id: "status", label: "Status", component: StatusTab },
  { id: "settings", label: "Settings", component: SettingsTab },
];

const current = ref("scope");
const tab = computed(() => tabs.find((t) => t.id === current.value));

function onHash() {
  const id = location.hash.slice(1);
  if (tabs.some((t) => t.id === id)) current.value = id;
}
function go(id) {
  current.value = id;
  location.hash = id;
}

const input = computed(() => live.status?.input);
const modeTitle = computed(() => MODES.find((m) => m.id === live.scope?.mode)?.title || live.scope?.mode || "–");

onMounted(() => {
  onHash();
  window.addEventListener("hashchange", onHash);
  startLive();
});
onUnmounted(() => {
  window.removeEventListener("hashchange", onHash);
  stopLive();
});
</script>

<template>
  <header>
    <h1>mxl-scopes</h1>
    <span v-if="live.info" class="node">{{ live.info.label }} · {{ modeTitle }} · {{ input?.label || "no input" }}</span>
    <span class="spacer"></span>
    <Pill v-if="input" :text="input.state.replace('_', ' ')" :kind="stateKind(input.state)" />
    <Pill v-if="live.status" :text="live.status.nmos.registered ? 'registered' : 'not registered'" :kind="live.status.nmos.registered ? 'ok' : 'neutral'" />
    <Pill :text="live.connected ? 'live' : 'offline'" :kind="live.connected ? 'ok' : 'bad'" />
    <span v-if="live.info" class="muted small">v{{ live.info.version }} · MXL {{ live.info.mxl_revision.slice(0, 7) }} · {{ live.info.simd.level }}</span>
  </header>
  <div v-if="live.error" class="banner bad">{{ live.error }}</div>
  <div v-else-if="live.everConnected && !live.connected" class="banner warn">Live updates lost. Reconnecting; values refresh every 2 s meanwhile.</div>
  <div v-if="live.restartKeys.length" class="banner warn">
    Restart required to apply {{ live.restartKeys.join(", ") }}.
    <button class="btn small secondary" @click="go('settings')">Settings</button>
  </div>
  <nav>
    <button
      v-for="t in tabs"
      :key="t.id"
      :class="{ active: current === t.id }"
      :aria-current="current === t.id ? 'page' : undefined"
      @click="go(t.id)"
    >
      {{ t.label }}
    </button>
  </nav>
  <main :class="{ full: tab.full }">
    <component :is="tab.component" />
  </main>
</template>
