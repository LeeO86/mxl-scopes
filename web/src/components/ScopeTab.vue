<script setup>
// Scope (§9.1): the live preview, the live controls and the measurements (§4.7).
import { computed, ref } from "vue";
import { live, MODES, patchScope, pct } from "../api.js";
import Field from "./Field.vue";
import ScopeView from "./ScopeView.vue";

const error = ref("");
const s = computed(() => live.scope || {});
const sampling = computed(() => `${s.value.line_step ?? 1}/${s.value.pixel_step ?? 1}`);
const m = computed(() => live.measurements);

async function set(change) {
  try {
    error.value = "";
    await patchScope(change);
  } catch (e) {
    error.value = e.message;
  }
}
function setSampling(value) {
  const [line, pixel] = value.split("/").map(Number);
  set({ line_step: line, pixel_step: pixel });
}
</script>

<template>
  <div class="scope-layout">
    <div class="panel">
      <h3>Scope</h3>
      <ScopeView />
      <h3 style="margin-top: 0.9rem">Measurements <span v-if="m && (m.luma || m.chroma)" class="muted small">(codes ±{{ (m.codes_per_bin / 2).toFixed(1) }})</span></h3>
      <dl class="kv">
        <template v-if="m?.luma">
          <dt>Luma min / max</dt>
          <dd>{{ m.luma.min_code }} / {{ m.luma.max_code }}</dd>
          <dt>Luma mean</dt>
          <dd>{{ m.luma.mean_code.toFixed(1) }}</dd>
          <dt>Luma below 64 / above 940</dt>
          <dd>{{ pct(m.luma.below_64_ratio) }} / {{ pct(m.luma.above_940_ratio) }}</dd>
        </template>
        <template v-if="m?.chroma">
          <dt>Cb/Cr outside 64–960</dt>
          <dd>{{ pct(m.chroma.out_of_range_ratio) }}</dd>
        </template>
        <template v-if="m?.rgb">
          <dt>R′G′B′ out of gamut (EBU R 103)</dt>
          <dd>{{ pct(m.rgb.out_of_gamut_ratio) }}</dd>
        </template>
        <template v-if="!m || (!m.luma && !m.chroma && !m.rgb)">
          <dt>–</dt>
          <dd>no measurement</dd>
        </template>
      </dl>
    </div>
    <div class="panel">
      <h3>Controls</h3>
      <Field label="Mode">
        <select :value="s.mode" @change="set({ mode: $event.target.value })">
          <option v-for="mode in MODES" :key="mode.id" :value="mode.id">{{ mode.title }}</option>
        </select>
      </Field>
      <Field label="Update rate" help="The overlays run at most 30 Hz (25 Hz from 50p).">
        <select :value="String(s.update_rate)" @change="set({ update_rate: $event.target.value })">
          <option value="frame">every frame</option>
          <option v-for="hz in ['50', '25', '12.5', '10', '5', '1']" :key="hz" :value="hz">{{ hz }} Hz</option>
        </select>
      </Field>
      <Field label="Accumulate">
        <select :value="s.accumulate" @change="set({ accumulate: $event.target.value })">
          <option value="latest">newest frame per update</option>
          <option value="all">every frame between updates</option>
        </select>
      </Field>
      <Field label="Sampling">
        <select :value="sampling" @change="setSampling($event.target.value)">
          <option value="1/1">full (every line, every sample)</option>
          <option value="2/1">half (every 2nd line)</option>
          <option value="2/2">quarter (every 2nd line and sample)</option>
        </select>
      </Field>
      <Field :label="`Intensity ${s.intensity ?? ''} %`">
        <input type="range" min="1" max="400" :value="s.intensity" @input="set({ intensity: Number($event.target.value) })" />
      </Field>
      <Field label="Graticule units">
        <select :value="s.graticule_units" @change="set({ graticule_units: $event.target.value })">
          <option value="mv">mV</option>
          <option value="percent">%</option>
          <option value="code">code</option>
        </select>
      </Field>
      <Field :label="`Graticule level ${s.graticule_level ?? ''} %`">
        <input type="range" min="0" max="100" :value="s.graticule_level" @input="set({ graticule_level: Number($event.target.value) })" />
      </Field>
      <Field label="Trace colour" help="Luma waveform only.">
        <select :value="s.trace_color" @change="set({ trace_color: $event.target.value })">
          <option value="green">green</option>
          <option value="white">white</option>
          <option value="amber">amber</option>
        </select>
      </Field>
      <Field label="Range">
        <select :value="s.range" @change="set({ range: $event.target.value })">
          <option value="limited">BT.709 limited</option>
          <option value="full">full</option>
        </select>
      </Field>
      <div v-if="error" class="msg err">{{ error }}</div>
      <div v-else-if="live.notPersisted.length" class="note">Set in the environment, applied until the restart: {{ live.notPersisted.join(", ") }}</div>
    </div>
  </div>
</template>
