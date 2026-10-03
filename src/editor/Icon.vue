<script setup lang="ts">
withDefaults(defineProps<{ name: string; size?: number }>(), { size: 16 });
const paths: Record<string, string[]> = {
  text: ['M4 5h16M12 5v15M8 20h8M4 5v3M20 5v3'],
  media: ['M4 5h16v14H4z', 'M8 2H2v14', 'm5 15 4-5 3 3 2-2 5 5', 'M15 8h.01'],
  video: ['M3 5h14v14H3z', 'm17 10 5-3v10l-5-3'],
  music: [
    'M9 18V5l11-2v13',
    'M9 5v5l11-2',
    'M9 18c0 2-6 4-6 0s6-4 6 0',
    'M20 16c0 2-6 4-6 0s6-4 6 0'
  ],
  image: ['M3 3h18v18H3z', 'm3 16 6-6 4 4 3-3 5 5', 'M16 7h.01'],
  folder: ['M3 7V4h6l2 3h10v13H3z'],
  update: ['M20 7a8 8 0 0 0-13-3L3 8', 'M3 3v5h5', 'M4 17a8 8 0 0 0 13 3l4-4', 'M21 21v-5h-5'],
  settings: [
    'M12 3v3m0 12v3M3 12h3m12 0h3M5.6 5.6l2.1 2.1m8.6 8.6 2.1 2.1M5.6 18.4l2.1-2.1m8.6-8.6 2.1-2.1',
    'M16 12a4 4 0 1 1-8 0 4 4 0 0 1 8 0'
  ],
  sliders: ['M4 6h6m4 0h6M4 12h10m4 0h2M4 18h2m4 0h10', 'M10 3v6m4 0v6m-8 0v6'],
  undo: ['m8 4-5 5 5 5', 'M3 9h11a7 7 0 0 1 0 14'],
  redo: ['m16 4 5 5-5 5', 'M21 9H10a7 7 0 0 0 0 14'],
  cut: [
    'M9 6a3 3 0 1 1-6 0 3 3 0 0 1 6 0',
    'M9 18a3 3 0 1 1-6 0 3 3 0 0 1 6 0',
    'm8 8 13 13M8 16 21 3'
  ],
  trash: ['M4 6h16M9 6V3h6v3M6 6l1 15h10l1-15M10 10v7m4-7v7'],
  cursor: ['m5 3 14 9-7 1-3 7z'],
  magnet: ['M5 4v9a7 7 0 0 0 14 0V4h-4v9a3 3 0 0 1-6 0V4z', 'M5 8h4m6 0h4'],
  link: [
    'm9 15 6-6',
    'm7 13-2 2a4 4 0 0 0 6 6l3-3a4 4 0 0 0 0-6',
    'm17 11 2-2a4 4 0 0 0-6-6l-3 3a4 4 0 0 0 0 6'
  ],
  unlink: [
    'm8 13-3 3a3 3 0 0 0 4 4l3-3',
    'm16 11 3-3a3 3 0 0 0-4-4l-3 3',
    'M8 3v3H5m11 15v-3h3',
    'm3 3 18 18'
  ],
  group: [
    'M8 3H3v5m13-5h5v5M3 16v5h5m13-5v5h-5',
    'M7 7h6v6H7zM11 11h6v6h-6z'
  ],
  ungroup: [
    'M3 3h7v7H3zM14 14h7v7h-7z',
    'M15 3h6v6m0-6-7 7M3 15v6h6m-6 0 7-7'
  ],
  rippleDelete: ['M2 7h5v10H2zM17 7h5v10h-5z', 'm10 10 4 4m0-4-4 4', 'm5 3 3 2-3 2m14 10-3 2 3 2'],
  eye: ['M2 12s4-7 10-7 10 7 10 7-4 7-10 7S2 12 2 12', 'M15 12a3 3 0 1 1-6 0 3 3 0 0 1 6 0'],
  eyeOff: ['m3 3 18 18', 'M6 6c-3 2-4 6-4 6s4 7 10 7c2 0 4-1 5-2M10 5c6-1 12 7 12 7s-1 2-3 4'],
  volume: ['M3 9h4l5-4v14l-5-4H3z', 'M16 8c2 2 2 6 0 8m3-11c4 4 4 10 0 14'],
  mute: ['M3 9h4l5-4v14l-5-4H3z', 'm17 9 5 6m0-6-5 6'],
  lock: ['M5 10h14v11H5z', 'M8 10V6a4 4 0 0 1 8 0v4', 'M12 14v3'],
  unlock: ['M5 10h14v11H5z', 'M8 10V6a4 4 0 0 1 8 0', 'M12 14v3'],
  play: ['m8 4 12 8-12 8z'],
  pause: ['M8 4v16M16 4v16'],
  previous: ['M5 5v14m13-14-9 7 9 7z'],
  next: ['M19 5v14M6 5l9 7-9 7z'],
  left: ['m14 6-6 6 6 6'],
  right: ['m10 6 6 6-6 6'],
  down: ['m6 9 6 6 6-6'],
  up: ['m6 15 6-6 6 6'],
  plus: ['M12 5v14M5 12h14'],
  minus: ['M5 12h14'],
  close: ['m6 6 12 12M6 18 18 6'],
  search: ['M17 10a7 7 0 1 1-14 0 7 7 0 0 1 14 0', 'm15 15 6 6'],
  zoomIn: ['M17 10a7 7 0 1 1-14 0 7 7 0 0 1 14 0', 'm15 15 6 6M7 10h6m-3-3v6'],
  zoomOut: ['M17 10a7 7 0 1 1-14 0 7 7 0 0 1 14 0', 'm15 15 6 6M7 10h6'],
  reset: ['M3 11a9 9 0 1 1 2 7M3 4v7h7'],
  export: ['M5 13v7h14v-7M12 16V2m-5 5 5-5 5 5'],
  import: ['M5 13v7h14v-7M12 2v14m-5-5 5 5 5-5'],
  grid: ['M3 3h7v7H3zM14 3h7v7h-7zM3 14h7v7H3zM14 14h7v7h-7z'],
  list: ['M8 5h13M8 12h13M8 19h13M3 5h.01M3 12h.01M3 19h.01'],
  monitor: ['M2 3h20v14H2zM8 21h8m-4-4v4'],
  layers: ['m12 3 10 6-10 6L2 9z', 'm2 13 10 6 10-6M2 17l10 6 10-6'],
  fit: ['M3 8V3h5m8 0h5v5M3 16v5h5m8 0h5v-5', 'M8 8h8v8H8z'],
  layout: ['M3 3h18v18H3zM9 3v18M9 15h12'],
  duplicate: ['M8 8h13v13H8zM16 8V3H3v13h5'],
  diamond: ['m12 3 9 9-9 9-9-9z'],
  time: ['M21 12a9 9 0 1 1-18 0 9 9 0 0 1 18 0', 'M12 6v6l4 2'],
  code: ['m8 6-6 6 6 6m8-12 6 6-6 6M14 3l-4 18'],
  info: ['M21 12a9 9 0 1 1-18 0 9 9 0 0 1 18 0', 'M12 11v6m0-10h.01']
};
</script>
<template>
  <svg
    :width="size"
    :height="size"
    viewBox="0 0 24 24"
    fill="none"
    stroke="currentColor"
    stroke-width="1.7"
    stroke-linecap="round"
    stroke-linejoin="round"
    aria-hidden="true"
  >
    <path v-for="(path, i) in paths[name] || paths.media" :key="i" :d="path" />
  </svg>
</template>
