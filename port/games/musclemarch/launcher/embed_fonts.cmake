# cmake -DOUT=fonts.cpp -DREGULAR=a.ttf -DSEMIBOLD=b.ttf -P embed_fonts.cmake
# Writes a C++ file with the fonts as byte arrays and blob(name) / source_revision() for ui.cpp.
function(embed name file out_var)
    file(READ "${file}" hex HEX)
    string(LENGTH "${hex}" length)
    math(EXPR count "${length} / 2")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    set(${out_var} "const unsigned char k_${name}[${count}] = {${bytes}};\nconst size_t k_${name}_size = ${count};\n" PARENT_SCOPE)
endfunction()

embed(font_regular "${REGULAR}" regular)
embed(font_semibold "${SEMIBOLD}" semibold)
file(WRITE "${OUT}" "#include <cstddef>\n#include <cstring>\n#include <string_view>\n\nnamespace {\n${regular}${semibold}}\n\n"
"const char* source_revision() { return \"\"; }\n\n"
"std::string_view blob(const char* name) {\n"
"    if (std::strcmp(name, \"font_regular\") == 0) return {reinterpret_cast<const char*>(k_font_regular), k_font_regular_size};\n"
"    if (std::strcmp(name, \"font_semibold\") == 0) return {reinterpret_cast<const char*>(k_font_semibold), k_font_semibold_size};\n"
"    return {};\n"
"}\n")
