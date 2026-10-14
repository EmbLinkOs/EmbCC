# EmbCC compiling C, for CMake (the shared rules: Compiler/EmbCC.cmake).
# EmbCC reads one dialect, C11 with the GNU extensions; -std= sets the
# standard's macros (__STDC_VERSION__), and below C11 says that it does
# not enforce the older one (docs/manual/c-language.md).
include(Compiler/EmbCC)
# what CMake says of a compiler it identified, once per build tree
if(NOT _EMBCC_C_ID_SHOWN)
  message(STATUS "The C compiler identification is EmbCC ${CMAKE_C_COMPILER_VERSION}")
  set(_EMBCC_C_ID_SHOWN 1 CACHE INTERNAL "")
endif()
__compiler_embcc(C)
set(CMAKE_C_COMPILE_OPTIONS_EXPLICIT_LANGUAGE -x c)

foreach(_embcc_std 90 99 11 17 23)
  set(CMAKE_C${_embcc_std}_STANDARD_COMPILE_OPTION "-std=c${_embcc_std}")
  set(CMAKE_C${_embcc_std}_EXTENSION_COMPILE_OPTION "-std=gnu${_embcc_std}")
  set(CMAKE_C${_embcc_std}_STANDARD__HAS_FULL_SUPPORT ON)
endforeach()
unset(_embcc_std)
set(CMAKE_C_STANDARD_LATEST 23)

# What `embcc` compiles with no -std=: C17's __STDC_VERSION__, extensions
# on. CMake reads these from the compiler when it identifies it, which for
# EmbCC it does not do (embcc.cmake names it instead).
set(CMAKE_C_STANDARD_COMPUTED_DEFAULT 17)
set(CMAKE_C_EXTENSIONS_COMPUTED_DEFAULT ON)
__compiler_check_default_language_standard(C 1.0 17)
