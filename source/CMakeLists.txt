cmake_minimum_required(VERSION 3.16)
project(TCLib CXX)

#========== Global Configurations =============#
#----------------------------------------------#

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

#========== Targets Configurations ============#

# El patcher lee resources/, sym/ y sub/ del directorio del ejecutable, asi que
# el binario se deja en la raiz del repositorio, al lado de esas carpetas.
add_executable(TCLib TCLib.cpp setup-funcs.cpp patcher.cpp)
set_target_properties(TCLib PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")

if(MINGW)
  # Sin esto el .exe necesita las DLL del toolchain al lado.
  target_link_options(TCLib PRIVATE -static)
endif()

#================== Tests =====================#

# Los tests solo necesitan patcher.cpp: no dependen de LTspice, de miniaudio ni
# de la consola, asi que corren en cualquier lado.
enable_testing()

add_executable(run-tests ../tests/run-tests.cpp patcher.cpp)
target_include_directories(run-tests PRIVATE .)

if(MINGW)
  target_link_options(run-tests PRIVATE -static)
endif()

add_test(NAME patcher
         COMMAND run-tests
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")
