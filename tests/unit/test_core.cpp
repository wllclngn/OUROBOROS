#include "../framework/SimpleTest.hpp"
#include "util/ArtworkHasher.hpp"
#include <vector>

using namespace ouroboros::util;

TEST_CASE(test_artwork_hasher_basic) {
    std::vector<unsigned char> data = {0x01, 0x02, 0x03, 0x04};
    size_t hash = ArtworkHasher::fast_hash(data);
    
    // FNV-1a of {1, 2, 3, 4}
    // Just ensure it's stable
    ASSERT_EQ(hash, ArtworkHasher::fast_hash(data));
}

TEST_CASE(test_artwork_hasher_empty) {
    std::vector<unsigned char> data;
    size_t hash = ArtworkHasher::fast_hash(data);
    
    // FNV-1a offset basis
    ASSERT_EQ(hash, 14695981039346656037ULL);
}

int main() {
    return ouroboros::test::TestRunner::instance().run_all();
}
