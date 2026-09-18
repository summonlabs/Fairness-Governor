# Fairness Governor test suite.
# No test in this suite uses a timeout: a hanging test is a defect to diagnose.

add_library(fg_test_support STATIC support/test_harness.cpp)
target_include_directories(fg_test_support PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(fg_test_support PUBLIC FairnessGovernor::fairness_governor)
fg_apply_strict_warnings(fg_test_support)

function(fg_add_test name)
  cmake_parse_arguments(FG "" "" "SOURCES" ${ARGN})
  add_executable(${name} ${FG_SOURCES})
  target_link_libraries(${name} PRIVATE fg_test_support FairnessGovernor::fairness_governor)
  fg_apply_strict_warnings(${name})
  add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  # No TIMEOUT property is set anywhere: a hanging test is a defect to diagnose.
  set_tests_properties(${name} PROPERTIES
    ENVIRONMENT
      "FG_TEST_TMP_DIR=${CMAKE_CURRENT_BINARY_DIR}/fg-test-tmp;FG_TOOL_DIR=${FG_TOOL_DIR};FG_EXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX};FG_SCENARIO_DIR=${CMAKE_CURRENT_SOURCE_DIR}/scenarios")
endfunction()

add_subdirectory(unit)
add_subdirectory(property)
add_subdirectory(adversarial)
add_subdirectory(concurrency)
add_subdirectory(persistence)
add_subdirectory(multiprocess)
