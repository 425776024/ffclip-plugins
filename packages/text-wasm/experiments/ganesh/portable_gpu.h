#pragma once
#include "include/core/SkSurface.h"
#include "include/core/SkImageInfo.h"
namespace videocut::text_wasm { sk_sp<SkSurface> MakePortableSurface(const SkImageInfo &info); }
