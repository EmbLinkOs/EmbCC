# EmbCC compiling C++, for CMake (the shared rules: Compiler/EmbCC.cmake).
# EmbCC parses C++20 with C++23's additions; -std= sets __cplusplus, and
# below C++20 says that it does not enforce the older standard
# (docs/manual/cxx.md).
include(Compiler/EmbCC)
# what CMake says of a compiler it identified, once per build tree
if(NOT _EMBCC_CXX_ID_SHOWN)
  message(STATUS "The CXX compiler identification is EmbCC ${CMAKE_CXX_COMPILER_VERSION}")
  set(_EMBCC_CXX_ID_SHOWN 1 CACHE INTERNAL "")
endif()
__compiler_embcc(CXX)
set(CMAKE_CXX_COMPILE_OPTIONS_EXPLICIT_LANGUAGE -x c++)

foreach(_embcc_std 98 11 14 17 20 23 26)
  set(CMAKE_CXX${_embcc_std}_STANDARD_COMPILE_OPTION "-std=c++${_embcc_std}")
  set(CMAKE_CXX${_embcc_std}_EXTENSION_COMPILE_OPTION "-std=gnu++${_embcc_std}")
  set(CMAKE_CXX${_embcc_std}_STANDARD__HAS_FULL_SUPPORT ON)
endforeach()
unset(_embcc_std)
set(CMAKE_CXX_STANDARD_LATEST 26)

# What `embcc` compiles a C++ unit as with no -std=: gnu++20.
set(CMAKE_CXX_STANDARD_COMPUTED_DEFAULT 20)
set(CMAKE_CXX_EXTENSIONS_COMPUTED_DEFAULT ON)
__compiler_check_default_language_standard(CXX 1.0 20)
