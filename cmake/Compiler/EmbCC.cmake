# EmbCC's rules for CMake, shared by C, C++ and assembler
# (Compiler/EmbCC-C.cmake and the others). CMake reads these because
# embcc.cmake names the compiler EmbCC and puts this directory on
# CMAKE_MODULE_PATH: CMake identifies a compiler by building a test file
# against the list of compilers it knows, which has no EmbCC in it.
#
# GCC's spellings, which EmbCC accepts (its own arrive with the option
# table, and these will move to them). Not CMake's GNU module: that one
# switches on GCC's version numbers, and asks for what EmbCC refuses.
if(__COMPILER_EMBCC)
  return()
endif()
set(__COMPILER_EMBCC 1)

include(Compiler/CMakeCommonCompilerMacros)

macro(__compiler_embcc lang)
  # No verbose flag: CMake passes one to read GCC's link line for the
  # libraries it links implicitly, and EmbCC links its own, in-process.
  set(CMAKE_${lang}_COMPILE_OPTIONS_WARNING_AS_ERROR "-Werror")

  # EmbCC emits no position-independent code and links no shared library.
  # Asked for either, it refuses by name rather than build ordinary code
  # (docs/manual/invoking.md, Refused options), so these are GCC's
  # spellings, which reach that refusal. CMake's IPO check finds no -flto,
  # which EmbCC refuses too.
  set(CMAKE_${lang}_COMPILE_OPTIONS_PIC "-fPIC")
  set(CMAKE_${lang}_COMPILE_OPTIONS_PIE "-fPIE")
  set(CMAKE_SHARED_LIBRARY_${lang}_FLAGS "-fPIC")
  set(CMAKE_SHARED_LIBRARY_CREATE_${lang}_FLAGS "-shared")
  set(_CMAKE_${lang}_IPO_SUPPORTED_BY_CMAKE YES)
  set(_CMAKE_${lang}_IPO_MAY_BE_SUPPORTED_BY_COMPILER NO)

  set(CMAKE_${lang}_LINKER_WRAPPER_FLAG "-Wl,")
  set(CMAKE_${lang}_LINKER_WRAPPER_FLAG_SEP ",")
  set(CMAKE_${lang}_LINK_MODE DRIVER)

  # header dependencies: the rule for every header, system ones included,
  # for the target CMake names
  set(CMAKE_DEPFILE_FLAGS_${lang} "-MD -MT <DEP_TARGET> -MF <DEP_FILE>")
  set(CMAKE_${lang}_DEPFILE_FORMAT gcc)
  set(CMAKE_${lang}_DEPENDS_USE_COMPILER TRUE)

  set(CMAKE_INCLUDE_SYSTEM_FLAG_${lang} "-isystem ")

  string(APPEND CMAKE_${lang}_FLAGS_INIT " ")
  string(APPEND CMAKE_${lang}_FLAGS_DEBUG_INIT " -g")
  string(APPEND CMAKE_${lang}_FLAGS_MINSIZEREL_INIT " -Os -DNDEBUG")
  string(APPEND CMAKE_${lang}_FLAGS_RELEASE_INIT " -O2 -DNDEBUG")
  string(APPEND CMAKE_${lang}_FLAGS_RELWITHDEBINFO_INIT " -O2 -g -DNDEBUG")

  set(CMAKE_${lang}_CREATE_PREPROCESSED_SOURCE
      "<CMAKE_${lang}_COMPILER> <DEFINES> <INCLUDES> <FLAGS> -E <SOURCE> > <PREPROCESSED_SOURCE>")
  set(CMAKE_${lang}_CREATE_ASSEMBLY_SOURCE
      "<CMAKE_${lang}_COMPILER> <DEFINES> <INCLUDES> <FLAGS> -S <SOURCE> -o <ASSEMBLY_SOURCE>")

  # embar writes the symbol index as it writes the archive: no ranlib step
  set(CMAKE_${lang}_ARCHIVE_CREATE "<CMAKE_AR> qcs <TARGET> <LINK_FLAGS> <OBJECTS>")
  set(CMAKE_${lang}_ARCHIVE_APPEND "<CMAKE_AR> qs <TARGET> <LINK_FLAGS> <OBJECTS>")
  set(CMAKE_${lang}_ARCHIVE_FINISH "")
endmacro()
