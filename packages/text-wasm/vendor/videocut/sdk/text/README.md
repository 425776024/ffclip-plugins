# VideoCutText

`VideoCutText` owns the renderer-neutral rich-text document, font references,
template validation/instantiation, deterministic animation sampling, layout
DTOs, and the text render-lane port.

Public headers are C++17 and deliberately contain no Skia, ICU, HarfBuzz,
FreeType, protobuf, Editor, or Flutter types. The Skia implementation lives in
`sdk/skia_runtime`.
