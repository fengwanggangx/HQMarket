cmake_minimum_required(VERSION 3.20)
project(HQMarketCoreTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    add_compile_options(/utf-8)
endif()
enable_testing()
add_executable(hqmarket_core_tests
    HQMarket/tests/CoreTests.cpp
    HQMarket/quote/MarketTypes.cpp
    HQMarket/quote/CHQCache.cpp
    HQMarket/quote/CSubscriptionMgr.cpp
    HQMarket/network/CFrameCodec.cpp)
target_include_directories(hqmarket_core_tests PRIVATE HQMarket)
add_test(NAME hqmarket_core_tests COMMAND hqmarket_core_tests)
