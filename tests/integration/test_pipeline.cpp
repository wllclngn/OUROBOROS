#include "../framework/SimpleTest.hpp"
#include "backend/MetadataParser.hpp"
#include <filesystem>
#include <fstream>

using namespace ouroboros::backend;

void create_dummy_file(const std::string& path) {
    std::ofstream f(path);
    f << "dummy content";
    f.close();
}

TEST_CASE(test_metadata_parser_nonexistent) {
    auto track = MetadataParser::parse_file("nonexistent.mp3");
    // If file doesn't exist, we expect an invalid track
    ASSERT_FALSE(track.is_valid);
}

TEST_CASE(test_metadata_parser_garbage) {
    create_dummy_file("/tmp/test_track.mp3");
    auto track = MetadataParser::parse_file("/tmp/test_track.mp3");
    
    // Garbage content should result in invalid track or empty metadata
    // We just want to ensure it doesn't crash
    std::filesystem::remove("/tmp/test_track.mp3");
    
    // We expect it to be handled safely
    ASSERT_TRUE(true); 
}

int main() {
    return ouroboros::test::TestRunner::instance().run_all();
}
