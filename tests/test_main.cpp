#include <filesystem>

#include <gtest/gtest.h>

class GlobalTestEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        // Create logs directory for observability tests
        std::filesystem::create_directories("logs");
    }

    void TearDown() override {}
};

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new GlobalTestEnvironment);
    return RUN_ALL_TESTS();
}
