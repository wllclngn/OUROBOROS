#include "SimpleTest.hpp"
#include "backend/GenreDecode.hpp"

#include <string>

using ouroboros::backend::decode_id3_genre;

TEST_CASE(test_genre_parenthesized_numeric) {
    ASSERT_TRUE(decode_id3_genre("(52)") == "Electronic");
    ASSERT_TRUE(decode_id3_genre("(0)") == "Blues");
    ASSERT_TRUE(decode_id3_genre("(17)") == "Rock");
}

TEST_CASE(test_genre_bare_numeric) {
    ASSERT_TRUE(decode_id3_genre("52") == "Electronic");
    ASSERT_TRUE(decode_id3_genre("13") == "Pop");
}

TEST_CASE(test_genre_refinement_string_wins) {
    // ID3v2.3 "(N)Refinement" -> the refinement text, not the table entry.
    ASSERT_TRUE(decode_id3_genre("(52)Ambient Dub") == "Ambient Dub");
    ASSERT_TRUE(decode_id3_genre("(4)Italo Disco") == "Italo Disco");
}

TEST_CASE(test_genre_plain_string_passthrough) {
    ASSERT_TRUE(decode_id3_genre("Electronic") == "Electronic");
    ASSERT_TRUE(decode_id3_genre("Drum & Bass") == "Drum & Bass");
    ASSERT_TRUE(decode_id3_genre("") == "");
}

TEST_CASE(test_genre_out_of_range_passthrough) {
    // No table entry -> leave the raw token unchanged rather than invent one.
    ASSERT_TRUE(decode_id3_genre("(999)") == "(999)");
    ASSERT_TRUE(decode_id3_genre("999") == "999");
}

TEST_CASE(test_genre_malformed_passthrough) {
    ASSERT_TRUE(decode_id3_genre("(52") == "(52");        // no closing paren
    ASSERT_TRUE(decode_id3_genre("(abc)") == "(abc)");    // non-numeric in parens
    ASSERT_TRUE(decode_id3_genre("(52a)") == "(52a)");    // non-numeric tail
    ASSERT_TRUE(decode_id3_genre("12x") == "12x");        // bare non-numeric
}

int main() {
    return ouroboros::test::TestRunner::instance().run_all();
}
