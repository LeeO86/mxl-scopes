<script setup>
// Status (§9.1): probes, SIMD level, backend, preview state, the counters of /metrics.
import { computed } from "vue";
import { live } from "../api.js";
import Pill from "./Pill.vue";

const st = computed(() => live.status);
const p = computed(() => live.status?.preview);
const simd = computed(() => live.info?.simd);
</script>

<template>
  <div class="grid wide">
    <div class="panel">
      <h3>Probes</h3>
      <dl v-if="st" class="kv">
        <dt>Live</dt>
        <dd><Pill :text="st.live ? 'live' : 'stale'" :kind="st.live ? 'ok' : 'bad'" /></dd>
        <dt>Ready</dt>
        <dd><Pill :text="st.ready ? 'ready' : st.ready_reason || 'not ready'" :kind="st.ready ? 'ok' : 'warn'" /></dd>
        <dt>Registered</dt>
        <dd>{{ st.nmos.registered ? "yes" : "no" }}</dd>
      </dl>
      <p class="note"><a href="/metrics" target="_blank">/metrics</a> · <a href="/api/v1/status" target="_blank">/api/v1/status</a></p>
    </div>
    <div class="panel">
      <h3>Engine</h3>
      <dl v-if="simd" class="kv">
        <dt>Backend</dt>
        <dd>{{ live.info.backend }}</dd>
        <dt>SIMD</dt>
        <dd>{{ simd.level }} (requested {{ simd.requested }}, CPU {{ simd.cpu }}{{ simd.cpu_family ? `, ${simd.cpu_family}` : "" }})</dd>
        <dt>Threads</dt>
        <dd>{{ st?.renderer.threads }}</dd>
      </dl>
    </div>
    <div class="panel">
      <h3>Preview <Pill v-if="p" :text="p.state" :kind="p.state === 'publishing' ? 'ok' : p.state === 'error' ? 'bad' : 'neutral'" /></h3>
      <dl v-if="p" class="kv">
        <dt>Mode</dt>
        <dd>{{ p.mode }}{{ p.publish ? ` (${p.publish} MediaMTX)` : "" }}</dd>
        <template v-if="p.mode === 'webrtc'">
          <dt>Path</dt>
          <dd>{{ p.path }}</dd>
          <dt>Encoder</dt>
          <dd>{{ p.encoder || "–" }}{{ p.width ? ` · ${p.width}×${p.height} · ${p.fps} fps` : "" }}</dd>
          <dt>Pictures</dt>
          <dd>{{ p.frames }}</dd>
          <dt>Error</dt>
          <dd>{{ p.error || "–" }}</dd>
          <template v-if="p.mediamtx">
            <dt>MediaMTX</dt>
            <dd>{{ p.mediamtx.running ? "running" : "stopped" }}, {{ p.mediamtx.restarts }} restarts</dd>
          </template>
        </template>
      </dl>
    </div>
    <div class="panel">
      <h3>Output</h3>
      <dl v-if="st" class="kv">
        <dt>Frames</dt>
        <dd>{{ st.output.frames }}</dd>
        <dt>Late / missed</dt>
        <dd>{{ st.output.late }} / {{ st.output.missed }}</dd>
        <dt>Updates / skipped / torn</dt>
        <dd>{{ st.renderer.updates }} / {{ st.renderer.skipped }} / {{ st.renderer.torn }}</dd>
      </dl>
    </div>
  </div>
</template>
