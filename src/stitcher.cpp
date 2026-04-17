// stitcher.cpp

#include "stitcher.h"
#include "image_io.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace picmerge {

namespace {

struct PlacedContribution {
    Contribution part;
    int out_y = 0;
};

class ScopedProfile {
public:
    explicit ScopedProfile(const char* label)
        : label_(label), enabled_(profile_enabled()),
          start_(enabled_ ? std::chrono::steady_clock::now()
                          : std::chrono::steady_clock::time_point()) {}

    ~ScopedProfile() {
        if (!enabled_) return;
        const auto elapsed = std::chrono::steady_clock::now() - start_;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        std::fprintf(stderr, "[profile] %s: %lld ms\n",
                     label_, static_cast<long long>(ms));
    }

private:
    static bool profile_enabled() {
        static const bool enabled = []() {
            const char* env = std::getenv("PICMERGE_PROFILE");
            return env && *env && std::strcmp(env, "0") != 0;
        }();
        return enabled;
    }

    const char* label_;
    bool enabled_;
    std::chrono::steady_clock::time_point start_;
};

inline size_t row_bytes(int width) {
    return static_cast<size_t>(width) * static_cast<size_t>(kChannels);
}

void push_span(std::vector<Contribution>& parts,
               int image_index,
               int y_begin,
               int y_end) {
    if (y_end <= y_begin) return;
    if (!parts.empty() &&
        parts.back().image_index == image_index &&
        parts.back().y_end == y_begin) {
        parts.back().y_end = y_end;
        return;
    }
    parts.push_back({image_index, y_begin, y_end});
}

std::vector<PlacedContribution> place_parts(const StitchPlan& plan) {
    std::vector<PlacedContribution> placed;
    placed.reserve(plan.parts.size());
    int out_y = 0;
    for (const auto& part : plan.parts) {
        placed.push_back({part, out_y});
        out_y += part.y_end - part.y_begin;
    }
    return placed;
}

bool validate_plan(const StitchPlan& plan) {
    if (plan.width <= 0 || plan.height <= 0) {
        std::fprintf(stderr, "[error] invalid plan dimensions %dx%d\n",
                     plan.width, plan.height);
        return false;
    }
    return true;
}

std::unique_ptr<uint8_t[]> allocate_output_buffer(const StitchPlan& plan) {
    const size_t total_bytes =
        row_bytes(plan.width) * static_cast<size_t>(plan.height);
    std::unique_ptr<uint8_t[]> out;
    try {
        out.reset(new uint8_t[total_bytes]);
    } catch (const std::bad_alloc&) {
        std::fprintf(stderr,
                     "[error] failed to allocate %zu bytes for output buffer\n",
                     total_bytes);
        return nullptr;
    }
    return out;
}

bool image_needed(const std::vector<PlacedContribution>& placed, int image_index) {
    for (const auto& span : placed) {
        if (span.part.image_index == image_index && span.part.y_end > span.part.y_begin) {
            return true;
        }
    }
    return false;
}

bool copy_image_spans(const Image& img,
                      const std::vector<PlacedContribution>& placed,
                      int image_index,
                      uint8_t* out,
                      size_t rb) {
    for (const auto& span : placed) {
        const int span_rows = span.part.y_end - span.part.y_begin;
        if (span.part.image_index != image_index || span_rows <= 0) continue;
        const uint8_t* src = img.row(span.part.y_begin);
        uint8_t* dst = out + static_cast<size_t>(span.out_y) * rb;
        std::memcpy(dst, src, rb * static_cast<size_t>(span_rows));
    }
    return true;
}

bool copy_raw_spans(const std::string& raw_path,
                    const std::vector<PlacedContribution>& placed,
                    int image_index,
                    uint8_t* out,
                    size_t rb) {
    std::FILE* file = std::fopen(raw_path.c_str(), "rb");
    if (!file) {
        std::fprintf(stderr, "[error] failed to open raw cache %s\n", raw_path.c_str());
        return false;
    }

    bool ok = true;
    for (const auto& span : placed) {
        const int span_rows = span.part.y_end - span.part.y_begin;
        if (span.part.image_index != image_index || span_rows <= 0) continue;

        const long offset = static_cast<long>(
            rb * static_cast<size_t>(span.part.y_begin));
        if (std::fseek(file, offset, SEEK_SET) != 0) {
            std::fprintf(stderr, "[error] failed to seek raw cache %s\n", raw_path.c_str());
            ok = false;
            break;
        }

        uint8_t* dst = out + static_cast<size_t>(span.out_y) * rb;
        const size_t bytes = rb * static_cast<size_t>(span_rows);
        if (std::fread(dst, 1, bytes, file) != bytes) {
            std::fprintf(stderr, "[error] failed to read raw cache %s\n", raw_path.c_str());
            ok = false;
            break;
        }
    }

    std::fclose(file);
    return ok;
}

} // namespace

StitchPlan plan_stitch(int width,
                       int image_height,
                       int num_images,
                       int top_bar,
                       int bottom_bar,
                       int bar_ref_image,
                       const std::vector<int>& self_sticky,
                       const std::vector<int>& fallback_skip,
                       const std::vector<OverlapResult>& overlaps) {
    StitchPlan plan;
    plan.width = width;
    plan.top_bar = top_bar;
    plan.bottom_bar = bottom_bar;

    if (num_images <= 0) return plan;

    const int usable_end = image_height - bottom_bar;

    if (top_bar > 0 && bar_ref_image != 0) {
        push_span(plan.parts, bar_ref_image, 0, top_bar);
    }

    for (int i = 0; i < num_images; ++i) {
        int content_begin = 0;
        if (i == 0) {
            content_begin = (top_bar > 0 && bar_ref_image != 0) ? top_bar : 0;
        } else {
            const OverlapResult& prev_ov = overlaps[static_cast<size_t>(i - 1)];
            const int prev_sticky = self_sticky[static_cast<size_t>(i - 1)];
            const int curr_sticky = self_sticky[static_cast<size_t>(i)];

            if (top_bar > 0 && curr_sticky > prev_sticky && !prev_ov.ok) {
                push_span(plan.parts, i,
                          top_bar + prev_sticky,
                          top_bar + curr_sticky);
            }

            if (prev_ov.ok) {
                const int seam_offset = prev_ov.seam_in_prev - prev_ov.offset_in_prev;
                content_begin = prev_ov.template_start_in_next + seam_offset;
            } else if (i == num_images - 1) {
                content_begin = top_bar + curr_sticky;
            } else {
                const int unified_skip = std::max(
                    fallback_skip[static_cast<size_t>(i - 1)],
                    fallback_skip[static_cast<size_t>(i)]);
                content_begin = top_bar + unified_skip;
            }

            if (content_begin < top_bar + curr_sticky) content_begin = top_bar + curr_sticky;
            if (content_begin > usable_end) content_begin = usable_end;
        }

        int content_end = usable_end;
        if (i < num_images - 1 && overlaps[static_cast<size_t>(i)].ok) {
            content_end = overlaps[static_cast<size_t>(i)].seam_in_prev;
        } else if (i < num_images - 1) {
            if (i + 1 == num_images - 1) {
                content_end = usable_end;
            } else {
                const int next_skip = fallback_skip[static_cast<size_t>(i + 1)];
                content_end = usable_end - next_skip;
            }
            if (content_end < content_begin) content_end = content_begin;
        }
        if (content_end < content_begin) content_end = content_begin;

        push_span(plan.parts, i, content_begin, content_end);
    }

    push_span(plan.parts, bar_ref_image, usable_end, image_height);

    int total = 0;
    for (const auto& p : plan.parts) total += (p.y_end - p.y_begin);
    plan.height = total;
    return plan;
}

bool execute_stitch(const StitchPlan& plan,
                    const std::vector<std::string>& input_paths,
                    const std::string& output_path,
                    int jpeg_quality) {
    if (!validate_plan(plan)) return false;

    const auto placed = place_parts(plan);
    const size_t rb = row_bytes(plan.width);
    std::unique_ptr<uint8_t[]> out = allocate_output_buffer(plan);
    if (!out) return false;

    {
        ScopedProfile profile("stitch copy");
        const int num_images = static_cast<int>(input_paths.size());
        for (int i = 0; i < num_images; ++i) {
            if (!image_needed(placed, i)) continue;

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

            if (!copy_image_spans(img, placed, i, out.get(), rb)) {
                return false;
            }
        }
    }

    {
        ScopedProfile profile("jpeg encode");
        if (!write_jpeg(output_path, plan.width, plan.height, out.get(), jpeg_quality)) {
            std::fprintf(stderr, "[error] failed to write %s\n", output_path.c_str());
            return false;
        }
    }

    return true;
}

bool execute_stitch_from_raw_cache(const StitchPlan& plan,
                                   const std::vector<std::string>& raw_cache_paths,
                                   const std::string& output_path,
                                   int jpeg_quality) {
    if (!validate_plan(plan)) return false;
    if (raw_cache_paths.empty()) {
        std::fprintf(stderr, "[error] raw cache path list is empty\n");
        return false;
    }

    const auto placed = place_parts(plan);
    const size_t rb = row_bytes(plan.width);
    std::unique_ptr<uint8_t[]> out = allocate_output_buffer(plan);
    if (!out) return false;

    {
        ScopedProfile profile("stitch raw copy");
        const int num_images = static_cast<int>(raw_cache_paths.size());
        for (int i = 0; i < num_images; ++i) {
            if (!image_needed(placed, i)) continue;
            if (!copy_raw_spans(raw_cache_paths[static_cast<size_t>(i)],
                                placed, i, out.get(), rb)) {
                return false;
            }
        }
    }

    {
        ScopedProfile profile("jpeg encode");
        if (!write_jpeg(output_path, plan.width, plan.height, out.get(), jpeg_quality)) {
            std::fprintf(stderr, "[error] failed to write %s\n", output_path.c_str());
            return false;
        }
    }

    return true;
}

} // namespace picmerge
