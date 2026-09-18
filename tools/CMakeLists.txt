add_executable(fg_governor_cli fg_governor_cli.cpp)
target_link_libraries(fg_governor_cli PRIVATE FairnessGovernor::fairness_governor)
fg_apply_strict_warnings(fg_governor_cli)

add_executable(fg_evidence_link fg_evidence_link.cpp)
target_link_libraries(fg_evidence_link PRIVATE FairnessGovernor::fairness_governor)
fg_apply_strict_warnings(fg_evidence_link)

add_executable(fg_evidence_publisher fg_evidence_publisher.cpp)
target_link_libraries(fg_evidence_publisher PRIVATE FairnessGovernor::fairness_governor)
fg_apply_strict_warnings(fg_evidence_publisher)

set(FG_TOOL_DIR "${CMAKE_CURRENT_BINARY_DIR}" CACHE INTERNAL "Directory holding fg tools")

install(TARGETS fg_governor_cli fg_evidence_link fg_evidence_publisher
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
