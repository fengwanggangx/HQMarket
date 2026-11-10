cmake_minimum_required(VERSION 3.20)
project(HQMarketOfflineTests LANGUAGES CXX)

# The production application is built from HQMarket.sln / HQMarket.vcxproj.
# This entry point only delegates to the maintained offline routing tests.
enable_testing()
add_subdirectory(HQMarket/system/tests)
