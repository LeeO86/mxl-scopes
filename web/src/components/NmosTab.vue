<script setup>
// NMOS (§9.1): node, registration, receiver and sender, output flow; IS-05 activation and disable of
// the receiver, sent to the Connection API on NMOS_PORT.
import { computed, ref } from "vue";
import { live, shortId } from "../api.js";
import Pill from "./Pill.vue";

const info = computed(() => live.info);
const nmos = computed(() => live.status?.nmos);
const out = computed(() => live.status?.output);
const domain = ref("");
const flow = ref("");
const msg = ref({ kind: "", text: "" });

async function stage(body) {
  const port = info.value?.ports?.nmos;
  const url = `${location.protocol}//${location.hostname}:${port}/x-nmos/connection/v1.2/single/receivers/${info.value.receiver_id}/staged`;
  try {
    const resp = await fetch(url, { method: "PATCH", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
    if (!resp.ok) throw new Error(`IS-05 answered ${resp.status}: ${(await resp.text()).slice(0, 200)}`);
    msg.value = { kind: "ok", text: "Activated." };
  } catch (e) {
    msg.value = { kind: "err", text: e.message };
  }
}
function activate() {
  stage({
    sender_id: null,
    master_enable: true,
    activation: { mode: "activate_immediate" },
    transport_params: [{ mxl_domain_id: domain.value.trim(), mxl_flow_id: flow.value.trim() }],
  });
}
function disable() {
  stage({ master_enable: false, activation: { mode: "activate_immediate" } });
}
</script>

<template>
  <div class="grid wide">
    <div class="panel">
      <h3>Node <Pill v-if="nmos" :text="nmos.enabled ? (nmos.registered ? 'registered' : 'not registered') : 'off'" :kind="nmos.registered ? 'ok' : 'neutral'" /></h3>
      <dl v-if="info" class="kv">
        <dt>Node</dt>
        <dd :title="info.node_id">{{ info.node_id }}</dd>
        <dt>Device</dt>
        <dd>{{ info.device_id }}</dd>
        <dt>Receiver "Scopes In Video"</dt>
        <dd>{{ info.receiver_id }}</dd>
        <dt>Sender "Scopes Out Video"</dt>
        <dd>{{ info.sender_id }}</dd>
        <dt>Registry</dt>
        <dd>{{ nmos?.registry || "none" }}</dd>
        <dt>NMOS port</dt>
        <dd>{{ info.ports.nmos }}</dd>
      </dl>
    </div>
    <div class="panel">
      <h3>Output flow</h3>
      <dl v-if="out" class="kv">
        <dt>Flow</dt>
        <dd>{{ out.flow_id || "–" }}</dd>
        <dt>Domain</dt>
        <dd>{{ out.domain_id }}</dd>
        <dt>Format</dt>
        <dd>{{ out.width }}×{{ out.height }} @ {{ out.rate }}</dd>
        <dt>Media type</dt>
        <dd>{{ out.media_type }}</dd>
        <dt>Frames / late / missed</dt>
        <dd>{{ out.frames }} / {{ out.late }} / {{ out.missed }}</dd>
      </dl>
    </div>
    <div class="panel">
      <h3>IS-05 receiver</h3>
      <p class="note">Routes the receiver ({{ shortId(info?.receiver_id) }}) to an MXL flow, or disables it. A controller normally does this.</p>
      <label for="is05-domain">MXL domain id</label>
      <input id="is05-domain" v-model="domain" placeholder="uuid" />
      <label for="is05-flow">MXL flow id</label>
      <input id="is05-flow" v-model="flow" placeholder="uuid" />
      <div class="actions">
        <span class="msg" :class="msg.kind" style="margin: 0">{{ msg.text }}</span>
        <span class="spacer"></span>
        <button class="btn secondary" @click="disable">Disable</button>
        <button class="btn" :disabled="!domain.trim() || !flow.trim()" @click="activate">Activate</button>
      </div>
    </div>
  </div>
</template>
