# VideoCutSkiaRuntime

`VideoCutSkiaRuntime` is the private C++20 implementation of the C++17
`VideoCutText` and `VideoCutVector` render ports. It owns SkParagraph, SkSVG and
Skottie objects and publishes CPU `videocut::frame::VideoFrame` overlays.

No Skia type is exposed by a public header. The target is always configured
and consumes the exact artifact package selected by
`VIDEOCUT_SKIA_ARTIFACT_ROOT`; configuration fails rather than producing an SDK
without text/vector animation support when that package is absent.

The CPU profile always rasterizes explicit premultiplied sRGB RGBA8888 and
performs one deterministic premultiplied-to-straight conversion before a
frame reaches the existing compositor.
