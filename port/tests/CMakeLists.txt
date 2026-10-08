add_executable(runtime_tests engine/runtime_tests.cpp engine/test_game.cpp)
target_link_libraries(runtime_tests PRIVATE wp_engine)
target_compile_options(runtime_tests PRIVATE -Wall -Wextra)
add_test(NAME runtime_tests COMMAND runtime_tests)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME ppc_tests COMMAND Python3::Interpreter -m unittest discover -s "${CMAKE_CURRENT_SOURCE_DIR}/recompiler")
set(WP_INSTRUCTION_CASES "${CMAKE_CURRENT_BINARY_DIR}/instruction_cases.cpp")
add_custom_command(OUTPUT "${WP_INSTRUCTION_CASES}"
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/instruction_tests.py" --output "${WP_INSTRUCTION_CASES}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/instruction_tests.py" "${CMAKE_SOURCE_DIR}/recompiler/ppc/emit.py" "${CMAKE_SOURCE_DIR}/recompiler/ppc/decoder.py"
    VERBATIM)
add_executable(translator_tests "${WP_INSTRUCTION_CASES}" engine/test_game.cpp)
set_source_files_properties("${WP_INSTRUCTION_CASES}" PROPERTIES COMPILE_OPTIONS -O1)
target_link_libraries(translator_tests PRIVATE wp_engine)
add_test(NAME translator_tests COMMAND translator_tests)
