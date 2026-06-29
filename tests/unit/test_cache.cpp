#include "SimpleTest.hpp"
#include "backend/Library.hpp"

#include <string>
#include <unordered_map>
#include <vector>

using ouroboros::backend::Library;
using Result = ouroboros::backend::Library::CacheValidationResult;
using ouroboros::model::Track;

namespace {

Track track_at(const std::string& path, std::time_t mtime) {
    Track t;
    t.path = path;
    t.file_mtime = mtime;
    return t;
}

// A small library of two files in one scanned directory.
const std::string DIR = "/music/album";
const std::string A = DIR + "/01.flac";
const std::string B = DIR + "/02.flac";

std::unordered_map<std::string, Track> two_track_cache() {
    std::unordered_map<std::string, Track> c;
    c[A] = track_at(A, 1000);
    c[B] = track_at(B, 2000);
    return c;
}

}  // namespace

TEST_CASE(test_cache_unchanged_is_valid) {
    auto cache = two_track_cache();
    std::vector<std::string> scanned{A, B};
    std::unordered_map<std::string, std::time_t> mtimes{{A, 1000}, {B, 2000}};
    std::unordered_map<std::string, std::time_t> dirs{{DIR, 500}};

    ASSERT_TRUE(Library::classify_tier0(scanned, mtimes, dirs, cache) == Result::Valid);
}

TEST_CASE(test_cache_added_file_is_count_mismatch) {
    auto cache = two_track_cache();
    std::string C = DIR + "/03.flac";
    std::vector<std::string> scanned{A, B, C};  // C not in cache
    std::unordered_map<std::string, std::time_t> mtimes{{A, 1000}, {B, 2000}, {C, 3000}};
    std::unordered_map<std::string, std::time_t> dirs{{DIR, 500}};

    ASSERT_TRUE(Library::classify_tier0(scanned, mtimes, dirs, cache) == Result::CountMismatch);
}

TEST_CASE(test_cache_edited_file_is_metadata_mismatch) {
    auto cache = two_track_cache();
    std::vector<std::string> scanned{A, B};
    // B retagged: on-disk mtime newer than cached 2000.
    std::unordered_map<std::string, std::time_t> mtimes{{A, 1000}, {B, 2500}};
    std::unordered_map<std::string, std::time_t> dirs{{DIR, 500}};

    ASSERT_TRUE(Library::classify_tier0(scanned, mtimes, dirs, cache) == Result::MetadataMismatch);
}

TEST_CASE(test_cache_equal_mtime_is_valid) {
    // Boundary: equal mtime is not "newer" -- must stay Valid.
    auto cache = two_track_cache();
    std::vector<std::string> scanned{A, B};
    std::unordered_map<std::string, std::time_t> mtimes{{A, 1000}, {B, 2000}};
    std::unordered_map<std::string, std::time_t> dirs{{DIR, 500}};

    ASSERT_TRUE(Library::classify_tier0(scanned, mtimes, dirs, cache) == Result::Valid);
}

TEST_CASE(test_cache_removed_in_scanned_dir_is_metadata_mismatch) {
    auto cache = two_track_cache();
    // B deleted: its directory WAS scanned, but B is absent from the scan.
    std::vector<std::string> scanned{A};
    std::unordered_map<std::string, std::time_t> mtimes{{A, 1000}};
    std::unordered_map<std::string, std::time_t> dirs{{DIR, 500}};

    ASSERT_TRUE(Library::classify_tier0(scanned, mtimes, dirs, cache) == Result::MetadataMismatch);
}

TEST_CASE(test_cache_unmounted_dir_is_valid) {
    // B's directory was NOT scanned this pass (unmounted drive). The cumulative
    // cache must keep B -- this is NOT a removal.
    auto cache = two_track_cache();
    std::string ext = "/mnt/external/03.flac";
    cache[ext] = track_at(ext, 3000);  // lives on an unmounted drive
    std::vector<std::string> scanned{A, B};  // only /music/album scanned
    std::unordered_map<std::string, std::time_t> mtimes{{A, 1000}, {B, 2000}};
    std::unordered_map<std::string, std::time_t> dirs{{DIR, 500}};  // /mnt/external absent

    ASSERT_TRUE(Library::classify_tier0(scanned, mtimes, dirs, cache) == Result::Valid);
}

int main() {
    return ouroboros::test::TestRunner::instance().run_all();
}
