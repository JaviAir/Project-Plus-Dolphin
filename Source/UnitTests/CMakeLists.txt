enable_testing()
add_custom_target(unittests)
add_custom_command(TARGET unittests POST_BUILD COMMAND ${CMAKE_CTEST_COMMAND} "--output-on-failure")

string(APPEND CMAKE_RUNTIME_OUTPUT_DIRECTORY "/Tests")

add_executable(tests EXCLUDE_FROM_ALL UnitTestsMain.cpp StubHost.cpp)
set_target_properties(tests PROPERTIES FOLDER Tests)
target_link_libraries(tests PRIVATE fmt::fmt gtest::gtest core uicommon)
add_test(NAME tests COMMAND tests)
add_custom_command(TARGET tests POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E remove_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Sys"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/Data/Sys" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Sys"
)
add_dependencies(unittests tests)

macro(add_dolphin_test target)
  add_library(${target} OBJECT ${ARGN})
  target_link_libraries(${target} PUBLIC fmt::fmt gtest::gtest PRIVATE core uicommon)
  target_link_libraries(tests PRIVATE ${target})
endmacro()

add_subdirectory(Common)
add_subdirectory(Core)
add_subdirectory(VideoCommon)

# These fixtures build production sources and run only on ephemeral loopback ports.
# Keep the index/server tests usable with ENABLE_QT=OFF.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_executable(netplay_index_tests EXCLUDE_FROM_ALL NetPlay/Index.cpp StubHost.cpp)
target_link_libraries(netplay_index_tests PRIVATE core uicommon)
add_dependencies(unittests netplay_index_tests)
add_test(NAME netplay_index_lifecycle
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/NetPlay/mock_index.py
          $<TARGET_FILE:netplay_index_tests>)
set_tests_properties(netplay_index_lifecycle PROPERTIES TIMEOUT 100)
