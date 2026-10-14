# EmbCC assembling, for CMake (the shared rules: Compiler/EmbCC.cmake):
# GNU-syntax .s and .S (preprocessed first) through the built-in
# assembler, NASM-syntax .asm for x86-64.
include(Compiler/EmbCC)
set(CMAKE_ASM_SOURCE_FILE_EXTENSIONS s;S;asm)
__compiler_embcc(ASM)
