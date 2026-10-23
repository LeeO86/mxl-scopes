<script setup>
// One labelled form control with an optional one-line help text.
defineProps({
  label: { type: String, required: true },
  id: { type: String, default: "" },
  help: { type: String, default: "" },
});
</script>

<template>
  <div class="field">
    <label :for="id || undefined">{{ label }}</label>
    <slot />
    <div v-if="help" class="note">{{ help }}</div>
  </div>
</template>
