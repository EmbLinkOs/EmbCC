# EmbCC as a CMake toolchain.
#
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=<prefix>/share/embcc/cmake/embcc.cmake \
#         -DEMBCC_TARGET=thumbv7em-none-eabihf [-DEMBCC_FLAGS="-mcpu=cortex-m4"]
#
# EMBCC_TARGET is any triple `embcc --target=` takes; without it the build
# is for the compiler's default target. EMBCC_FLAGS are added to every
# compile and link (a CPU, an FPU, -Os). Everything else a build asks of a
# toolchain comes from EmbCC too: the archiver (embar, which writes its own
# symbol index, so there is no ranlib step), objcopy, size and nm (embpack
# and embmap under binutils' names), and header dependencies (-MD), so
# that editing a header rebuilds what includes it.
#
# Found next to itself: installed, this file is in share/embcc/cmake and
# the tools in bin; in a source tree it is in cmake/ and the tools at the
# top.

set(EMBCC_TARGET "" CACHE STRING "the triple EmbCC compiles for (embcc --target=)")
set(EMBCC_FLAGS "" CACHE STRING "flags for every compile and link: -mcpu=, -mfpu=, -O...")

set(_embcc_bin "")
foreach(_d "${CMAKE_CURRENT_LIST_DIR}/../../../bin" "${CMAKE_CURRENT_LIST_DIR}/..")
  if(NOT _embcc_bin AND EXISTS "${_d}/embcc")
    get_filename_component(_embcc_bin "${_d}" ABSOLUTE)
  endif()
endforeach()
if(NOT _embcc_bin)
  message(FATAL_ERROR "embcc.cmake: no embcc next to ${CMAKE_CURRENT_LIST_DIR} "
                      "(installed: ../../../bin; in the source tree: ..)")
endif()

set(CMAKE_C_COMPILER "${_embcc_bin}/embcc")
set(CMAKE_CXX_COMPILER "${_embcc_bin}/embcc")
set(CMAKE_ASM_COMPILER "${_embcc_bin}/embcc")

# The system CMake should think it is building for, from the triple.
set(_embcc_t "${EMBCC_TARGET}")
if(NOT _embcc_t)
  execute_process(COMMAND "${CMAKE_C_COMPILER}" -dumpmachine
                  OUTPUT_VARIABLE _embcc_t OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
string(REGEX MATCH "^[^-]+" CMAKE_SYSTEM_PROCESSOR "${_embcc_t}")
if(_embcc_t MATCHES "-linux")
  set(CMAKE_SYSTEM_NAME Linux)
elseif(_embcc_t MATCHES "-apple-darwin")
  set(CMAKE_SYSTEM_NAME Darwin)
elseif(_embcc_t MATCHES "-windows|-mingw")
  set(CMAKE_SYSTEM_NAME Windows)
else()
  # a part with no operating system: CMake's checks link nothing
  set(CMAKE_SYSTEM_NAME Generic)
  set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
endif()

set(_embcc_flags "${EMBCC_FLAGS}")
if(EMBCC_TARGET)
  set(_embcc_flags "--target=${EMBCC_TARGET} ${_embcc_flags}")
endif()
foreach(_lang C CXX ASM)
  set(CMAKE_${_lang}_FLAGS_INIT "${_embcc_flags}")
  # header dependencies, in the form gcc writes them
  set(CMAKE_DEPFILE_FLAGS_${_lang} "-MD -MT <DEP_TARGET> -MF <DEP_FILE>")
  set(CMAKE_${_lang}_DEPFILE_FORMAT gcc)
  set(CMAKE_${_lang}_DEPENDS_USE_COMPILER TRUE)
  # embar writes the symbol index as it writes the archive
  set(CMAKE_${_lang}_ARCHIVE_CREATE "<CMAKE_AR> qcs <TARGET> <LINK_FLAGS> <OBJECTS>")
  set(CMAKE_${_lang}_ARCHIVE_APPEND "<CMAKE_AR> qs <TARGET> <LINK_FLAGS> <OBJECTS>")
  set(CMAKE_${_lang}_ARCHIVE_FINISH "")
endforeach()
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_embcc_flags}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_embcc_flags}")

set(CMAKE_AR "${_embcc_bin}/embar" CACHE FILEPATH "embar")
set(CMAKE_RANLIB "${_embcc_bin}/embcc-ranlib" CACHE FILEPATH "embar as ranlib")
set(CMAKE_OBJCOPY "${_embcc_bin}/embcc-objcopy" CACHE FILEPATH "embpack as objcopy")
set(CMAKE_SIZE "${_embcc_bin}/embcc-size" CACHE FILEPATH "embmap as size")
set(CMAKE_NM "${_embcc_bin}/embcc-nm" CACHE FILEPATH "embmap as nm")

# A bare-metal build finds no libraries or headers on the host.
if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
  set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
  set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
  set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
  set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
endif()
