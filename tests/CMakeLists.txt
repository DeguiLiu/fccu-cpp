# tests/CMakeLists.txt

add_executable(fccu_tests test_fccu.cpp)
target_link_libraries(fccu_tests PRIVATE fccu Catch2::Catch2WithMain)

include(CTest)
include(Catch)
catch_discover_tests(fccu_tests)
