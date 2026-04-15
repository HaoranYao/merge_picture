#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
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

struct TestFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void expect(bool condition, const std::string& message) {
    if (!condition) throw TestFailure(message);
}

picmerge::RowSignatures make_signatures(const std::vector<int>& values) {
    picmerge::RowSignatures sigs;
    sigs.height = static_cast<int>(values.size());
    sigs.fp.resize(values.size() * picmerge::kSigBins);
    for (size_t y = 0; y < values.size(); ++y) {
        for (int k = 0; k < picmerge::kSigBins; ++k) {
            sigs.fp[y * picmerge::kSigBins + static_cast<size_t>(k)] =
                static_cast<uint8_t>(values[y]);
        }
    }
    return sigs;
}

std::vector<std::string> collect_images(const fs::path& dir) {
    std::vector<std::string> paths;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        const std::string filename = entry.path().filename().string();
        if (filename.rfind("merge_", 0) == 0) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".jpg" || ext == ".jpeg" || ext == ".png") {
            paths.push_back(entry.path().string());
        }
    }
    std::sort(paths.begin(), paths.end(),
              [](const std::string& a, const std::string& b) {
                  const std::string sa = fs::path(a).filename().stem().string();
                  const std::string sb = fs::path(b).filename().stem().string();
                  const bool na = !sa.empty() &&
                      std::all_of(sa.begin(), sa.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
                  const bool nb = !sb.empty() &&
                      std::all_of(sb.begin(), sb.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
                  if (na && nb) return std::stoi(sa) < std::stoi(sb);
                  return sa < sb;
              });
    return paths;
}

std::vector<std::string> collect_demo_dirs(const fs::path& demo_root) {
    std::vector<std::string> dirs;
    for (const auto& entry : fs::directory_iterator(demo_root)) {
        if (entry.is_directory()) dirs.push_back(entry.path().string());
    }
    std::sort(dirs.begin(), dirs.end());
    return dirs;
}

struct DatasetMetrics {
    std::string name;
    int num_images = 0;
    int width = 0;
    int height = 0;
    int top_bar = 0;
    int bottom_bar = 0;
    int sticky_images = 0;
    int overlap_pairs = 0;
    int overlap_ok = 0;
    int seam_trimmed_pairs = 0;
    int output_height = 0;
    double sample_retention = 0.0;
    int max_duplicate_rows = 0;
};

std::vector<picmerge::RowSignatures> load_signatures(const std::vector<std::string>& paths,
                                                     int& width,
                                                     int& height) {
    std::vector<picmerge::RowSignatures> sigs;
    for (size_t i = 0; i < paths.size(); ++i) {
        picmerge::Image img;
        expect(img.load(paths[i]), "failed to load image: " + paths[i]);
        if (i == 0) {
            width = img.width();
            height = img.height();
        } else {
            expect(img.width() == width && img.height() == height,
                   "dimension mismatch in dataset");
        }
        sigs.push_back(picmerge::compute_row_signatures(img));
    }
    return sigs;
}

DatasetMetrics analyze_dataset(const fs::path& dir) {
    DatasetMetrics metrics;
    metrics.name = dir.filename().string();

    const std::vector<std::string> paths = collect_images(dir);
    metrics.num_images = static_cast<int>(paths.size());
    expect(metrics.num_images >= 2, "dataset needs at least 2 images: " + metrics.name);

    int width = 0;
    int height = 0;
    std::vector<picmerge::RowSignatures> sigs = load_signatures(paths, width, height);
    metrics.width = width;
    metrics.height = height;

    const picmerge::FixedBars bars = picmerge::detect_fixed_bars(sigs);
    metrics.top_bar = bars.top_height;
    metrics.bottom_bar = bars.bottom_height;

    const int usable_end = height - bars.bottom_height;
    const int max_sticky =
        static_cast<int>((height - bars.top_height - bars.bottom_height) * 0.40);

    std::vector<int> sticky_pair(paths.size(), 0);
    for (size_t k = 1; k < paths.size(); ++k) {
        sticky_pair[k] = picmerge::detect_sticky_header(
            sigs[k - 1], sigs[k], bars.top_height, bars.bottom_height, max_sticky);
    }

    std::vector<int> self_sticky(paths.size(), 0);
    for (size_t k = 0; k < paths.size(); ++k) {
        int s = 0;
        if (k >= 1) s = std::max(s, sticky_pair[k]);
        if (k + 1 < paths.size()) s = std::max(s, sticky_pair[k + 1]);
        self_sticky[k] = s;
        if (s > 0) ++metrics.sticky_images;
    }

    std::vector<picmerge::OverlapResult> overlaps(paths.size() - 1);
    for (size_t k = 0; k + 1 < paths.size(); ++k) {
        const int prev_sticky = self_sticky[k];
        const int next_sticky = self_sticky[k + 1];
        const int shared = std::max(prev_sticky, next_sticky);
        const int next_template_start = bars.top_height + shared;
        const int prev_search_begin = bars.top_height + prev_sticky;

        overlaps[k] = picmerge::find_overlap(sigs[k], sigs[k + 1],
                                             prev_search_begin, usable_end,
                                             next_template_start, usable_end);
        ++metrics.overlap_pairs;
        if (overlaps[k].ok) {
            picmerge::refine_overlap_seam(sigs[k], sigs[k + 1], overlaps[k], usable_end);
            ++metrics.overlap_ok;
            if (overlaps[k].seam_in_prev < usable_end) ++metrics.seam_trimmed_pairs;
        }
    }

    constexpr int kChromeLenientL1 = 300;
    std::vector<int> chrome_pair(paths.size(), 0);
    for (size_t k = 1; k < paths.size(); ++k) {
        int s = 0;
        while (s < max_sticky) {
            const int y = bars.top_height + s;
            if (picmerge::row_l1(sigs[k - 1].row(y), sigs[k].row(y)) > kChromeLenientL1)
                break;
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

    const int bar_ref = (bars.bottom_height > 0) ? bars.bot_ref : bars.top_ref;
    const picmerge::StitchPlan plan = picmerge::plan_stitch(
        width, height, static_cast<int>(paths.size()),
        bars.top_height, bars.bottom_height, bar_ref,
        self_sticky, fallback_skip, overlaps);

    const fs::path temp_root = fs::temp_directory_path() / "picmerge_tests";
    fs::create_directories(temp_root);
    const fs::path out_path = temp_root / (metrics.name + "_stitched.jpg");
    expect(picmerge::execute_stitch(plan, paths, out_path.string(), 90),
           "failed to execute stitch for " + metrics.name);

    picmerge::Image out_img;
    expect(out_img.load(out_path.string()), "failed to load stitched output");
    expect(out_img.width() == plan.width && out_img.height() == plan.height,
           "stitched output dimensions mismatch");
    metrics.output_height = out_img.height();

    const picmerge::RowSignatures out_sigs = picmerge::compute_row_signatures(out_img);

    int sample_total = 0;
    int sample_matches = 0;
    int out_y = 0;
    for (const auto& part : plan.parts) {
        const int rows = part.y_end - part.y_begin;
        if (rows <= 0) continue;
        const int step = std::max(1, rows / 8);
        for (int local = 0; local < rows; local += step) {
            const int src_y = part.y_begin + local;
            const int dst_y = out_y + local;
            ++sample_total;
            if (picmerge::row_l1(sigs[static_cast<size_t>(part.image_index)].row(src_y),
                                 out_sigs.row(dst_y)) <= 200) {
                ++sample_matches;
            }
        }
        if ((rows - 1) % step != 0) {
            const int src_y = part.y_end - 1;
            const int dst_y = out_y + rows - 1;
            ++sample_total;
            if (picmerge::row_l1(sigs[static_cast<size_t>(part.image_index)].row(src_y),
                                 out_sigs.row(dst_y)) <= 200) {
                ++sample_matches;
            }
        }
        out_y += rows;
    }
    metrics.sample_retention = sample_total > 0
        ? static_cast<double>(sample_matches) / static_cast<double>(sample_total)
        : 0.0;

    int seam_y = 0;
    for (size_t i = 0; i + 1 < plan.parts.size(); ++i) {
        seam_y += plan.parts[i].y_end - plan.parts[i].y_begin;
        const int max_check = std::min({128, seam_y, plan.height - seam_y});
        int longest = 0;
        for (int len = 1; len <= max_check; ++len) {
            bool same = true;
            for (int j = 0; j < len; ++j) {
                if (picmerge::row_l1(out_sigs.row(seam_y - len + j),
                                     out_sigs.row(seam_y + j)) > 80) {
                    same = false;
                    break;
                }
            }
            if (!same) break;
            longest = len;
        }
        metrics.max_duplicate_rows = std::max(metrics.max_duplicate_rows, longest);
    }

    std::error_code ec;
    fs::remove(out_path, ec);
    return metrics;
}

void test_detect_sticky_header() {
    const auto prev = make_signatures({1, 2, 10, 10, 10, 20, 21, 22, 23, 24});
    const auto next = make_signatures({1, 2, 10, 10, 10, 30, 31, 32, 33, 34});
    const int sticky = picmerge::detect_sticky_header(prev, next, 2, 0, 5);
    expect(sticky == 3, "sticky header height should be 3");
}

void test_overlap_finder_with_dynamic_header() {
    std::vector<int> prev_rows;
    std::vector<int> next_rows;
    for (int i = 0; i < 400; ++i) prev_rows.push_back(i % 256);
    next_rows.insert(next_rows.end(), 128, 250);
    for (int i = 160; i < 400; ++i) next_rows.push_back(i % 256);

    const auto prev = make_signatures(prev_rows);
    const auto next = make_signatures(next_rows);

    picmerge::OverlapResult r = picmerge::find_overlap(prev, next, 0, prev.height, 0, next.height);
    expect(r.ok, "overlap should succeed after skipping dynamic header");
    expect(r.template_start_in_next >= 128, "template should move beyond dynamic header");
    expect(r.offset_in_prev == 160, "overlap offset should align to the later clean template");
}

void test_refine_overlap_seam_trims_dirty_tail() {
    std::vector<int> prev_rows;
    std::vector<int> next_rows;
    for (int i = 0; i < 80; ++i) {
        prev_rows.push_back(i);
        next_rows.push_back(i);
    }
    for (int i = 80; i < 90; ++i) prev_rows.push_back(250);
    for (int i = 80; i < 90; ++i) next_rows.push_back(i);

    const auto prev = make_signatures(prev_rows);
    const auto next = make_signatures(next_rows);

    picmerge::OverlapResult r;
    r.ok = true;
    r.offset_in_prev = 0;
    r.template_start_in_next = 0;
    r.template_length = 32;

    picmerge::refine_overlap_seam(prev, next, r, prev.height);
    expect(r.seam_in_prev < prev.height, "dirty tail should move seam upward");
}

void test_plan_stitch_avoids_overlap_duplication() {
    std::vector<int> sticky = {0, 0};
    std::vector<int> fallback = {0, 0};
    std::vector<picmerge::OverlapResult> overlaps(1);
    overlaps[0].ok = true;
    overlaps[0].offset_in_prev = 60;
    overlaps[0].template_start_in_next = 0;
    overlaps[0].template_length = 32;
    overlaps[0].seam_in_prev = 100;

    const picmerge::StitchPlan plan = picmerge::plan_stitch(
        100, 120, 2, 5, 10, 0, sticky, fallback, overlaps);

    expect(plan.height == 180, "plan height should remove overlap and duplicate bottom bar");
    expect(plan.parts.size() == 3, "plan should contain content+content+bottom bar spans");
    expect(plan.parts[0].image_index == 0 && plan.parts[0].y_begin == 0 && plan.parts[0].y_end == 100,
           "first image span mismatch");
    expect(plan.parts[1].image_index == 1 && plan.parts[1].y_begin == 40 && plan.parts[1].y_end == 110,
           "second image span mismatch");
}

void test_demo_datasets() {
    const fs::path demo_root = fs::path(PICMERGE_SOURCE_DIR) / "demo_pic";
    const std::vector<std::string> dirs = collect_demo_dirs(demo_root);
    expect(!dirs.empty(), "demo_pic is empty");

    int total_pairs = 0;
    int total_overlap_ok = 0;
    int total_trimmed = 0;
    int datasets_with_sticky = 0;
    double retention_sum = 0.0;
    double retention_min = 1.0;
    int worst_duplicate_rows = 0;
    int pdd3_bottom_bar = 0;

    for (const auto& dir : dirs) {
        const DatasetMetrics m = analyze_dataset(dir);
        total_pairs += m.overlap_pairs;
        total_overlap_ok += m.overlap_ok;
        total_trimmed += m.seam_trimmed_pairs;
        if (m.sticky_images > 0) ++datasets_with_sticky;
        retention_sum += m.sample_retention;
        retention_min = std::min(retention_min, m.sample_retention);
        worst_duplicate_rows = std::max(worst_duplicate_rows, m.max_duplicate_rows);
        if (m.name == "pdd3") pdd3_bottom_bar = m.bottom_bar;

        std::cout << "[dataset] " << m.name
                  << " images=" << m.num_images
                  << " bars(top=" << m.top_bar << ", bottom=" << m.bottom_bar << ")"
                  << " sticky_images=" << m.sticky_images
                  << " overlap=" << m.overlap_ok << "/" << m.overlap_pairs
                  << " seam_trimmed=" << m.seam_trimmed_pairs
                  << " retention=" << m.sample_retention
                  << " max_duplicate_rows=" << m.max_duplicate_rows
                  << " output_height=" << m.output_height
                  << "\n";

        expect(m.output_height > m.height, "stitched output should exceed one input image for " + m.name);
        expect(m.output_height < m.height * m.num_images,
               "stitched output should be shorter than raw concat for " + m.name);
        expect(m.sample_retention >= 0.95, "retention too low for " + m.name);
        expect(m.max_duplicate_rows <= 64, "duplicate overlap leak too large for " + m.name);
    }

    const double overlap_ratio = total_pairs > 0
        ? static_cast<double>(total_overlap_ok) / static_cast<double>(total_pairs)
        : 0.0;
    const double avg_retention = retention_sum / static_cast<double>(dirs.size());

    std::cout << "[summary] datasets=" << dirs.size()
              << " overlap_ratio=" << overlap_ratio
              << " datasets_with_sticky=" << datasets_with_sticky
              << " seam_trimmed_pairs=" << total_trimmed
              << " avg_retention=" << avg_retention
              << " min_retention=" << retention_min
              << " worst_duplicate_rows=" << worst_duplicate_rows
              << "\n";

    expect(overlap_ratio >= 0.80, "overall overlap success ratio is too low");
    expect(datasets_with_sticky >= 3, "sticky-header coverage is too small");
    expect(total_trimmed >= 1, "dirty seam refinement never triggered");
    expect(avg_retention >= 0.98, "average retention is too low");
    expect(retention_min >= 0.95, "minimum retention is too low");
    expect(worst_duplicate_rows <= 64, "worst duplicate leakage is too large");
    expect(pdd3_bottom_bar >= 240, "pdd3 bottom bar regression: CTA strip was under-detected");
}

} // namespace

int main() {
    try {
        test_detect_sticky_header();
        test_overlap_finder_with_dynamic_header();
        test_refine_overlap_seam_trims_dirty_tail();
        test_plan_stitch_avoids_overlap_duplication();
        test_demo_datasets();
    } catch (const TestFailure& ex) {
        std::cerr << "[FAIL] " << ex.what() << "\n";
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] unexpected exception: " << ex.what() << "\n";
        return 1;
    }

    std::cout << "[PASS] picmerge tests\n";
    return 0;
}
