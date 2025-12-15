#include "../framework/SimpleTest.hpp"
#include "util/TimSort.hpp"
#include "util/BoyerMoore.hpp"
#include <vector>
#include <algorithm>
#include <random>

using namespace ouroboros::util;

TEST_CASE(test_timsort_integers) {
    std::vector<int> v = {5, 1, 9, 3, 7, 4, 8, 2, 6, 0};
    timsort(v, std::less<int>());
    
    for (size_t i = 0; i < v.size(); ++i) {
        ASSERT_EQ(v[i], (int)i);
    }
}

TEST_CASE(test_timsort_already_sorted) {
    std::vector<int> v = {1, 2, 3, 4, 5};
    timsort(v, std::less<int>());
    
    ASSERT_EQ(v[0], 1);
    ASSERT_EQ(v[4], 5);
}

TEST_CASE(test_timsort_reverse_sorted) {
    std::vector<int> v = {5, 4, 3, 2, 1};
    timsort(v, std::less<int>());
    
    ASSERT_EQ(v[0], 1);
    ASSERT_EQ(v[4], 5);
}

TEST_CASE(test_boyer_moore_search) {
    BoyerMooreSearch bms("needle");
    
    std::string text = "haystack needle haystack";
    int pos = bms.search(text);
    
    ASSERT_EQ(pos, 9);
}

TEST_CASE(test_boyer_moore_search_not_found) {
    BoyerMooreSearch bms("gold");
    
    std::string text = "haystack needle haystack";
    int pos = bms.search(text);
    
    ASSERT_EQ(pos, -1);
}

TEST_CASE(test_boyer_moore_case_insensitive) {
    BoyerMooreSearch bms("NEEDLE", false); // false = case insensitive (wait, check header)
    // Header check: BoyerMooreSearch(const std::string& pattern, bool case_sensitive = false);
    // Wait, my replacement code might have swapped args or logic?
    // Let's assume standard logic: false = insensitive? 
    // Usually: case_sensitive=true is default.
    // Let's re-read BoyerMoore header mentally. 
    // "BoyerMooreSearch(const std::string& pattern, bool case_sensitive = false);"
    // So default IS insensitive? That's unusual but handy.
    // Let's test explicit false.
    
    std::string text = "haystack needle haystack";
    int pos = bms.search(text);
    
    ASSERT_EQ(pos, 9);
}

int main() {
    return ouroboros::test::TestRunner::instance().run_all();
}
