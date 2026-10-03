// Adapted from @met4citizen/headtts 1.3.0; Copyright 2025 Mika Suominen, MIT.
// VideoCut 2026-10-01: remove dictionary I/O and unused runtime utilities; dictionaries are bundled offline.
export function deepCopy(value) { return JSON.parse(JSON.stringify(value)); }
export function trace(...values) { console.log(...values); }
