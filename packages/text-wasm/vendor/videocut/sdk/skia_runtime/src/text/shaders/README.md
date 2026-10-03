# Text shader sources

These are product-owned sources used by the existing text pipelines:

- `material/*.sksl`: execution-graph materials.
- `post/*.sksl`: text post effects.
- `gradient/*.sksl`: gradient LUT sampling.
- `metal/*.metal`: native Metal text passes.

Edit these files directly. Do not add shader strings to C++ or Objective-C++, or
embed Lua/JavaScript programs. Reference scripts describe behavior; production
evaluation remains in the typed C++ IR and existing render graph.

`cmake/TextShaderSources.cmake` lists the sources and embeds them during CMake
configuration into headers under the existing target's binary directory. Each
source is a configure dependency, so edits trigger regeneration; unchanged
headers retain their timestamps. There is no runtime file lookup or separate
shader installation requirement. Generated headers are not source files to edit.

SkSL consumers call `GetTextRuntimeProgram(TextRuntimeShader::...)`. Its shared
implementation owns one lazy, thread-safe cache entry per shader, including
compilation errors. Uniform values, texture bindings, coordinate conversion,
render targets and pass ordering stay in their existing pipeline. New programs
must have matching entries in the enum, source table and CMake source list.

The initial extraction preserved all 83 shader bodies byte-for-byte. This does
not validate the algorithms: current capability changes still require the
frozen 61-template batch and numeric comparison prescribed by the master plan.
