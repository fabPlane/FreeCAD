# SPDX-License-Identifier: LGPL-2.1-or-later

add_executable(Api_tests_run
    Codec.cpp
    Server.cpp
)

target_include_directories(Api_tests_run PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/src)

target_link_libraries(Api_tests_run PRIVATE
    GTest::gmock_main
    ${Python3_LIBRARIES}
    FreeCADApiCore
    FreeCADApp
)

add_test(NAME Api_tests_run COMMAND Api_tests_run)
