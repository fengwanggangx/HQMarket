cmake_minimum_required(VERSION 3.20)
project(HQMarketCoreTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()
add_executable(hqmarket_core_tests
    HQMarket/tests/CoreTests.cpp
    HQMarket/market/MarketTypes.cpp
    HQMarket/market/CMarketCache.cpp
    HQMarket/market/CSubscriptionManager.cpp
    HQMarket/network/CFrameCodec.cpp)
target_include_directories(hqmarket_core_tests PRIVATE HQMarket)
add_test(NAME hqmarket_core_tests COMMAND hqmarket_core_tests)
