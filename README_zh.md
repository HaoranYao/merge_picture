# picmerge

纯 C++17 命令行工具，将多张连续的手机电商截图拼接成一张完整长图。专为端侧运行设计：无 OpenCV 依赖、无 GPU、单线程，不做全分辨率暴力像素匹配。

## 功能说明

给定一组在同一页面滚动截取的图片（如 `1.jpg`、`2.jpg`、`3.jpg`、`4.jpg`，宽度相同），`picmerge` 将在输入图片所在目录生成 `merge_<时间戳>.jpg`，满足：

- 顶部 OS 状态栏**只出现一次**
- 底部固定导航栏 / CTA 按钮栏**只出现一次**
- 滚动后才出现的粘性头部（搜索框 + 分类标签）**只保留首次出现**
- 相邻截图中重叠的滚动内容被自动去除
- 右侧悬浮徽标（如促销按钮）不会导致对齐错误
- 接缝处的浮窗污染（如"快要抢光"横幅）通过接缝优化自动规避

## 构建

需要 C++17 工具链（MSVC 2019+、GCC 9+、Clang 10+）和 CMake 3.16+。
所有图像依赖（`stb_image.h`、`stb_image_write.h`）已内置于 `third_party/stb/`。

### 跨平台

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

### Windows（MSVC Build Tools 2019）

`build.bat` 会自动激活 MSVC 开发者环境并使用 Ninja 编译：

```
build.bat
```

产物为 `build/picmerge.exe`（Windows）或 `build/picmerge`（Unix）。

## 用法

```
picmerge <输入目录>
```

- 读取 `<输入目录>` 中所有 `.jpg` / `.jpeg` / `.png` 文件
- 按自然序排序（`2.jpg` 排在 `10.jpg` 前面）
- 将结果写入与输入图片**相同目录**下，文件名为 `merge_<时间戳>.jpg`
- 已有的 `merge_*.jpg` 文件会被自动跳过，不参与输入

所有输入图片必须具有相同的宽度和高度，否则程序报错退出。单张图片输入时直接转存为输出。

### 子目录批量处理

若输入目录包含子目录，每个子目录会被**独立处理**并各自生成一个 `merge_<时间戳>.jpg`：

```
picmerge demo_pic
```

将分别处理 `demo_pic/jingdong1/`、`demo_pic/taobao1/` 等。

### 示例输出

```
[dir] demo_pic/pdd1 — 9 image(s)
[info] all images are 1206x2622
[info] fixed top bar = 0 rows, bottom bar = 309 rows (bar ref = img[7])
[info] img[2] self sticky header = 245 rows
[info] pair 0->1: overlap=786 rows, offset_in_prev=1527, cost=4172 (runner-up=174019), seam_trim=78
[info] pair 1->2: overlap=690 rows, offset_in_prev=1623, cost=4083 (runner-up=52468)
[info] output dimensions: 1206x12415, 10 span(s)
[info] wrote demo_pic/pdd1/merge_1776035592.jpg (1206x12415)
```

`seam_trim=78` 表示接缝被上移 78 行以规避浮窗遮挡。

### 调试模式

设置环境变量 `PICMERGE_DEBUG_BARS=1` 可输出底部 Bar 检测的逐行 L1 值，便于诊断问题：

```
PICMERGE_DEBUG_BARS=1 picmerge demo_pic/pdd1
```

## 算法概述

1. **行签名计算**：每张图解码为 RGB 后，对每行计算一个 16 字节指纹（取行中央 50% 宽度的各通道均值，按 16 个分桶统计）。之后丢弃像素数据，所有对齐操作均在 1D 签名上进行（每张图约 42 KB）。

2. **固定 Bar 检测**：在所有输入图之间寻找签名完全一致的最长前缀（顶部 Bar）和最长后缀（底部 Bar）。采用 L1 总和阈值（而非逐 bin 最大值）以容忍 JPEG 重压缩噪声和渐变背景差异。底部 Bar 支持多数投票，允许少数图片因悬浮窗遮挡而异常。

3. **粘性头部检测**：对每对相邻图 `(N, N+1)`，找出最长的 `S` 使图 `N+1` 顶部 `[top_bar, top_bar+S)` 行与图 `N` 相同位置完全匹配，即为滚动后固定的粘性头部。

4. **重叠偏移搜索**：对每对相邻图，在图 `N` 的滚动区域内滑动来自图 `N+1` 的短模板，最小化指纹 L1 距离，找到最优对齐偏移。支持多模板起始位置重试，应对"轮播图切换"等动态头部场景。

5. **接缝优化**：在已找到的重叠区底部检测高 L1 区域（浮窗污染特征）。若发现脏尾，将接缝上移至干净区域，由下一张图补上被遮挡的部分。无污染时行为与原始算法完全一致。

6. **规划与拼接**：计算每张图的贡献行范围。图 0 贡献除底部 Bar 外的全部内容；后续每张图贡献接缝以下的新内容（以及首次出现的粘性头部行）。预先分配单一输出缓冲区，逐张图解码、`memcpy` 贡献行后立即释放，峰值内存约为一张输入图 + 输出缓冲。

7. **JPEG 编码**：通过 `stb_image_write.h` 以质量 90 写出输出文件。

## 退出码

| 退出码 | 含义 |
|--------|------|
| `0` | 成功 |
| `1` | 参数错误 / 用法错误 |
| `2` | I/O 错误、解码失败、尺寸不匹配或写入失败 |

## 项目结构

```
picture/
├── CMakeLists.txt
├── build.bat                    # Windows MSVC 构建脚本
├── README.md                    # 英文文档
├── README_zh.md                 # 中文文档（本文件）
├── prd.md                       # 产品需求文档
├── demo_pic/                    # 示例图片集（多个 App 场景）
├── third_party/stb/
│   ├── stb_image.h
│   └── stb_image_write.h
└── src/
    ├── main.cpp                 # CLI 入口 + 流水线编排
    ├── image_io.{h,cpp}         # stb 解/编码封装（RAII）
    ├── row_signature.{h,cpp}    # 16-bin 行指纹计算
    ├── bar_detector.{h,cpp}     # 全局顶/底固定 Bar 检测
    ├── sticky_header.{h,cpp}    # 逐对粘性头部检测
    ├── overlap_finder.{h,cpp}   # 1D 滑窗 L1 重叠搜索 + 接缝优化
    └── stitcher.{h,cpp}         # 拼接规划 + 流式 memcpy 组装
```

## 限制

- 所有输入图片必须具有相同的宽度和高度
- 不处理 EXIF 旋转信息，如需旋转请预先处理
- 接缝处不做光度融合（假设同一设备同一会话截取，曝光一致）
