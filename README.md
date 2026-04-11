# picmerge

A pure C++17 command-line tool that stitches multiple consecutive mobile
e-commerce screenshots into a single long image. Designed for edge devices:
no OpenCV, no GPU, single-threaded, and no brute-force full-resolution pixel
matching.

## What it does

Given a directory of screenshots taken while scrolling through the same page
(e.g. `1.jpg`, `2.jpg`, `3.jpg`, `4.jpg` at the same width), `picmerge`
produces a single `merge.jpg` in the current directory such that:

- The OS status bar at the top appears **once**.
- The fixed bottom nav / CTA bar appears **once**.
- Any sticky header that only becomes pinned after scrolling (search box +
  category tabs) appears **once**, at its first appearance.
- Duplicated scroll content between adjacent screenshots is removed.
- Right-edge floating badges (e.g. promo buttons) do not cause misalignment.

## Build

Requires a C++17 toolchain (MSVC 2019+, GCC 9+, Clang 10+) and CMake 3.16+.
All image dependencies (`stb_image.h`, `stb_image_write.h`) are vendored in
`third_party/stb/`.

### Cross-platform

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

### Windows (MSVC Build Tools 2019)

A convenience wrapper `build.bat` activates the MSVC developer environment
and builds with Ninja:

```
build.bat
```

The resulting executable is `build/picmerge.exe` (Windows) or
`build/picmerge` (Unix).

## Usage

```
picmerge <input_dir>
```

- Reads every `.jpg` / `.jpeg` / `.png` file in `<input_dir>`.
- Sorts them in natural order so `2.jpg` precedes `10.jpg`.
- Writes `merge.jpg` into the **current working directory**.

All inputs must share the same width and height; if not, `picmerge` exits
with a dimension-mismatch error. A single-file input is copied through
unchanged.

### Example

```
picmerge demo_pic
```

Output on the provided `demo_pic/` fixture (four 1206×2622 screenshots):

```
[info] 4 input image(s): 1.jpg 2.jpg 3.jpg 4.jpg
[info] all images are 1206x2622
[info] fixed top bar = 0 rows, bottom bar = 398 rows
[info] img[2] self sticky header = 423 rows
[info] img[3] self sticky header = 423 rows
[info] pair 0->1: overlap=468 rows, offset_in_prev=1756, cost=3501 (runner-up=226234)
[info] pair 1->2: overlap=769 rows, offset_in_prev=1455, cost=12124 (runner-up=91764)
[info] pair 2->3: overlap=493 rows, offset_in_prev=1731, cost=8148  (runner-up=66296)
[info] output dimensions: 1206x5950, 5 span(s)
[info] wrote merge.jpg (1206x5950)
```

## Algorithm overview

1. **Row signatures.** Every image is decoded once into RGB. For each row
   we compute a 16-byte fingerprint (per-bin mean over the central 50% of
   the row width), and then discard the 2D pixel buffer. All subsequent
   alignment work is done on these 1D signatures only (~42 KB per image
   at 2622 rows). This is what satisfies the "no brute-force full-res
   matching" constraint.

2. **Fixed bar detection.** Find the longest prefix and suffix of rows
   whose fingerprints match across **all** input images (within a small
   per-bin tolerance to absorb JPEG recompression noise). These become
   the single top bar and single bottom bar in the output.

3. **Sticky header detection.** For each adjacent pair `(N, N+1)`, find
   the longest `S` such that rows `[top_bar, top_bar+S)` of image `N+1`
   match those of image `N` at the same Y. That's a sticky header that
   became pinned between the two screenshots.

4. **Overlap search.** For each adjacent pair, slide a short template
   from the top of image `N+1`'s scroll region across image `N`'s scroll
   region and minimise L1 distance on the fingerprints. We keep the best
   and a de-clustered runner-up for acceptance diagnostics, and fall back
   to multiple template start positions (stepping further down image
   `N+1`) to survive the "hero carousel swap" case where the very top of
   the two images shows different content but shared scroll content
   exists lower down. If every template fails the absolute-cost threshold,
   the pair is reported as failed and falls back to direct concatenation
   with a `[warn]` on stderr.

5. **Plan and stitch.** The stitcher computes per-image contribution
   spans: image 0 contributes everything except its bottom bar; each
   subsequent image contributes only the tail below its overlap with the
   previous image (plus injected sticky-header rows the first time one
   appears, guarded on having a reliable top-bar boundary). Finally, the
   bottom bar from image 0 is appended. A single output buffer is
   pre-allocated; images are decoded one at a time, their contributing
   rows are `memcpy`d into the output, and each decoded image is freed
   before the next is loaded. Peak memory is roughly one decoded input
   plus the output buffer.

6. **Encode.** The output buffer is written out as JPEG at quality 90 via
   `stb_image_write.h`.

## Exit codes

- `0` — success
- `1` — bad arguments / usage
- `2` — I/O error, decode failure, dimension mismatch, or write failure

## Project layout

```
picture/
├── CMakeLists.txt
├── build.bat                    # Windows MSVC build wrapper
├── README.md
├── prd.md                       # product requirements
├── demo_pic/                    # sample 4-image fixture
├── third_party/stb/
│   ├── stb_image.h
│   └── stb_image_write.h
└── src/
    ├── main.cpp                 # CLI entry + pipeline orchestration
    ├── image_io.{h,cpp}         # RAII stb wrapper, load/save
    ├── row_signature.{h,cpp}    # 16-bin per-row fingerprint
    ├── bar_detector.{h,cpp}     # global top/bottom fixed bar detection
    ├── sticky_header.{h,cpp}    # per-pair sticky header detection
    ├── overlap_finder.{h,cpp}   # 1D sliding-window L1 overlap search
    └── stitcher.{h,cpp}         # plan + streamed memcpy assembly
```

## Limitations

- Inputs must share the same width and height.
- EXIF rotation is not honoured; rotate inputs beforehand if needed.
- No photometric blending at seams — the assumption is that the input
  frames were captured on the same device in the same session, so
  exposure is consistent.
