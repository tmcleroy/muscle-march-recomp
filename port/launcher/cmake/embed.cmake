get_filename_component(WP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" ls-files -- CMakeLists.txt LICENSE THIRD_PARTY_NOTICES.md engine recompiler games/wiiparty third_party tests tools/fetch_sdl.py
                WORKING_DIRECTORY "${WP_ROOT}" OUTPUT_VARIABLE WP_SOURCE_LIST OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND "${GIT_EXECUTABLE}" describe --always --dirty
                WORKING_DIRECTORY "${WP_ROOT}" OUTPUT_VARIABLE WP_SOURCE_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WP_ROOT}/.git/index")

set(WP_PAYLOAD_LIST "${CMAKE_CURRENT_BINARY_DIR}/payload.txt")
set(WP_PAYLOAD "${CMAKE_CURRENT_BINARY_DIR}/payload.tar.gz")
file(WRITE "${WP_PAYLOAD_LIST}.in" "${WP_SOURCE_LIST}\n")
configure_file("${WP_PAYLOAD_LIST}.in" "${WP_PAYLOAD_LIST}" COPYONLY)
string(REPLACE "\n" ";" WP_SOURCE_FILES "${WP_SOURCE_LIST}")
list(TRANSFORM WP_SOURCE_FILES PREPEND "${WP_ROOT}/")
add_custom_command(OUTPUT "${WP_PAYLOAD}"
    COMMAND "${CMAKE_COMMAND}" -E tar czf "${WP_PAYLOAD}" --format=gnutar "--files-from=${WP_PAYLOAD_LIST}"
    WORKING_DIRECTORY "${WP_ROOT}"
    DEPENDS "${WP_PAYLOAD_LIST}" ${WP_SOURCE_FILES}
    VERBATIM)

set(WP_BLOBS
    payload "${WP_PAYLOAD}"
    font_regular "${inter_SOURCE_DIR}/extras/ttf/Inter-Regular.ttf"
    font_semibold "${inter_SOURCE_DIR}/extras/ttf/Inter-SemiBold.ttf"
    license_project "${WP_ROOT}/LICENSE"
    license_notices "${WP_ROOT}/THIRD_PARTY_NOTICES.md"
    license_sdl "${sdl3_SOURCE_DIR}/LICENSE.txt"
    license_imgui "${imgui_SOURCE_DIR}/LICENSE.txt"
    license_inter "${inter_SOURCE_DIR}/LICENSE.txt"
    license_winpthreads "${CMAKE_CURRENT_SOURCE_DIR}/licenses/winpthreads.txt"
    license_gcc "${CMAKE_CURRENT_SOURCE_DIR}/licenses/gcc-runtime-library-exception.txt")

if(APPLE)
    set(WP_SECTION ".const_data")
    set(WP_PREFIX "_")
elseif(WIN32)
    set(WP_SECTION ".section .rdata,\\\"dr\\\"")
    set(WP_PREFIX "")
else()
    set(WP_SECTION ".section .rodata")
    set(WP_PREFIX "")
endif()

set(WP_ASM "")
set(WP_TABLE "")
set(WP_EXTERNS "")
set(WP_BLOB_FILES "")
list(LENGTH WP_BLOBS WP_BLOB_LENGTH)
math(EXPR WP_BLOB_LAST "${WP_BLOB_LENGTH} - 1")
foreach(WP_INDEX RANGE 0 ${WP_BLOB_LAST} 2)
    math(EXPR WP_PATH_INDEX "${WP_INDEX} + 1")
    list(GET WP_BLOBS ${WP_INDEX} WP_NAME)
    list(GET WP_BLOBS ${WP_PATH_INDEX} WP_PATH)
    list(APPEND WP_BLOB_FILES "${WP_PATH}")
    string(APPEND WP_ASM "    \"${WP_SECTION}\\n.balign 16\\n.globl ${WP_PREFIX}wp_blob_${WP_NAME}\\n${WP_PREFIX}wp_blob_${WP_NAME}:\\n.incbin \\\"${WP_PATH}\\\"\\n.globl ${WP_PREFIX}wp_blob_${WP_NAME}_end\\n${WP_PREFIX}wp_blob_${WP_NAME}_end:\\n.byte 0\\n\"\n")
    string(APPEND WP_EXTERNS "extern \"C\" const unsigned char wp_blob_${WP_NAME}[];\nextern \"C\" const unsigned char wp_blob_${WP_NAME}_end[];\n")
    string(APPEND WP_TABLE "    {\"${WP_NAME}\", wp_blob_${WP_NAME}, wp_blob_${WP_NAME}_end},\n")
endforeach()

set(WP_EMBEDDED "${CMAKE_CURRENT_BINARY_DIR}/embedded.cpp")
file(WRITE "${WP_EMBEDDED}.in" "#include \"embedded.h\"\n\n#include <cstring>\n\nasm(\n${WP_ASM});\n\n${WP_EXTERNS}\nnamespace {\n\nstruct Entry {\n    const char* name;\n    const unsigned char* begin;\n    const unsigned char* end;\n};\n\nconst Entry kEntries[] = {\n${WP_TABLE}};\n\n}\n\nconst char* source_revision() {\n    return \"${WP_SOURCE_REVISION}\";\n}\n\nstd::string_view blob(const char* name) {\n    for (const Entry& entry : kEntries) {\n        if (std::strcmp(entry.name, name) == 0) {\n            return {reinterpret_cast<const char*>(entry.begin), static_cast<size_t>(entry.end - entry.begin)};\n        }\n    }\n    return {};\n}\n")
configure_file("${WP_EMBEDDED}.in" "${WP_EMBEDDED}" COPYONLY)
target_sources(launcher PRIVATE "${WP_EMBEDDED}")
set_source_files_properties("${WP_EMBEDDED}" PROPERTIES OBJECT_DEPENDS "${WP_BLOB_FILES}")
