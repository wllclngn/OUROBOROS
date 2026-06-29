#include "SimpleTest.hpp"
#include "model/Snapshot.hpp"
#include "util/SortKeys.hpp"
#include "util/SortDispatch.hpp"
#include "util/UnicodeUtils.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using Track = ouroboros::model::Track;
namespace u = ouroboros::util;

namespace {

// Identity artist key (the prefix-strip is config-driven and orthogonal to the
// fold/byte-order question this test validates).
std::string akey(const std::string& a) { return a; }

// Reference comparator: exactly the production track comparator (group_by_dir=false).
bool ref_less(const Track& a, const Track& b) {
    int c = u::case_insensitive_compare(akey(a.artist), akey(b.artist));
    if (c != 0) return c < 0;
    if (a.date != b.date) return a.date < b.date;
    return a.track_number < b.track_number;
}

// The sublimation path: composite key -> byte-order string sort -> index order.
std::vector<uint32_t> sublimation_order(const std::vector<Track>& ts) {
    const size_t n = ts.size();
    std::vector<std::string> keys(n);
    std::vector<const char*> ptrs(n);
    std::vector<uint32_t> idx(n);
    for (size_t i = 0; i < n; ++i) {
        keys[i] = u::track_sort_key(u::fold_case(akey(ts[i].artist)),
                                    ts[i].date, /*dir=*/"", ts[i].track_number,
                                    /*group_by_dir=*/false);
        ptrs[i] = keys[i].c_str();
        idx[i] = static_cast<uint32_t>(i);
    }
    u::sort_by_string(ptrs, idx);
    return idx;
}

Track mk(const std::string& artist, const std::string& date, int track) {
    Track t;
    t.artist = artist;
    t.date = date;
    t.track_number = track;
    return t;
}

}  // namespace

// Parity: the composite-key order must equal the ICU comparator order. Distinct
// keys per element so stable-vs-unstable never enters into it.
TEST_CASE(test_sort_parity_with_icu_comparator) {
    std::vector<Track> tracks = {
        mk("The Beatles", "1969", 1), mk("beatles", "1966", 2),
        mk("Bjork", "2001", 1), mk("Bjork", "1997", 3),
        mk("ABBA", "1976", 1), mk("abba", "1974", 2),
        mk("2Pac", "1996", 1), mk("Zapp", "1980", 1),
        mk("DJ Shadow", "1996", 4), mk("dj shadow", "2002", 2),
        mk("[bootleg] X", "2010", 1), mk("Aphex Twin", "1992", 7),
        mk("aphex twin", "1994", 1), mk("Boards of Canada", "1998", 5),
        mk("Autechre", "1995", 2), mk("autechre", "2001", 9),
        mk("M83", "2011", 3), mk("Mum", "2002", 1),
    };

    std::vector<uint32_t> ref(tracks.size());
    for (uint32_t i = 0; i < ref.size(); ++i) ref[i] = i;
    std::stable_sort(ref.begin(), ref.end(),
                     [&](uint32_t a, uint32_t b) { return ref_less(tracks[a], tracks[b]); });

    std::vector<uint32_t> got = sublimation_order(tracks);

    ASSERT_EQ(got.size(), ref.size());
    for (size_t p = 0; p < ref.size(); ++p) {
        // Compare by value (artist/date/track), not raw index, to be robust.
        ASSERT_TRUE(tracks[got[p]].artist == tracks[ref[p]].artist);
        ASSERT_TRUE(tracks[got[p]].date == tracks[ref[p]].date);
        ASSERT_EQ(tracks[got[p]].track_number, tracks[ref[p]].track_number);
    }
}

// Scale check at a realistic library size: the composite-key sublimation sort
// produces a fully-ordered result, timed for reference. (The head-to-head against
// the retired parallel TimSort measured ~2x in sublimation's favor at this N --
// single-threaded sublimation beating the multi-threaded TimSort, whose
// thread-spawn overhead dominated; see COMMIT_MESSAGE / ROADMAP.)
TEST_CASE(test_sort_scale_46k_ordered) {
    constexpr size_t N = 46000;
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> artist_pick(0, 4000);
    std::uniform_int_distribution<int> year_pick(1960, 2026);
    std::uniform_int_distribution<int> track_pick(1, 24);

    std::vector<Track> tracks;
    tracks.reserve(N);
    for (size_t i = 0; i < N; ++i) {
        tracks.push_back(mk("Artist " + std::to_string(artist_pick(rng)),
                            std::to_string(year_pick(rng)), track_pick(rng)));
    }

    auto t0 = std::chrono::steady_clock::now();
    auto idx = sublimation_order(tracks);
    auto t1 = std::chrono::steady_clock::now();
    std::printf("[BENCH] sublimation track sort n=%zu  %.2fms\n", N,
                std::chrono::duration<double, std::milli>(t1 - t0).count());

    ASSERT_EQ(idx.size(), N);
    // Fully ordered by the reference comparator (no inversions).
    for (size_t p = 1; p < idx.size(); ++p) {
        ASSERT_FALSE(ref_less(tracks[idx[p]], tracks[idx[p - 1]]));
    }
}

int main() {
    return ouroboros::test::TestRunner::instance().run_all();
}
