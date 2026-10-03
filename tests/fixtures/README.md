# Native regression fixture

`native-companion-sync-mismatch.vcut` is a real Project Format 1 directory created
with the SDK bridge. Its video track has `syncLocked=true`, while its embedded
audio companion has `syncLocked=false`. The native document itself is valid, but
the browser cannot collapse these tracks without losing the independent setting;
import must reject it.

The package contains only binary project metadata and synthetic media facts. It
references `/tmp/videocut-synthetic-sync.mp4`; no media or font bytes are included.
It is intentionally excluded from the npm distribution along with all tests.
