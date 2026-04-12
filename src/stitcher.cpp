// stitcher.cpp

#include "stitcher.h"
#include "image_io.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

namespace picmerge {

namespace {

inline size_t row_bytes(int width) {
    return static_cast<size_t>(width) * static_cast<size_t>(kChannels);
}

// Append a non-empty span to the plan, merging adjacent spans from the same
// image when possible (keeps memcpy count low).
void push_span(std::vector<Contribution>& parts,
               int image_index, int y_begin, int y_end) {
    if (y_end <= y_begin) return;
    if (!parts.empty() &&
        parts.back().image_index == image_index &&
        parts.back().y_end == y_begin) {
        parts.back().y_end = y_end;
        return;
    }
    parts.push_back({image_index, y_begin, y_end});
}

} // namespace

StitchPlan plan_stitch(int width,
                       int image_height,
                       int num_images,
                       int top_bar,
                       int bottom_bar,
                       int bar_ref_image,
                       const std::vector<int>& self_sticky,
                       const std::vector<OverlapResult>& overlaps) {
    StitchPlan plan;
    plan.width = width;
    plan.top_bar = top_bar;
    plan.bottom_bar = bottom_bar;

    if (num_images <= 0) return plan;

    const int usable_end = image_height - bottom_bar;  // exclusive

    // Top bar comes from the bar-reference image (the one most
    // representative of the majority). Content from img[0] starts right
    // after it.
    if (top_bar > 0 && bar_ref_image != 0)
        push_span(plan.parts, bar_ref_image, 0, top_bar);

    // img[0] contributes everything except the bottom bar (and the top bar
    // if it was already emitted from the bar-reference image above).
    push_span(plan.parts, 0, (top_bar > 0 && bar_ref_image != 0) ? top_bar : 0, usable_end);

    for (int i = 1; i < num_images; ++i) {
        const OverlapResult& ov = overlaps[static_cast<size_t>(i - 1)];

        // Sticky-header injection: if image i is the first in the sequence
        // where a sticky header appears (self_sticky[i] > self_sticky[i-1]),
        // emit img[i]'s sticky header rows once.
        //
        // Guard: we only inject when `top_bar > 0`, i.e. when we have a
        // reliable OS-status-bar boundary. Without that boundary, "sticky
        // header rows" detected from y=0 will include themed status bars,
        // and injecting them paints a second "20:46" into the middle of
        // the output. When `top_bar == 0`, the safer choice is to skip
        // injection entirely — subsequent images' top regions are then
        // never emitted, so no ghost status bar can leak in.
        const int prev_sticky = self_sticky[static_cast<size_t>(i - 1)];
        const int curr_sticky = self_sticky[static_cast<size_t>(i)];
        if (top_bar > 0 && curr_sticky > prev_sticky) {
            push_span(plan.parts, i,
                      top_bar + prev_sticky,
                      top_bar + curr_sticky);
        }

        int content_begin;
        if (ov.ok) {
            // Rows [template_start_in_next .. template_start_in_next + overlap_height)
            // of img[i] are already covered by img[i-1]'s contribution.
            const int overlap_height = usable_end - ov.offset_in_prev;
            content_begin = ov.template_start_in_next + overlap_height;
        } else {
            // Degraded path: no reliable overlap → concatenate starting
            // right below the top bar + sticky-shared region.
            content_begin = top_bar + std::max(curr_sticky, prev_sticky);
        }

        // Clamp: never go backwards and never exceed usable_end.
        if (content_begin < top_bar + curr_sticky) content_begin = top_bar + curr_sticky;
        if (content_begin > usable_end)            content_begin = usable_end;

        push_span(plan.parts, i, content_begin, usable_end);
    }

    // Bottom bar once, from the bar-reference image.
    push_span(plan.parts, bar_ref_image, usable_end, image_height);

    // Compute total height.
    int total = 0;
    for (const auto& p : plan.parts) total += (p.y_end - p.y_begin);
    plan.height = total;
    return plan;
}

bool execute_stitch(const StitchPlan& plan,
                    const std::vector<std::string>& input_paths,
                    const std::string& output_path,
                    int jpeg_quality) {
    if (plan.width <= 0 || plan.height <= 0) {
        std::fprintf(stderr, "[error] invalid plan dimensions %dx%d\n",
                     plan.width, plan.height);
        return false;
    }

    const size_t rb = row_bytes(plan.width);
    const size_t total_bytes = rb * static_cast<size_t>(plan.height);

    std::unique_ptr<uint8_t[]> out;
    try {
        out.reset(new uint8_t[total_bytes]);
    } catch (const std::bad_alloc&) {
        std::fprintf(stderr,
                     "[error] failed to allocate %zu bytes for output buffer\n",
                     total_bytes);
        return false;
    }

    // Group contributions by image index so we only decode each input once.
    // Within the pass for image i, we copy every span owned by i. We go
    // through images in ascending index order; for each, we scan the plan
    // for any span belonging to that image.
    //
    // This keeps peak memory at: sizeof(output buffer) + sizeof(one decoded
    // image). We never hold two decoded inputs simultaneously.

    const int num_images = static_cast<int>(input_paths.size());
    for (int i = 0; i < num_images; ++i) {
        // Early skip if no span for this image (unlikely but harmless).
        bool needed = false;
        for (const auto& c : plan.parts) {
            if (c.image_index == i) { needed = true; break; }
        }
        if (!needed) continue;

        Image img;
        if (!img.load(input_paths[static_cast<size_t>(i)])) {
            std::fprintf(stderr, "[error] failed to decode %s\n",
                         input_paths[static_cast<size_t>(i)].c_str());
            return false;
        }
        if (img.width() != plan.width) {
            std::fprintf(stderr,
                         "[error] image %s width %d != plan width %d\n",
                         input_paths[static_cast<size_t>(i)].c_str(),
                         img.width(), plan.width);
            return false;
        }

        // Walk the plan, copying spans that belong to image i. We maintain a
        // running output y cursor by counting bytes written to each span's
        // position in the output buffer.
        int out_y = 0;
        for (const auto& span : plan.parts) {
            const int span_rows = span.y_end - span.y_begin;
            if (span.image_index == i && span_rows > 0) {
                const uint8_t* src = img.row(span.y_begin);
                uint8_t* dst = out.get() + static_cast<size_t>(out_y) * rb;
                std::memcpy(dst, src, rb * static_cast<size_t>(span_rows));
            }
            out_y += span_rows;
        }

        // Free immediately — we're done with this image's pixels.
        img.reset();
    }

    if (!write_jpeg(output_path, plan.width, plan.height, out.get(), jpeg_quality)) {
        std::fprintf(stderr, "[error] failed to write %s\n", output_path.c_str());
        return false;
    }
    return true;
}

} // namespace picmerge
