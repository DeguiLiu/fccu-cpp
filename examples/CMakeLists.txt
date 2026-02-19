# examples/CMakeLists.txt

find_package(Threads REQUIRED)

# basic_demo: standalone (no mccc/ztask)
add_executable(fccu_basic_demo basic_demo.cpp)
target_link_libraries(fccu_basic_demo PRIVATE fccu Threads::Threads)

# bus_demo: mccc integration
add_executable(fccu_bus_demo bus_demo.cpp)
target_link_libraries(fccu_bus_demo PRIVATE fccu mccc Threads::Threads)

# ztask_demo: ztask integration
add_executable(fccu_ztask_demo ztask_demo.cpp)
target_link_libraries(fccu_ztask_demo PRIVATE fccu ztask Threads::Threads)
