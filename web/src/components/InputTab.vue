<script setup>
// Input (§9.1): state, routed sender and flow, format, colorimetry, counters of the renderer.
import { computed } from "vue";
import { live, stateKind } from "../api.js";
import Pill from "./Pill.vue";

const input = computed(() => live.status?.input);
const r = computed(() => live.status?.renderer);
</script>

<template>
  <div class="grid wide">
    <div class="panel">
      <h3>Input <Pill v-if="input" :text="input.state.replace('_', ' ')" :kind="stateKind(input.state)" /></h3>
      <dl v-if="input" class="kv">
        <dt>Reason</dt>
        <dd>{{ input.reason || "–" }}</dd>
        <dt>Label</dt>
        <dd>{{ input.label || "–" }}</dd>
        <dt>Sender</dt>
        <dd>{{ input.route.sender_id || "–" }}</dd>
        <dt>Flow</dt>
        <dd>{{ input.route.flow_id || "–" }}</dd>
        <dt>Domain</dt>
        <dd>{{ input.route.domain_id || "–" }}</dd>
        <dt>Format</dt>
        <dd>{{ input.width ? `${input.width}×${input.height} ${input.interlaced ? "i" : "p"} ${input.rate}` : "–" }}</dd>
        <dt>Media type</dt>
        <dd>{{ input.media_type || "–" }}</dd>
        <dt>Colorimetry</dt>
        <dd>{{ input.colorimetry || "–" }}<span v-if="input.colorimetry_warning" class="badge env">shown as BT.709</span></dd>
        <dt>Grains</dt>
        <dd>{{ input.grains }}</dd>
        <dt>Resyncs</dt>
        <dd>{{ input.resyncs }}</dd>
        <dt>Latency</dt>
        <dd>{{ input.latency_ms.toFixed(1) }} ms</dd>
      </dl>
    </div>
    <div class="panel">
      <h3>Updates</h3>
      <dl v-if="r" class="kv">
        <dt>Updates</dt>
        <dd>{{ r.updates }}</dd>
        <dt>Skipped / torn</dt>
        <dd>{{ r.skipped }} / {{ r.torn }}</dd>
        <dt>Update rate</dt>
        <dd>{{ r.update_rate_hz.toFixed(2) }} Hz</dd>
        <dt>Analyse / render</dt>
        <dd>{{ r.analyse_ms.toFixed(2) }} / {{ r.render_ms.toFixed(2) }} ms</dd>
        <dt>Line step</dt>
        <dd>{{ r.line_step }}</dd>
        <dt>SIMD, threads</dt>
        <dd>{{ r.simd }}, {{ r.threads }}</dd>
      </dl>
    </div>
  </div>
</template>
