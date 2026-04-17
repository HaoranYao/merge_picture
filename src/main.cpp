// main.cpp - picmerge CLI entry point.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
constexpr const char* kOutputPrefix = "merge_";

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

struct TempDirGuard {
    fs::path dir;

    ~TempDirGuard() {
        if (dir.empty()) return;
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

std::string make_output_name() {
    const auto now = std::chrono::system_clock::now();
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    return std::string(kOutputPrefix) + std::to_string(secs) + ".jpg";
}

fs::path make_cache_dir() {
    const fs::path base = fs::temp_directory_path();
    for (int attempt = 0; attempt < 16; ++attempt) {
        const auto stamp = std::chrono::high_resolution_clock::now()
                               .time_since_epoch().count();
        const fs::path candidate =
            base / ("picmerge_cache_" + std::to_string(stamp + attempt));
        std::error_code ec;
        if (fs::create_directories(candidate, ec)) return candidate;
    }
    return {};
}

bool write_raw_cache(const picmerge::Image& img, const fs::path& path) {
    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    if (!file) return false;

    const size_t bytes = static_cast<size_t>(img.width()) *
                         static_cast<size_t>(img.height()) *
                         static_cast<size_t>(picmerge::kChannels);
    const size_t written = std::fwrite(img.data(), 1, bytes, file);
    std::fclose(file);
    return written == bytes;
}

bool has_image_extension(const fs::path& p) {
    std::string ext = p.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png";
}

bool is_merge_output(const fs::path& p) {
    const std::string name = p.filename().string();
    return name.rfind(kOutputPrefix, 0) == 0;
}

bool natural_less(const std::string& a, const std::string& b) {
    size_t i = 0;
    size_t j = 0;
    while (i < a.size() && j < b.size()) {
        const bool da = std::isdigit(static_cast<unsigned char>(a[i])) != 0;
        const bool db = std::isdigit(static_cast<unsigned char>(b[j])) != 0;
        if (da && db) {
            size_t ia = i;
            size_t jb = j;
            while (ia < a.size() && std::isdigit(static_cast<unsigned char>(a[ia]))) ++ia;
            while (jb < b.size() && std::isdigit(static_cast<unsigned char>(b[jb]))) ++jb;

            size_t la_start = i;
            size_t lb_start = j;
            while (la_start + 1 < ia && a[la_start] == '0') ++la_start;
            while (lb_start + 1 < jb && b[lb_start] == '0') ++lb_start;

            const size_t la = ia - la_start;
            const size_t lb = jb - lb_start;
            if (la != lb) return la < lb;
            for (size_t k = 0; k < la; ++k) {
                if (a[la_start + k] != b[lb_start + k]) {
                    return a[la_start + k] < b[lb_start + k];
                }
            }
            i = ia;
            j = jb;
        } else {
            const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
            const char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[j])));
            if (ca != cb) return ca < cb;
            ++i;
            ++j;
        }
    }
    return a.size() < b.size();
}

int usage() {
    std::fprintf(stderr, "Usage: picmerge <input_dir>\n");
    std::fprintf(stderr,
                 "  Reads all .jpg/.jpeg/.png files in <input_dir>, sorts\n"
                 "  them in natural order, and writes merge_<timestamp>.jpg\n"
                 "  alongside the input images. If <input_dir> contains\n"
                 "  subdirectories, each subdirectory is merged independently.\n");
    return 1;
}

bool merge_directory(const fs::path& dir) {
    using namespace picmerge;

    const std::string dir_str = dir.string();
    const std::string out_path = (dir / make_output_name()).string();

    std::vector<std::string> paths;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) {
            std::fprintf(stderr, "[error] directory iteration failed in %s: %s\n",
                         dir_str.c_str(), ec.message().c_str());
            return false;
        }
        if (!entry.is_regular_file()) continue;
        if (!has_image_extension(entry.path())) continue;
        if (is_merge_output(entry.path())) continue;
        paths.push_back(entry.path().string());
    }
    if (paths.empty()) return true;

    std::sort(paths.begin(), paths.end(),
              [](const std::string& a, const std::string& b) {
                  return natural_less(fs::path(a).filename().string(),
                                      fs::path(b).filename().string());
              });

    std::fprintf(stdout, "\n[dir] %s - %zu image(s)\n", dir_str.c_str(), paths.size());
    for (const auto& p : paths) {
        std::fprintf(stdout, "         %s\n", p.c_str());
    }

    int ref_w = 0;
    int ref_h = 0;
    if (!probe_image(paths[0], ref_w, ref_h) || ref_w <= 0 || ref_h <= 0) {
        std::fprintf(stderr, "[error] cannot read image metadata: %s\n",
                     paths[0].c_str());
        return false;
    }
    for (size_t i = 1; i < paths.size(); ++i) {
        int w = 0;
        int h = 0;
        if (!probe_image(paths[i], w, h)) {
            std::fprintf(stderr, "[error] cannot read image metadata: %s\n",
                         paths[i].c_str());
            return false;
        }
        if (w != ref_w || h != ref_h) {
            std::fprintf(stderr,
                         "[error] dimension mismatch: %s is %dx%d, expected %dx%d\n",
                         paths[i].c_str(), w, h, ref_w, ref_h);
            return false;
        }
    }
    std::fprintf(stdout, "[info] all images are %dx%d\n", ref_w, ref_h);

    if (paths.size() == 1) {
        Image img;
        if (!img.load(paths[0])) {
            std::fprintf(stderr, "[error] failed to decode %s\n", paths[0].c_str());
            return false;
        }
        if (!write_jpeg(out_path, img.width(), img.height(), img.data(), kJpegQuality)) {
            return false;
        }
        std::fprintf(stdout, "[info] wrote %s (%dx%d, single image)\n",
                     out_path.c_str(), img.width(), img.height());
        return true;
    }

    TempDirGuard cache_guard;
    cache_guard.dir = make_cache_dir();
    if (cache_guard.dir.empty()) {
        std::fprintf(stderr, "[error] failed to create raw cache directory\n");
        return false;
    }

    std::vector<RowSignatures> sigs(paths.size());
    std::vector<std::string> raw_cache_paths(paths.size());
    {
        ScopedProfile profile("decode + signature + raw cache");
        for (size_t i = 0; i < paths.size(); ++i) {
            Image img;
            if (!img.load(paths[i])) {
                std::fprintf(stderr, "[error] failed to decode %s\n", paths[i].c_str());
                return false;
            }

            sigs[i] = compute_row_signatures(img);

            const fs::path raw_path = cache_guard.dir / (std::to_string(i) + ".rgb");
            if (!write_raw_cache(img, raw_path)) {
                std::fprintf(stderr, "[error] failed to write raw cache %s\n",
                             raw_path.string().c_str());
                return false;
            }
            raw_cache_paths[i] = raw_path.string();
        }
    }

    FixedBars bars;
    {
        ScopedProfile profile("detect fixed bars");
        bars = detect_fixed_bars(sigs);
    }
    std::fprintf(stdout, "[info] fixed top bar = %d rows, bottom bar = %d rows",
                 bars.top_height, bars.bottom_height);
    if (bars.bot_ref != 0) {
        std::fprintf(stdout, " (bar ref = img[%d])", bars.bot_ref);
    }
    std::fprintf(stdout, "\n");

    const int usable_end = ref_h - bars.bottom_height;
    const int max_sticky = static_cast<int>(
        (ref_h - bars.top_height - bars.bottom_height) * 0.40);

    std::vector<int> sticky_pair(paths.size(), 0);
    std::vector<int> self_sticky(paths.size(), 0);
    {
        ScopedProfile profile("detect sticky headers");
        for (size_t k = 1; k < paths.size(); ++k) {
            sticky_pair[k] = detect_sticky_header(sigs[k - 1], sigs[k],
                                                  bars.top_height, bars.bottom_height,
                                                  max_sticky);
        }

        for (size_t k = 0; k < paths.size(); ++k) {
            int s = 0;
            if (k >= 1) s = std::max(s, sticky_pair[k]);
            if (k + 1 < paths.size()) s = std::max(s, sticky_pair[k + 1]);
            self_sticky[k] = s;
        }
    }
    for (size_t k = 0; k < paths.size(); ++k) {
        if (self_sticky[k] > 0) {
            std::fprintf(stdout, "[info] img[%zu] self sticky header = %d rows\n",
                         k, self_sticky[k]);
        }
    }

    std::vector<OverlapResult> overlaps(paths.size() - 1);
    {
        ScopedProfile profile("detect overlaps");
        for (size_t k = 0; k + 1 < paths.size(); ++k) {
            const int prev_sticky = self_sticky[k];
            const int next_sticky = self_sticky[k + 1];
            const int shared = std::max(prev_sticky, next_sticky);
            const int next_template_start = bars.top_height + shared;
            const int prev_search_begin = bars.top_height + prev_sticky;

            overlaps[k] = find_overlap(sigs[k], sigs[k + 1],
                                       prev_search_begin, usable_end,
                                       next_template_start, usable_end);

            if (overlaps[k].ok) {
                refine_overlap_seam(sigs[k], sigs[k + 1], overlaps[k], usable_end);
                const int overlap_h = usable_end - overlaps[k].offset_in_prev;
                const int seam_trim = usable_end - overlaps[k].seam_in_prev;
                std::fprintf(stdout,
                             "[info] pair %zu->%zu: overlap=%d rows, "
                             "offset_in_prev=%d, cost=%.0f (runner-up=%.0f)",
                             k, k + 1, overlap_h, overlaps[k].offset_in_prev,
                             overlaps[k].best_cost, overlaps[k].second_best_cost);
                if (seam_trim > 0) {
                    std::fprintf(stdout, ", seam_trim=%d", seam_trim);
                }
                std::fprintf(stdout, "\n");
            } else {
                std::fprintf(stderr,
                             "[warn] overlap detection failed between img[%zu] and img[%zu]; "
                             "falling back to direct concat (best cost=%.0f, runner-up=%.0f)\n",
                             k, k + 1, overlaps[k].best_cost, overlaps[k].second_best_cost);
            }
        }
    }

    constexpr int kChromeLenientL1 = 300;
    std::vector<int> chrome_pair(paths.size(), 0);
    for (size_t k = 1; k < paths.size(); ++k) {
        int s = 0;
        while (s < max_sticky) {
            const int y = bars.top_height + s;
            if (row_l1(sigs[k - 1].row(y), sigs[k].row(y)) > kChromeLenientL1) {
                break;
            }
            ++s;
        }
        chrome_pair[k] = s;
    }

    std::vector<int> fallback_skip(paths.size(), 0);
    for (size_t k = 0; k < paths.size(); ++k) {
        int c = self_sticky[k];
        if (k >= 1) c = std::max(c, chrome_pair[k]);
        if (k + 1 < paths.size()) c = std::max(c, chrome_pair[k + 1]);
        fallback_skip[k] = c;
    }

    sigs.clear();
    sigs.shrink_to_fit();

    StitchPlan plan;
    {
        ScopedProfile profile("plan stitch");
        const int bar_ref = (bars.bottom_height > 0) ? bars.bot_ref : bars.top_ref;
        plan = plan_stitch(ref_w, ref_h,
                           static_cast<int>(paths.size()),
                           bars.top_height, bars.bottom_height,
                           bar_ref,
                           self_sticky, fallback_skip, overlaps);
    }
    std::fprintf(stdout, "[info] output dimensions: %dx%d, %zu span(s)\n",
                 plan.width, plan.height, plan.parts.size());

    if (!execute_stitch_from_raw_cache(plan, raw_cache_paths, out_path, kJpegQuality)) {
        return false;
    }

    std::fprintf(stdout, "[info] wrote %s (%dx%d)\n",
                 out_path.c_str(), plan.width, plan.height);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return usage();

    const fs::path root = argv[1];
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        std::fprintf(stderr, "[error] not a directory: %s\n", argv[1]);
        return 2;
    }

    std::vector<fs::path> subdirs;
    bool root_has_images = false;

    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) {
            std::fprintf(stderr, "[error] directory iteration failed: %s\n",
                         ec.message().c_str());
            return 2;
        }
        if (entry.is_directory()) {
            subdirs.push_back(entry.path());
        } else if (entry.is_regular_file() && has_image_extension(entry.path())) {
            if (!is_merge_output(entry.path())) {
                root_has_images = true;
            }
        }
    }

    std::sort(subdirs.begin(), subdirs.end(),
              [](const fs::path& a, const fs::path& b) {
                  return natural_less(a.filename().string(), b.filename().string());
              });

    std::vector<fs::path> targets;
    if (root_has_images) targets.push_back(root);
    for (const auto& d : subdirs) targets.push_back(d);

    if (targets.empty()) {
        std::fprintf(stderr,
                     "[error] no image files found in %s or its subdirectories\n",
                     argv[1]);
        return 2;
    }

    int exit_code = 0;
    for (const auto& dir : targets) {
        if (!merge_directory(dir)) {
            exit_code = 2;
        }
    }
    return exit_code;
}
