# VideoCutVector

`VideoCutVector` owns renderer-neutral SVG/Lottie documents, controlled resource
maps, security preflight, deterministic clip-local time mapping, and the
vector render-lane port. It is a C++17 SDK target and exposes no Skia types.

The C++20 SkSVG/Skottie implementation lives in `sdk/skia_runtime`.
