cmake_minimum_required(VERSION 3.25)
project(MudletInstaller VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    set(QT_STATIC ON)
    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")

    # Enable static runtime linking (GCC/MinGW)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static -static-libgcc -static-libstdc++")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -Wl,--gc-sections")

endif()

# Explicitly prevent CMake from finding system Qt6 when we want static build
if(WIN32 AND DEFINED CMAKE_PREFIX_PATH)
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
    set(QT_NO_CMAKE_PACKAGE_CACHE ON)
endif()


find_package(QT NAMES Qt6 REQUIRED COMPONENTS Core Widgets Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Widgets Network)

# Try to find StateMachine module
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS StateMachine)

find_package(Qt6LinguistTools QUIET)

if(NOT Qt6LinguistTools_FOUND)
    message(STATUS "Qt6LinguistTools not found in static build, trying system installation...")
    # If not found, try to find it in the system installation
    find_path(Qt6LinguistTools_DIR
        NAMES Qt6LinguistToolsConfig.cmake
        PATHS
            /clang64/lib/cmake/Qt6LinguistTools
            /clang64/share/cmake/Qt6LinguistTools
        NO_DEFAULT_PATH
    )
    if(Qt6LinguistTools_DIR)
        find_package(Qt6LinguistTools PATHS ${Qt6LinguistTools_DIR} NO_DEFAULT_PATH)
    endif()
endif()

if(Qt6LinguistTools_FOUND)
    message(STATUS "Found Qt6LinguistTools at: ${Qt6LinguistTools_DIR}")
else()
    message(WARNING "Qt6LinguistTools not found - translations will be disabled")
endif()


set(TS_FILES MudletInstaller_en_US.ts)
set(PROJECT_SOURCES
        main.cpp
        MudletInstaller.h
        MudletInstaller.cpp
        ${TS_FILES}
)

qt_add_executable(MudletInstaller
    MANUAL_FINALIZATION
    ${PROJECT_SOURCES}
)

# Add resources
qt_add_resources(RESOURCES resources.qrc)
target_sources(MudletInstaller PRIVATE ${RESOURCES})

# Link Qt libraries
target_link_libraries(MudletInstaller PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
    Qt${QT_VERSION_MAJOR}::StateMachine
)


if(WIN32)
    if(DEFINED ENV{MINGW_PREFIX})
        # MINGW_PREFIX should be set by GHA
        # May be a Unix-style path when running outside MSYS2 shell
        set(_MINGW_PREFIX "$ENV{MINGW_PREFIX}")
        if(NOT _MINGW_PREFIX MATCHES "^[A-Za-z]:")
            set(_MINGW_PREFIX "C:/msys64${_MINGW_PREFIX}")
        endif()
        set(MSYS2_LIB_PATH "${_MINGW_PREFIX}/lib")
        message(STATUS "Using MINGW_PREFIX: ${MSYS2_LIB_PATH}")
    elseif(DEFINED ENV{MSYSTEM_PREFIX})
        # MSYSTEM_PREFIX is another MSYS2 environment variable
        # It may be a Unix-style path (e.g. /clang64) when running outside MSYS2 shell,
        # so prepend MSYS2 root if the path doesn't look like a Windows absolute path
        set(_MSYSTEM_PREFIX "$ENV{MSYSTEM_PREFIX}")
        if(NOT _MSYSTEM_PREFIX MATCHES "^[A-Za-z]:")
            set(_MSYSTEM_PREFIX "C:/msys64${_MSYSTEM_PREFIX}")
        endif()
        set(MSYS2_LIB_PATH "${_MSYSTEM_PREFIX}/lib")
        message(STATUS "Using MSYSTEM_PREFIX: ${MSYS2_LIB_PATH}")
    elseif(EXISTS "D:/a/_temp/msys64/clang64/lib")
        # GHA path (D: drive)
        set(MSYS2_LIB_PATH "D:/a/_temp/msys64/clang64/lib")
        message(STATUS "Using GitHub Actions D: path: ${MSYS2_LIB_PATH}")
    elseif(EXISTS "C:/a/_temp/msys64/clang64/lib")
        # GHA path (C: drive)
        set(MSYS2_LIB_PATH "C:/a/_temp/msys64/clang64/lib")
        message(STATUS "Using GitHub Actions C: path: ${MSYS2_LIB_PATH}")
    elseif(EXISTS "C:/msys64/clang64/lib")
        # Local MSYS2 installation
        set(MSYS2_LIB_PATH "C:/msys64/clang64/lib")
        message(STATUS "Using local MSYS2 path: ${MSYS2_LIB_PATH}")
    else()
        # Fallback: detect from compiler path
        get_filename_component(COMPILER_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
        get_filename_component(MSYS2_ROOT ${COMPILER_DIR} DIRECTORY)
        get_filename_component(MSYS2_ROOT ${MSYS2_ROOT} DIRECTORY)
        set(MSYS2_LIB_PATH "${MSYS2_ROOT}/lib")
        message(STATUS "Detected MSYS2 lib path from compiler: ${MSYS2_LIB_PATH}")
    endif()
    
    # Verify the path exists and contains expected libraries
    if(NOT EXISTS "${MSYS2_LIB_PATH}")
        message(WARNING "MSYS2 lib path does not exist: ${MSYS2_LIB_PATH}")
    endif()
    
    message(STATUS "Using MSYS2 lib path: ${MSYS2_LIB_PATH}")
    
    # Verify the libraries exist
    if(NOT EXISTS "${MSYS2_LIB_PATH}/libbz2.a")
        message(FATAL_ERROR "libbz2.a not found at ${MSYS2_LIB_PATH}/libbz2.a")
    endif()
    if(NOT EXISTS "${MSYS2_LIB_PATH}/libbrotlicommon.a")
        message(FATAL_ERROR "libbrotlicommon.a not found at ${MSYS2_LIB_PATH}/libbrotlicommon.a")
    endif()
    if(NOT EXISTS "${MSYS2_LIB_PATH}/libzstd.a")
        message(FATAL_ERROR "libzstd.a not found at ${MSYS2_LIB_PATH}/libzstd.a")
    endif()

    # Don't link libraries early - let Qt's dependencies handle them
    # Just ensure they're available at the end with --whole-archive
    target_link_options(MudletInstaller PRIVATE
        "LINKER:--whole-archive"
        "LINKER:${MSYS2_LIB_PATH}/libbz2.a"
        "LINKER:${MSYS2_LIB_PATH}/libbrotlicommon.a"
        "LINKER:${MSYS2_LIB_PATH}/libzstd.a"
        "LINKER:${MSYS2_LIB_PATH}/libmd4c.a"
        "LINKER:--no-whole-archive"
    )

    # Only link Windows system libraries
    target_link_libraries(MudletInstaller PRIVATE
        ws2_32 winmm imm32 wtsapi32 dwmapi uxtheme
        rpcrt4 crypt32 secur32 ole32 oleaut32 uuid
        advapi32 shell32 user32 kernel32 gdi32 comdlg32 winspool
    )
endif()


message(STATUS
    "Project Version: ${PROJECT_VERSION}\n"
    "Project Version Major: ${PROJECT_VERSION_MAJOR}\n"
    "Project Version Minor: ${PROJECT_VERSION_MINOR}"
)

set_target_properties(MudletInstaller PROPERTIES
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE_LONG_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_ICON_FILE mudlet.icns
    MACOSX_BUNDLE_BUNDLE_NAME MudletInstaller
    MACOSX_BUNDLE_GUI_IDENTIFIER org.mudlet.installer
    WIN32_EXECUTABLE TRUE
)

if(APPLE)
    file(GLOB ICON_FILE "mudlet.icns")
    message(STATUS "Using icon file: ${ICON_FILE}")
    target_sources(MudletInstaller PRIVATE ${ICON_FILE})
    set_source_files_properties(${ICON_FILE} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()

include(GNUInstallDirs)
install(TARGETS MudletInstaller
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(MudletInstaller)
endif()
