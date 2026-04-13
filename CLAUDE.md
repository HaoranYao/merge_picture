# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

**Cross-platform (CMake):**
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

**Windows (MSVC Build Tools 2019):**
```
build.bat
```
This script activates the MSVC developer environment and builds with Ninja (falls back to NMake if Ninja unavailable).

Executable output: `build/picmerge.exe` (Windows) or `build/picmerge` (Unix).

## Usage

```
picmerge <input_dir>
```

- Reads all `.jpg/.jpeg/.png` files in `<input_dir>` (or its subdirectories)
- Sorts files in natural order (`2.jpg` before `10.jpg`)
- Outputs `merge_<timestamp>.jpg` in each processed directory
- Skips existing `merge_*.jpg` files to avoid re-processing outputs

**Debug mode:** Set `PICMERGE_DEBUG_BARS=1` environment variable to output bar detection diagnostics.

## Architecture Overview

This is a pure C++17 command-line tool for stitching consecutive mobile e-commerce screenshots into a single long image. Key design constraints (from PRD):

1. **No heavy dependencies** — Only `stb_image.h` and `stb_image_write.h` (vendored in `third_party/stb/`). No OpenCV.
2. **No brute-force pixel matching** — All alignment uses 1D row fingerprints (~42 KB per image at 2622 rows) instead of full-resolution 2D search.
3. **Single-threaded** — Designed for edge devices; no GPU, no parallelization.
4. **Memory-efficient** — Peak memory ≈ one decoded input + output buffer. Images are decoded one at a time, contribution rows are copied to output, then freed immediately.

## Pipeline Stages (in `main.cpp`)

1. **Enumerate + natural sort** — Collect image paths, sort by filename
2. **Probe metadata** — Verify all images share same width/height
3. **Row signatures** (`row_signature.cpp`) — Compute 16-byte fingerprint per row (central 50% width, binned means)
4. **Fixed bar detection** (`bar_detector.cpp`) — Find longest prefix/suffix of identical rows across ALL images (top/bottom bars)
5. **Sticky header detection** (`sticky_header.cpp`) — Per-pair detection of headers that appear only after scrolling
6. **Overlap search** (`overlap_finder.cpp`) — 1D sliding-window L1 distance to find scroll offset between adjacent pairs. Includes seam refinement to avoid floating UI artifacts.
7. **Stitch** (`stitcher.cpp`) — Compute contribution spans, stream pixels to output buffer, encode as JPEG

## Key Components

| File | Purpose |
|------|---------|
| `src/main.cpp` | CLI entry, pipeline orchestration, directory handling |
| `src/image_io.{h,cpp}` | RAII wrapper around stb_image for load/save |
| `src/row_signature.{h,cpp}` | 16-bin per-row fingerprint computation |
| `src/bar_detector.{h,cpp}` | Global top/bottom fixed bar detection with majority voting |
| `src/sticky_header.{h,cpp}` | Per-pair sticky header height detection |
| `src/overlap_finder.{h,cpp}` | 1D sliding L1 overlap search + seam refinement for floating UI |
| `src/stitcher.{h,cpp}` | Plan contribution spans, execute streamed memcpy assembly |

## Core Algorithm Details

- **Row fingerprint**: 16 bins × central 50% row width → compact signature for matching
- **Bar detection**: Uses L1 sum threshold (not per-bin max) to tolerate JPEG noise and gradient backgrounds; supports majority voting when some images have temporary overlays
- **Overlap matching**: Multiple template start positions to survive dynamic content (carousel swaps); reports best + runner-up cost for diagnostics
- **Seam refinement**: Detects high-L1 dirty tail in overlap (floating UI), moves seam upward to use cleaner pixels from next image
- **Stitch execution**: Pre-allocate output buffer, decode inputs one-by-one, memcpy contribution spans, free each input before loading next

## Exit Codes

- `0` — success
- `1` — bad arguments / usage error
- `2` — I/O error, decode failure, dimension mismatch, or write failure