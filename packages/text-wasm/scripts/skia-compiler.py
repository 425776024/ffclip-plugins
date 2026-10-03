#!/usr/bin/env python3
"""Select Skia's 4-lane raster pipeline through Emscripten SSE-to-WASM SIMD.

Scope SSE feature macros to the pipeline implementation. Defining them globally
would make skcms include native AVX512 headers on a WASM target. No Skia source
or raster algorithm is patched; the compiler lowers these intrinsics to SIMD128.
"""
import os
import sys

args = sys.argv[1:]
if any(arg.replace('\\', '/').endswith('/src/core/SkOpts.cpp') for arg in args):
    args.append('-msse4.1')
if any(arg.replace('\\', '/').endswith('/src/core/SkBlurEngine.cpp') for arg in args):
    # SkVx has native WASM SIMD implementations. The default wasm build disables
    # them, scalarizing the per-channel accumulators in the Gaussian/box passes.
    # Vec size/alignment are identical; scope this to the measured blur hot path.
    args = [arg for arg in args if arg != '-DSKVX_DISABLE_SIMD']
os.execvp(args[0], args)
