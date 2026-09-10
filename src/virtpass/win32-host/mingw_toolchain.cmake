# MinGW toolchain for cross-compiling from Windows to Windows
# (native build, but using MinGW toolchain)

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER "C:/msys64/mingw64/bin/gcc.exe")
set(CMAKE_CXX_COMPILER "C:/msys64/mingw64/bin/g++.exe")
set(CMAKE_RC_COMPILER "C:/msys64/mingw64/bin/windres.exe")

set(CMAKE_FIND_ROOT_PATH "C:/msys64/mingw64")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)