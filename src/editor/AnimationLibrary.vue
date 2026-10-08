<script setup lang="ts">
import type { PagxContent } from '../../packages/core/types';
import PagxLibrary from './PagxLibrary.vue';
defineProps<{ busy: boolean }>();
const emit = defineEmits<{ pagx: [pagx: PagxContent, name: string] }>();
</script>
<template>
  <PagxLibrary :busy="busy" @insert="(content, name) => emit('pagx', content, name)" />
</template>
