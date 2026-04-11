// main.cpp — picmerge CLI entry point.
//
// Usage: picmerge <input_dir>
//
// Reads every .jpg / .jpeg / .png file in <input_dir>, sorts them in natural
// order (so 2.jpg < 10.jpg), and writes a stitched `merge.jpg` into the
// current working directory.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "bar_detector.h"
#include "image_io.h"
#include "overlap_finder.h"
#include "row_signature.h"
#include "sticky_header.h"
#include "stitcher.h"

namespace fs = std::filesystem;

namespace {

constexpr int kJpegQuality = 90;
constexpr const char* kOutputName = "merge.jpg";

bool has_image_extension(const fs::path& p) {
    std::string ext = p.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png";
}

// Natural-order comparator: split filename into alternating digit / non-digit
// chunks and compare chunk-wise (numbers as integers, text as case-insensitive
// strings). Makes "2.jpg" sort before "10.jpg".
bool natural_less(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const bool da = std::isdigit(static_cast<unsigned char>(a[i])) != 0;
        const bool db = std::isdigit(static_cast<unsigned char>(b[j])) != 0;
        if (da && db) {
            // Compare whole digit runs numerically.
            size_t ia = i, jb = j;
            while (ia < a.size() && std::isdigit(static_cast<unsigned char>(a[ia]))) ++ia;
            while (jb < b.size() && std::isdigit(static_cast<unsigned char>(b[jb]))) ++jb;
            // Strip leading zeros when comparing by length.
            size_t la_start = i, lb_start = j;
            while (la_start + 1 < ia && a[la_start] == '0') ++la_start;
            while (lb_start + 1 < jb && b[lb_start] == '0') ++lb_start;
            const size_t la = ia - la_start, lb = jb - lb_start;
            if (la != lb) return la < lb;
            for (size_t k = 0; k < la; ++k) {
                if (a[la_start + k] != b[lb_start + k])
                    return a[la_start + k] < b[lb_start + k];
            }
            i = ia; j = jb;
        } else {
            char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
            char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[j])));
            if (ca != cb) return ca < cb;
            ++i; ++j;
        }
    }
    return a.size() < b.size();
}

int usage() {
    std::fprintf(stderr, "Usage: picmerge <input_dir>\n");
    std::fprintf(stderr,
                 "  Reads all .jpg/.jpeg/.png files in <input_dir>, sorts\n"
                 "  them in natural order, and writes merge.jpg to the\n"
                 "  current directory.\n");
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    using namespace picmerge;

    if (argc != 2) return usage();

    const std::string input_dir = argv[1];
    std::error_code ec;
    if (!fs::is_directory(input_dir, ec)) {
        std::fprintf(stderr, "[error] not a directory: %s\n", input_dir.c_str());
        return 2;
    }

    // ---- 1. Enumerate + natural sort -------------------------------------
    std::vector<std::string> paths;
    for (const auto& entry : fs::directory_iterator(input_dir, ec)) {
        if (ec) {
            std::fprintf(stderr, "[error] directory iteration failed: %s\n",
                         ec.message().c_str());
            return 2;
        }
        if (!entry.is_regular_file()) continue;
        if (!has_image_extension(entry.path())) continue;
        paths.push_back(entry.path().string());
    }
    if (paths.empty()) {
        std::fprintf(stderr, "[error] no image files found in %s\n",
                     input_dir.c_str());
        return 2;
    }
    std::sort(paths.begin(), paths.end(),
              [](const std::string& a, const std::string& b) {
                  return natural_less(fs::path(a).filename().string(),
                                      fs::path(b).filename().string());
              });

    std::fprintf(stdout, "[info] %zu input image(s):\n", paths.size());
    for (const auto& p : paths) {
        std::fprintf(stdout, "         %s\n", p.c_str());
    }

    // ---- 2. Probe metadata; fail fast on dimension mismatch ---------------
    int ref_w = 0, ref_h = 0;
    if (!probe_image(paths[0], ref_w, ref_h) || ref_w <= 0 || ref_h <= 0) {
        std::fprintf(stderr, "[error] cannot read image metadata: %s\n",
                     paths[0].c_str());
        return 2;
    }
    for (size_t i = 1; i < paths.size(); ++i) {
        int w = 0, h = 0;
        if (!probe_image(paths[i], w, h)) {
            std::fprintf(stderr, "[error] cannot read image metadata: %s\n",
                         paths[i].c_str());
            return 2;
        }
        if (w != ref_w || h != ref_h) {
            std::fprintf(stderr,
                         "[error] dimension mismatch: %s is %dx%d, expected %dx%d\n",
                         paths[i].c_str(), w, h, ref_w, ref_h);
            return 2;
        }
    }
    std::fprintf(stdout, "[info] all images are %dx%d\n", ref_w, ref_h);

    // ---- Trivial case: single image --------------------------------------
    if (paths.size() == 1) {
        Image img;
        if (!img.load(paths[0])) {
            std::fprintf(stderr, "[error] failed to decode %s\n", paths[0].c_str());
            return 2;
        }
        if (!write_jpeg(kOutputName, img.width(), img.height(), img.data(), kJpegQuality)) {
            return 2;
        }
        std::fprintf(stdout, "[info] wrote %s (%dx%d, single image)\n",
                     kOutputName, img.width(), img.height());
        return 0;
    }

    // ---- 3. Compute row signatures for every image -----------------------
    std::vector<RowSignatures> sigs(paths.size());
    for (size_t i = 0; i < paths.size(); ++i) {
        Image img;
        if (!img.load(paths[i])) {
            std::fprintf(stderr, "[error] failed to decode %s\n", paths[i].c_str());
            return 2;
        }
        sigs[i] = compute_row_signatures(img);
        // `img` goes out of scope → pixels freed. We only keep the (tiny)
        // signature vector alive for planning.
    }

    // ---- 4. Detect fixed top / bottom bars --------------------------------
    const FixedBars bars = detect_fixed_bars(sigs);
    std::fprintf(stdout, "[info] fixed top bar = %d rows, bottom bar = %d rows\n",
                 bars.top_height, bars.bottom_height);

    const int usable_end = ref_h - bars.bottom_height;

    // ---- 5. Per-pair sticky header detection -----------------------------
    // sticky_pair[k] for k in [1, N-1]: sticky height between img[k-1] and img[k]
    const int max_sticky = static_cast<int>((ref_h - bars.top_height - bars.bottom_height) * 0.40);
    std::vector<int> sticky_pair(paths.size(), 0);
    for (size_t k = 1; k < paths.size(); ++k) {
        sticky_pair[k] = detect_sticky_header(sigs[k - 1], sigs[k],
                                              bars.top_height, bars.bottom_height,
                                              max_sticky);
    }

    // Per-image self sticky height: an image's sticky header is visible to
    // its neighbours as "identical rows at same Y". Take the max over both
    // neighbour pairs to catch the first-appearance case.
    std::vector<int> self_sticky(paths.size(), 0);
    for (size_t k = 0; k < paths.size(); ++k) {
        int s = 0;
        if (k >= 1)                   s = std::max(s, sticky_pair[k]);
        if (k + 1 < paths.size())     s = std::max(s, sticky_pair[k + 1]);
        self_sticky[k] = s;
    }
    for (size_t k = 0; k < paths.size(); ++k) {
        if (self_sticky[k] > 0) {
            std::fprintf(stdout, "[info] img[%zu] self sticky header = %d rows\n",
                         k, self_sticky[k]);
        }
    }

    // ---- 6. Per-pair overlap detection -----------------------------------
    std::vector<OverlapResult> overlaps(paths.size() - 1);
    for (size_t k = 0; k + 1 < paths.size(); ++k) {
        const int prev_sticky = self_sticky[k];
        const int next_sticky = self_sticky[k + 1];
        const int shared = std::max(prev_sticky, next_sticky);
        const int next_template_start = bars.top_height + shared;
        const int prev_search_begin   = bars.top_height + prev_sticky;

        overlaps[k] = find_overlap(sigs[k], sigs[k + 1],
                                   prev_search_begin, usable_end,
                                   next_template_start, usable_end);

        if (overlaps[k].ok) {
            const int overlap_h = usable_end - overlaps[k].offset_in_prev;
            std::fprintf(stdout,
                         "[info] pair %zu->%zu: overlap=%d rows, "
                         "offset_in_prev=%d, cost=%.0f (runner-up=%.0f)\n",
                         k, k + 1, overlap_h, overlaps[k].offset_in_prev,
                         overlaps[k].best_cost, overlaps[k].second_best_cost);
        } else {
            std::fprintf(stderr,
                         "[warn] overlap detection failed between img[%zu] "
                         "and img[%zu]; falling back to direct concat "
                         "(best cost=%.0f, runner-up=%.0f)\n",
                         k, k + 1, overlaps[k].best_cost, overlaps[k].second_best_cost);
        }
    }

    // Signatures no longer needed.
    sigs.clear();
    sigs.shrink_to_fit();

    // ---- 7. Plan and execute the stitch ----------------------------------
    const StitchPlan plan = plan_stitch(ref_w, ref_h,
                                        static_cast<int>(paths.size()),
                                        bars.top_height, bars.bottom_height,
                                        self_sticky, overlaps);
    std::fprintf(stdout, "[info] output dimensions: %dx%d, %zu span(s)\n",
                 plan.width, plan.height, plan.parts.size());

    if (!execute_stitch(plan, paths, kOutputName, kJpegQuality)) {
        return 2;
    }
    std::fprintf(stdout, "[info] wrote %s (%dx%d)\n",
                 kOutputName, plan.width, plan.height);
    return 0;
}
