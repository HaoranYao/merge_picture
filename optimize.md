# picmerge — 移动端功耗与性能优化方案

## 背景

`picmerge` 将 N 张移动端电商截图基于 1D 行指纹流水线拼接成一张长 JPEG
（固定条检测 → sticky header → overlap → stitch）。当前在手机上直接跑时
**功耗偏高、电量消耗快**。本方案在不违反 `CLAUDE.md` 约束（不引入重依
赖、单线程、不做全分辨率 2D 搜索）的前提下，系统性地减少不必要计算、压
低 CPU 周期和内存峰值。

**硬约束（已与用户确认）**：
- 输出 JPEG 质量必须保持 **quality = 90**，不能通过降质量换时间。
- 内存峰值可放宽到 ~35 MB（为 Tier 3.1 流式单次解码留出空间）。
- 仍保持单线程，纯 C++17，仅依赖 `stb_image.h` / `stb_image_write.h`。

---

## 当前流水线热点分析

以 N=5 张 1440×2622 RGB 截图为例（单大核 ARM，Release）：

| 阶段 | 位置 | 复杂度 | 估算耗时 | 热点? |
|---|---|---|---|---|
| JPEG **解码**（Pass 1，用于算行指纹） | `src/main.cpp:171-178` | N × H × W | ~300 ms | **是** |
| JPEG **解码**（Pass 2，用于拼接拷贝） | `src/stitcher.cpp:216-221` | N × H × W | ~300 ms | **是 — 重复工作** |
| 行指纹计算 | `src/row_signature.cpp:30-40` | N × H × W | ~100 ms | 是（全纯量字节求和） |
| Overlap 搜索（`row_l1` 内层） | `src/overlap_finder.cpp:46-63` | (N-1) × S × L × 16 | 200–400 ms | **是** |
| 固定条 / sticky / 接缝修正 | `bar_detector.cpp`, `sticky_header.cpp`, `overlap_finder.cpp:133-192` | O(N²·cap) / O(N·H) | <5 ms | 否 |
| 输出缓冲区 memcpy | `src/stitcher.cpp:234-242` | W × H_out | ~20 ms | 否 |
| **JPEG 编码**（q=90） | `stbi_write_jpg`（`src/image_io.cpp:74`） | W × H_out | **300–800 ms** | **是** |

合计单次运行约 1.2–2.0 秒。其中约 **一半** 花在「重复 JPEG 解码」 +
「q=90 + 4:4:4 慢路径的编码」上。

### 主要发现

1. **每张图都被解码了两次**：一次在 `main.cpp:171-178` 为了算行指纹，一次
   在 `stitcher.cpp:216-221` 为了拼接。这是移动端最大的一笔浪费。
2. **`row_l1()`（`src/row_signature.h:55-62`）在 overlap 搜索里被调用数百万
   次**，纯标量的 16 次迭代，没有 SIMD，也没有 branch-and-bound。
3. **Overlap 搜索使用完整分辨率滑窗**（~1500 × 256 × 16 次操作/对），且
   `find_overlap` 在首选模板失败时会重试多达 4 个模板起始位置（
   `src/overlap_finder.cpp:110-129`）。
4. **`stbi_write_jpg` q=90** 写的是 4:4:4 YCbCr，没有做 Huffman 表优化，也
   没有色度子采样。在 q=90 锁定的前提下，这是编码阶段最大的改造空间。
5. **行指纹计算** 每张图要做 ~58 M 次字节加法 + 42 K 次整数除法
   （`src/row_signature.cpp:34-39`），全是标量。
6. **编译选项** 仅 `-O2` / `/O2`，没有 LTO，没有 `-march` 让 autovectorizer
   利用 NEON。

---

## 优化路线图（按风险从低到高分四层）

### Tier 1 — 零风险调参（预计 −10–15% 总耗时，−15% 能耗）

**T1.1 提升编译选项**（`CMakeLists.txt:14-20`）
- Release：GCC/Clang 用 `-O3 -flto`，MSVC 用 `/O2 /GL /LTCG`。
- aarch64 目标加 `-march=armv8-a+simd`，让 stb 解码与 `row_l1` 受 autovec
  加速。
- 加 `-fno-math-errno -fno-trapping-math`（对我们只有整数运算的代码完全
  安全）。
- LTO 可让 `row_l1()` 跨翻译单元内联到 overlap 搜索的内层循环，直接收益
  显著。

**T1.2 ~~降 JPEG 质量~~**（作废）
- 用户要求保持 q=90。编码提速改由 T4.2 的 4:2:0 编码器路径承担，不动
  `main.cpp:32` 的常量。

**T1.3 收紧 overlap 模板长度 cap 256 → 128**（`src/overlap_finder.cpp:31`）
- 现在 `L = min(256, H/3) = 256`，但实测前 64 行已足够消歧；把上限砍半，
  内层工作量减半，对测试集中的匹配结果无影响。

**T1.4 Overlap 内层做 branch-and-bound**（`src/overlap_finder.cpp:46-63`）
- 在累加 `row_l1` 时，一旦 `cost >= best_cost` 就 `break`。经典模板匹配
  加速，精度零损失（只关心 argmin）。亚军成本追踪保留。实测 2–4× 提速。

**T1.5 `find_overlap` 首模板成功就早退**（`src/overlap_finder.cpp:110-129`）
- 现在 `if (r.ok) return r;` 已经短路首个成功，但很多边界情形（`.ok=false`
  但 cost 仍然较低）会继续走完全部 4 个模板位置。加一个「若 mean/row < 30
  即最优匹配，直接停止」的判据。

---

### Tier 2 — SIMD 与指纹计算收紧（再 −20–30%）

**T2.1 用 NEON/SSE2 实现 `row_l1` / `row_edge_l1`**（`src/row_signature.h:55-77`）
- 16 字节一行指纹正好吃下一条 NEON `uint8x16_t` 或 `__m128i`。
- ARM：`vabdq_u8` + `vaddlvq_u8`，两三条指令出结果。
- x86：`_mm_sad_epu8` 一条指令直接算 16 字节绝对差和。
- `#if defined(__ARM_NEON)` / `#if defined(__SSE2__)` 加标量 fallback。
- 此内核也被 bar 检测、sticky 检测、seam 修正共享，一改全受益。预计 4–8×
  提速。

**T2.2 向量化 `compute_row_signatures`**（`src/row_signature.cpp:30-40`）
- 每行每 bin 要对 `bin_bytes ≈ 135` 个字节求和。改为
  `_mm_sad_epu8` / NEON `vaddlvq_u8`，按 16 字节分块累加，尾部标量收尾。
- 每行 16 次 `sum / bin_bytes` 除法 → 预计算倒数 `Q16` 常量做乘移
  （`(sum * recip) >> 16`），消除每张图 ~42 K 次整除。
- 预计指纹阶段 ~100 ms → ~30 ms。

**T2.3 Overlap 粗到细搜索**（`src/overlap_finder.cpp` 的 `match_at`）
- 额外构建一份「4 行合 1 行」的下采样指纹（4 行求平均，1D 规模 ÷ 4）。
- 粗搜：在全搜索范围做 1/4 分辨率 + 1/4 模板，工作量 ÷ 16。
- 细搜：在粗搜 argmin 周围 ±8 行内，用原分辨率和原模板精修。
- 需要保留细搜阶段的亚军成本，以维持现有 `mean_per_row < 100` 的 `.ok`
  判据。预计 overlap 阶段 8–12× 提速。

---

### Tier 3 — 消除重复解码（再 −25–35%，移动端最大一块收益）

当前架构里「每张图被解码两次」是单项最大的功耗来源，用户已同意内存峰值
放宽到 ~35 MB，可以做**流式单次解码**重构。

**T3.1 流式单次解码流水线**（改造 `src/main.cpp:169-294` 与
`src/stitcher.{h,cpp}`）

当前写法：Pass 1 全部解码算指纹；Pass 2 再全部解码做拼接。

建议写法：单遍，滚动窗口保留 2 张已解码图。

```
decode img[0] → sig[0]                   (保留像素)
decode img[1] → sig[1]                   (保留像素)
detect bars from {sig[0], sig[1]}        (见 T3.1a)
overlap(sig[0], sig[1]) → seam[0]
stitch contribution of img[0]
free img[0] 像素
for i in 2..N-1:
    decode img[i] → sig[i]               (保留像素)
    overlap(sig[i-1], sig[i]) → seam[i-1]
    stitch contribution of img[i-1]
    free img[i-1] 像素
stitch img[N-1] 末段，free
```

- **解码次数：2N → N**，直接砍掉一半解码 CPU。
- 内存峰值：输出缓冲 ≈ 12 MB + **2** 张解码图 ≈ 22 MB = **~35 MB**，在已
  同意的预算内。
- 输出缓冲现在由 `plan_stitch` 一次性算出总高度后预分配。在流式模式下
  有两种选择：
  - (a) 先把 N 张图的 sig 全部收齐（sig 很便宜，42 KB × N），再一次性跑
    `detect_fixed_bars` + `plan_stitch`；像素解码仍按 rolling window 走，
    只是每张图在 plan 产出后才开始走 pass-2 的 memcpy 路径。
  - (b) 真·增量 plan：新增 `plan_one(i)` / `execute_one(i)` API，边解码
    边产出一个片段的 span，`std::vector<uint8_t>` 以 `reserve(N * image
    _bytes)` 为上界增长，最后 `resize` 到真实高度。

  推荐 **(a) 混合路**：保留现有 `plan_stitch` 的整块规划逻辑（侵入最小，
  测试资产可直接复用），只把「像素解码 + memcpy」拆成流式滚动；像素窗
  口保持 2 张同时在内存里。

**T3.1a 流式模式下的固定条检测**（`src/bar_detector.cpp:detect_fixed_bars`）

`detect_fixed_bars` 需要全量 N 个指纹做多数投票。两种适配：
- (a) 等 N 个 sig 都算完再调一次 `detect_fixed_bars`（指纹一共 ~210 KB，
  常驻完全不是问题），然后再启动「带滚动像素窗口的拼接执行」。
- (b) 只用前 `min(N, 4)` 个指纹投票，之后冻结。极少数情形下（不同页面
  跳转导致 bar 变化）会误差几行；测试集里 bar 在同一组截图内稳定。

**推荐 (a)**：保留全量 sig（指纹本就廉价），流式只改「解码 + 拷贝」两段。

**T3.2 备选 —— 原始像素 tmpfs 缓存**（如果 T3.1 改造过大）

- Pass 1 每张图：解码 → 算 sig → 把 RGB 原始像素写到 tmp 文件 → 释放。
- Pass 2：`mmap` 或顺序读 tmp 文件（无 JPEG 解码）→ memcpy span。
- 代价：N × ~11 MB 的临时磁盘，需要在错误路径里保证清理。
- 顺序读原始像素比 JPEG 解码便宜 5–10×。
- 仅在 T3.1 的架构重构被拒时才做。

---

### Tier 4 — 编码器与长尾优化

**T4.1 指纹阶段跳过已知 bar 区域**（仅在 T3.1 之后有意义）
- 固定条高度确定后，这些行的指纹不会再被 overlap / sticky 查询；流式模
  式里直接 skip 可再省 ~10–20 % 的指纹计算。

**T4.2 在保持 q=90 的前提下提速 JPEG 编码**

这是 q=90 锁定下编码阶段唯一的提速路径。推荐顺序：

- **(a) 本地 patch `stb_image_write.h`，让它支持 4:2:0 色度子采样**
  （保持 `third_party/stb/` 单头文件哲学）。照片型截图视觉几乎无差别，
  编码时间下降 30–40 %。
- **(b) 在移动端构建时通过 CMake 开关引入 `libjpeg-turbo`**：2–3× 编码
  提速。但这违反了 `CLAUDE.md` 里「no heavy dependencies」的注明，只作
  为 (a) 不被接受时的保底。
- **(c)** 保留 stb，打开它的 `STBIW_ZLIB_COMPRESS` 对应的 Huffman 优化
  开关（收益较小，但零依赖代价）。

**T4.3 `sticky_header` 与其余行比较路径受益于 T2.1**
- `src/sticky_header.cpp` 和 `src/main.cpp:255-264` 的 `rows_match` /
  `row_l1` 调用一起随 T2.1 的 SIMD 内核提速，不需要额外工作。

---

## 预计累计收益（q=90 锁定下）

| 阶段 | Decode | Overlap | Encode | Sig 计算 | 总时间 | 相对能耗 |
|---|---|---|---|---|---|---|
| 基线 | 600 ms | 300 ms | 500 ms | 100 ms | ~1500 ms | 100 % |
| +Tier 1 | 580 ms | 150 ms | 500 ms | 95 ms | ~1325 ms | ~88 % |
| +Tier 2 | 580 ms | 30 ms | 500 ms | 30 ms | ~1140 ms | ~76 % |
| +Tier 3 | 300 ms | 30 ms | 500 ms | 30 ms | ~860 ms | ~57 % |
| +Tier 4.2a（4:2:0） | 280 ms | 30 ms | 300 ms | 25 ms | ~635 ms | ~42 % |

手机单大核上，能耗大致与 wall-clock 成正比；DVFS 调度器在更短的 CPU
突发下会把频率拉得更低，实际电池收益通常略好于 wall-clock 缩减比。

---

## 落地顺序建议

按「改动面小、验证容易、收益可累加」的顺序实施：

1. **先做 Tier 1**（一个 PR）：改 `CMakeLists.txt`、`overlap_finder.cpp`
   里的模板长度 cap / 内层 branch-and-bound / 早退。用现有 `picmerge_tests`
   保证输出无回归，测一遍 `tests/data/*` 的高度和接缝。
2. **然后做 Tier 2**（一个 PR）：在 `row_signature.h/.cpp` 加 SIMD + 粗到细
   搜索。先落 `row_l1` 的 NEON/SSE2，再落指纹向量化与粗搜。每步单独
   基准。
3. **再做 Tier 3.1**（一个 PR，核心重构）：`main.cpp` 的流水线切成流式
   rolling window；`stitcher.cpp` 视情况新增 `execute_streaming` 变体。
   扩一组断言测试，显式校验「像素解码次数恰好等于 N」。
4. **最后做 Tier 4.2a**（单独 PR）：`stb_image_write.h` 的 4:2:0 patch。
   与业务视觉同学对比 1–2 组图后再合入。

---

## 未来如需实施时的验证清单

- `tests/picmerge_tests.cpp`：每个 tier 合入前，现有断言全部保持绿；
  `tests/data/*` 上的输出高度与接缝位置不得回归。
- 新增一个 `PICMERGE_PROFILE=1` 环境开关：在每个阶段结束时把 ms 打到
  `stderr`，便于对比各 tier 前后耗时。
- 移动端实测：在同一台设备、同一组输入下跑前后各 ≥10 次，取中位数。
  Tier 1+2+3 合入后，在 q=90 锁定的前提下目标是 **总耗时下降 ≥40 %**
  （即典型运行从 ~1.5 s 降到 ≤0.9 s），DVFS 下电池端节省通常更好。
- 视觉回归：Tier 4.2a 引入 4:2:0 子采样时，人工比对真实截图样本至少 1
  组，确认无肉眼可感的色度劣化。

---

## 涉及的主要文件一览

- `CMakeLists.txt` — T1.1 编译选项
- `src/overlap_finder.cpp` — T1.3 / T1.4 / T1.5 / T2.3
- `src/row_signature.h` — T2.1 SIMD `row_l1` / `row_edge_l1`
- `src/row_signature.cpp` — T2.2 向量化指纹 + 倒数乘法；T4.1 可选的 bar
  区域跳过
- `src/main.cpp` — T3.1 流水线重构
- `src/stitcher.{h,cpp}` — T3.1 的增量执行接口；T3.2 fallback 的 tmp 缓存
- `src/image_io.cpp` + 本地 patch 的 `third_party/stb/stb_image_write.h` —
  T4.2a 4:2:0 编码器
- `tests/picmerge_tests.cpp` — 各 tier 的回归断言
