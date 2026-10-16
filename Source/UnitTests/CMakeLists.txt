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
  COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/NetPlay/mock_index.py
          $<TARGET_FILE:netplay_index_tests>)
set_tests_properties(netplay_index_lifecycle PROPERTIES TIMEOUT 100)

if(ENABLE_QT)
  find_package(Qt6 REQUIRED COMPONENTS Core)
  function(configure_quickplay_test target)
    if(MSVC)
      # Match DolphinQt: Qt 6 requires RTTI even when core disables it.
      remove_cxx_flag_from_target(${target} "/GR-")
      target_compile_options(${target} PRIVATE "/GR")
    endif()
  endfunction()
  add_executable(quickplay_lobby_tests EXCLUDE_FROM_ALL
    NetPlay/QuickPlay.cpp StubHost.cpp
    ../Core/DolphinQt/QuickPlay/QuickPlayMatchmaker.cpp
    ../Core/DolphinQt/QuickPlay/QuickPlayIndexMatchmaker.cpp)
  target_link_libraries(quickplay_lobby_tests PRIVATE core uicommon Qt6::Core)
  configure_quickplay_test(quickplay_lobby_tests)
  add_dependencies(unittests quickplay_lobby_tests)
  add_test(NAME quickplay_listing_recovery
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/NetPlay/recovery.py
            $<TARGET_FILE:quickplay_lobby_tests>)
  set_tests_properties(quickplay_listing_recovery PROPERTIES TIMEOUT 150)
endif()

add_executable(netplay_version_tests EXCLUDE_FROM_ALL NetPlay/VersionHandshake.cpp StubHost.cpp)
target_link_libraries(netplay_version_tests PRIVATE core uicommon)
add_dependencies(unittests netplay_version_tests)
add_test(NAME netplay_version_handshake COMMAND netplay_version_tests)
set_tests_properties(netplay_version_handshake PROPERTIES TIMEOUT 20)

if(ENABLE_QT)
  # Each executable compiles production version accessors and the production matchmaker
  # against a distinct generated identity. No dependency on the developer's Git hashes.
  function(add_quickplay_identity name revision other_platform)
    set(DOLPHIN_WC_REVISION "${revision}")
    set(DOLPHIN_WC_DESCRIBE "collision-7")
    set(DOLPHIN_WC_BRANCH "fixture")
    set(DOLPHIN_WC_COMMITS_AHEAD_MASTER 7)
    set(identity_dir "${CMAKE_CURRENT_BINARY_DIR}/identity_${name}")
    configure_file(../Core/Common/scmrev.h.in "${identity_dir}/Common/scmrev.h")
    add_executable(quickplay_identity_${name} EXCLUDE_FROM_ALL
      NetPlay/Identity.cpp NetPlay/VersionFixture.cpp StubHost.cpp
      ../Core/DolphinQt/QuickPlay/QuickPlayMatchmaker.cpp
      ../Core/DolphinQt/QuickPlay/QuickPlayIndexMatchmaker.cpp)
    target_include_directories(quickplay_identity_${name} BEFORE PRIVATE "${identity_dir}")
    target_link_libraries(quickplay_identity_${name} PRIVATE core uicommon Qt6::Core)
    configure_quickplay_test(quickplay_identity_${name})
    if(other_platform)
      target_compile_definitions(quickplay_identity_${name} PRIVATE NETPLAY_TEST_OTHER_PLATFORM)
    endif()
    add_dependencies(unittests quickplay_identity_${name})
  endfunction()
  add_quickplay_identity(a aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa FALSE)
  add_quickplay_identity(b bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb FALSE)
  add_quickplay_identity(other_platform aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa TRUE)
  add_test(NAME quickplay_revision_matching
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/NetPlay/identity.py
            $<TARGET_FILE:quickplay_identity_a> $<TARGET_FILE:quickplay_identity_b>
            $<TARGET_FILE:quickplay_identity_other_platform>)
  set_tests_properties(quickplay_revision_matching PROPERTIES TIMEOUT 60)
endif()

if(ENABLE_QT)
  find_package(Qt6 REQUIRED COMPONENTS Widgets)
  add_executable(quickplay_settings_tests EXCLUDE_FROM_ALL
    NetPlay/Settings.cpp StubHost.cpp
    ../Core/DolphinQt/QuickPlay/QuickPlayMatchmaker.cpp
    ../Core/DolphinQt/QuickPlay/QuickPlayClient.cpp
    ../Core/DolphinQt/QuickPlay/QuickPlaySettingsDialog.cpp
    ../Core/DolphinQt/QuickPlay/QuickPlaySettingsDialog.h)
  set_target_properties(quickplay_settings_tests PROPERTIES AUTOMOC ON)
  configure_quickplay_test(quickplay_settings_tests)
  target_link_libraries(quickplay_settings_tests PRIVATE core uicommon Qt6::Widgets)
  add_dependencies(unittests quickplay_settings_tests)
  add_test(NAME quickplay_coordinator_settings COMMAND quickplay_settings_tests)
  set_tests_properties(quickplay_coordinator_settings PROPERTIES
    TIMEOUT 20 ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=;QT_STYLE_OVERRIDE=Fusion")
endif()
