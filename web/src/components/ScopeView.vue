<script setup>
// The scope picture: the WebRTC preview, else HLS (where the browser plays it), else the PNG
// snapshot (SCOPES_PREVIEW_MODE=off, or while WebRTC is down). Emits `shown` at the first picture.
import { computed, onMounted, onUnmounted, ref, watch } from "vue";
import { live } from "../api.js";
import { nativeHls, preview, previewUrl, releasePreview, usePreview } from "../preview.js";

const props = defineProps({ fill: { type: Boolean, default: false }, snapshotMs: { type: Number, default: 500 } });
const emit = defineEmits(["shown"]);

const video = ref(null);
const snapshot = ref("");
let timer = 0;

const source = computed(() => {
  if (preview.state === "playing" && preview.stream) return "webrtc";
  if (live.info?.preview?.mode !== "off" && preview.state === "error" && nativeHls()) return "hls";
  return "snapshot";
});

function refresh() {
  if (source.value === "snapshot" && !document.hidden) {
    const next = new Image();
    next.onload = () => {
      snapshot.value = next.src;
      emit("shown");
    };
    next.src = `/api/v1/snapshot.png?scale=2&t=${Date.now()}`;
  }
  timer = setTimeout(refresh, props.snapshotMs);
}

watch([source, () => preview.stream, video], () => {
  const el = video.value;
  if (!el) return;
  if (source.value === "webrtc") {
    el.srcObject = preview.stream;
  } else if (source.value === "hls") {
    el.srcObject = null;
    el.src = previewUrl("hls");
  }
  el.play?.().catch(() => {});
});

onMounted(() => {
  usePreview();
  refresh();
});
onUnmounted(() => {
  clearTimeout(timer);
  releasePreview();
});
</script>

<template>
  <div class="scope-view" :class="{ fill }">
    <video v-show="source !== 'snapshot'" ref="video" muted autoplay playsinline @playing="emit('shown')"></video>
    <img v-if="source === 'snapshot' && snapshot" :src="snapshot" alt="scope picture" />
    <div v-if="source === 'snapshot'" class="scope-note">
      {{ live.info?.preview?.mode === "off" ? "preview off · snapshot" : preview.error ? `snapshot · ${preview.error}` : "snapshot" }}
    </div>
  </div>
</template>
