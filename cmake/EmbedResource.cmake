# Embeds a binary resource (shader bytecode, compiled SPIR-V, …) as a C/C++
# static array so backends ship self-contained.
#
# Usage:
#   cmake -DSOURCE_DIR=<dir> -DFILE=<name> -P EmbedResource.cmake
#
# Writes <name>.h in the current directory, defining
#   static const char <NAME>[]  (FILE name with '.' and '/' → '_', upper-cased)
# Consumers include the header and use that array + strlen()/sizeof()-1 as the
# byte blob.
#
# Adapted from the BRender-v1.3.2 drivers' cmake/EmbedResource.cmake
# (original function by amir-saniyan; Argonaut Software, MIT License).

function(embed_resource resource_file_name source_file_name variable_name)

if(EXISTS "${source_file_name}")
    if("${source_file_name}" IS_NEWER_THAN "${resource_file_name}")
        return()
    endif()
endif()

file(READ "${resource_file_name}" hex_content HEX)

if(hex_content STREQUAL "")
    # Empty input (e.g. a tool produced no output). Emit a valid zero-length
    # array so the generated header still compiles; consumers can size-check.
    set(array_definition "static const char ${variable_name}[1] = { 0 };")
else()
    string(REPEAT "[0-9a-f]" 32 pattern)
    string(REGEX REPLACE "(${pattern})" "\\1\n" content "${hex_content}")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " content "${content}")
    string(REGEX REPLACE ", $" "" content "${content}")
    set(array_definition "static const char ${variable_name}[] =\n{\n${content}\n};")
endif()
set(source "// Auto generated file.\n${array_definition}\n")
file(WRITE "${source_file_name}" "${source}")

endfunction()


if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is not set")
endif(NOT DEFINED SOURCE_DIR)

if(NOT DEFINED FILE)
    message(FATAL_ERROR "FILE is not set")
endif(NOT DEFINED FILE)

string(REPLACE "." "_" generated_name "${FILE}")
string(REPLACE "/" "_" generated_name "${generated_name}")
string(TOUPPER "${generated_name}" generated_name)
embed_resource("${SOURCE_DIR}/${FILE}" "${FILE}.h" "${generated_name}")