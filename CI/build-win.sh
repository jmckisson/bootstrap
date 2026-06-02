#!/bin/bash
###########################################################################
#   Copyright (C) 2024-2026  by John McKisson - john.mckisson@gmail.com   #
#   Copyright (C) 2023-2024  by Stephen Lyons - slysven@virginmedia.com   #
#                                                                         #
#   This program is free software; you can redistribute it and/or modify  #
#   it under the terms of the GNU General Public License as published by  #
#   the Free Software Foundation; either version 2 of the License, or     #
#   (at your option) any later version.                                   #
#                                                                         #
#   This program is distributed in the hope that it will be useful,       #
#   but WITHOUT ANY WARRANTY; without even the implied warranty of        #
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         #
#   GNU General Public License for more details.                          #
#                                                                         #
#   You should have received a copy of the GNU General Public License     #
#   along with this program; if not, write to the                         #
#   Free Software Foundation, Inc.,                                       #
#   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             #
###########################################################################

# Version: 2.0.0    Rework to build on an MSYS2 MINGW64 Github workflow
#          1.5.0    Change BUILD_TYPE to BUILD_CONFIG to avoid clash with
#                   CI/CB system using same variable
#          1.4.0    Rewrite Makefile to use ccache.exe if available
#          1.3.0    No changes
#          1.2.0    No changes
#          1.1.0    No changes
#          1.0.0    Original version

# Script to build the Mudlet code currently checked out in
# ${GITHUB_WORKSPACE} in a MINGW32 or MINGW64 shell

# To be used AFTER setup-windows-sdk.sh has been run; once this has completed
# successfully, package-mudlet-for-windows.sh is run by the workflow

# Exit codes:
# 0 - Everything is fine. 8-)
# 1 - Failure to change to a directory
# 2 - Unsupported MSYS2/MINGGW shell type
# 3 - Unsupported build type

if [ "${MSYSTEM}" != "CLANG64" ]; then
  echo "Please run this script from a CLANG64 type bash terminal."
  echo "Current MSYSTEM is: ${MSYSTEM}"
  exit 2
fi

export MINGW_BASE_DIR="${MSYSTEM_PREFIX}"
export PATH="${MINGW_BASE_DIR}/usr/local/bin:${MINGW_BASE_DIR}/bin:/usr/bin:${PATH}"
RUNNER_WORKSPACE_UNIX_PATH=$(echo "${RUNNER_WORKSPACE}" | sed 's|\\|/|g' | sed 's|D:|/d|g')
export CCACHE_DIR=${RUNNER_WORKSPACE_UNIX_PATH}/ccache

echo "MSYSTEM is: ${MSYSTEM}"
echo "CCACHE_DIR is: ${CCACHE_DIR}"
echo "PATH is now:"
echo "${PATH}"
echo ""

cd $GITHUB_WORKSPACE || exit 1

LAUNCH_INI_PATH="${GITHUB_WORKSPACE}/resources/launch.ini"
Qt6_PREFIX=${RUNNER_WORKSPACE}/qt-static-install
QT_DIR=${Qt6_PREFIX}/lib/cmake/Qt6
export QT_DIR
QT_LINGUIST_DIR=$(cygpath -w ${MSYSTEM_PREFIX}/lib/cmake/Qt6LinguistTools)
echo "Qt6_PREFIX is: ${Qt6_PREFIX}"
echo "QT_DIR is: ${QT_DIR}"
echo "QT_LINGUIST_DIR is: ${QT_LINGUIST_DIR}"

# Configure ONCE in a single build tree. Every game produces an identical binary
# apart from the embedded launch.ini, so we configure/compile once and only relink
# per game further down. The bundled-Qt cmake fixups below also run just once.
BUILD_DIR="${GITHUB_WORKSPACE}/build"
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}" || exit 1

  # Check if Qt6Config.cmake exists
  QT_CONFIG_FILE="${RUNNER_WORKSPACE}/qt-static-install/lib/cmake/Qt6/Qt6Config.cmake"
  if [ -f "${QT_CONFIG_FILE}" ]; then
    echo "Found Qt6Config.cmake at: ${QT_CONFIG_FILE}"
  else
    echo "ERROR: Qt6Config.cmake not found at: ${QT_CONFIG_FILE}"
    # Try to find it
    find "${RUNNER_WORKSPACE}/qt-static-install" -name "Qt6Config.cmake" -type f
    exit 1
  fi

  # Check if LinguistTools exists
  if [ -f "${QT_LINGUIST_DIR}/Qt6LinguistToolsConfig.cmake" ]; then
    echo "Found Qt6LinguistToolsConfig.cmake at: ${QT_LINGUIST_DIR}/Qt6LinguistToolsConfig.cmake"
  else
    echo "ERROR: Qt6LinguistToolsConfig.cmake not found at: ${QT_LINGUIST_DIR}/Qt6LinguistToolsConfig.cmake"
    find "${MSYSTEM_PREFIX}" -name "Qt6LinguistToolsConfig.cmake" -type f 2>/dev/null
    exit 1
  fi

  # Fix missing Qt bundled library CMake files
  echo "Fixing missing Qt bundled library CMake files..."
  QT_CMAKE_DIR="${RUNNER_WORKSPACE}/qt-static-install/lib/cmake"

  # Debug: Check what PCRE2 libraries are available
  echo "Checking for PCRE2 libraries..."
  find ${MSYSTEM_PREFIX}/lib -name "*pcre*" -type f 2>/dev/null | head -5
  find "${RUNNER_WORKSPACE}/qt-static-install" -name "*pcre*" -type f 2>/dev/null | head -5

  # Find the actual PCRE2 library path
  PCRE2_LIB_PATH=$(find ${MSYSTEM_PREFIX}/lib -name "libpcre2-16.a" -type f | head -1)
  if [ -z "$PCRE2_LIB_PATH" ]; then
    PCRE2_LIB_PATH=$(find ${MSYSTEM_PREFIX}/lib -name "*pcre2*" -type f | head -1)
  fi
  echo "Found PCRE2 library at: $PCRE2_LIB_PATH"

  # Create Qt6BundledPcre2
  if [ ! -f "${QT_CMAKE_DIR}/Qt6BundledPcre2/Qt6BundledPcre2Config.cmake" ]; then
    mkdir -p "${QT_CMAKE_DIR}/Qt6BundledPcre2"
    cat > "${QT_CMAKE_DIR}/Qt6BundledPcre2/Qt6BundledPcre2Config.cmake" << 'EOF'
set(Qt6BundledPcre2_FOUND TRUE)

if(NOT TARGET Qt6::BundledPcre2)
    # First try to find Qt's bundled PCRE2
    find_library(QT_BUNDLED_PCRE2_LIB
        NAMES qtpcre2 qt6pcre2 libqtpcre2 libqt6pcre2
        PATHS
            "${CMAKE_CURRENT_LIST_DIR}/../../"
            "${CMAKE_CURRENT_LIST_DIR}/../../../"
        PATH_SUFFIXES lib
        NO_DEFAULT_PATH
    )

    # If not found, use system PCRE2
    if(NOT QT_BUNDLED_PCRE2_LIB)
        find_library(QT_BUNDLED_PCRE2_LIB
            NAMES pcre2-16 libpcre2-16
            PATHS
                "$ENV{MSYSTEM_PREFIX}/lib"
            NO_DEFAULT_PATH
        )
    endif()

    # Last resort - try without NO_DEFAULT_PATH
    if(NOT QT_BUNDLED_PCRE2_LIB)
        find_library(QT_BUNDLED_PCRE2_LIB
            NAMES pcre2-16 libpcre2-16 qtpcre2 qt6pcre2
        )
    endif()

    if(QT_BUNDLED_PCRE2_LIB)
        add_library(Qt6::BundledPcre2 STATIC IMPORTED)
        set_target_properties(Qt6::BundledPcre2 PROPERTIES
            IMPORTED_LOCATION "${QT_BUNDLED_PCRE2_LIB}"
            INTERFACE_COMPILE_DEFINITIONS "PCRE2_STATIC"
        )
        message(STATUS "Qt6::BundledPcre2 using: ${QT_BUNDLED_PCRE2_LIB}")
    else()
        # Create a dummy target to avoid fatal error
        add_library(Qt6::BundledPcre2 INTERFACE IMPORTED)
        message(WARNING "Could not find PCRE2 library - created dummy target")
    endif()
endif()

set(Qt6BundledPcre2_VERSION "QT_VERSION_PLACEHOLDER")
EOF
    sed -i "s/QT_VERSION_PLACEHOLDER/${QT_VERSION}/" "${QT_CMAKE_DIR}/Qt6BundledPcre2/Qt6BundledPcre2Config.cmake"
    echo "Created Qt6BundledPcre2Config.cmake"
  fi

  # Create other bundled lib configs if needed
  for bundled_lib in "Zlib" "Freetype" "Harfbuzz" "Jpeg" "Png"; do
    if [ ! -f "${QT_CMAKE_DIR}/Qt6Bundled${bundled_lib}/Qt6Bundled${bundled_lib}Config.cmake" ]; then
      mkdir -p "${QT_CMAKE_DIR}/Qt6Bundled${bundled_lib}"
      cat > "${QT_CMAKE_DIR}/Qt6Bundled${bundled_lib}/Qt6Bundled${bundled_lib}Config.cmake" << EOF
set(Qt6Bundled${bundled_lib}_FOUND TRUE)
if(NOT TARGET Qt6::Bundled${bundled_lib})
    add_library(Qt6::Bundled${bundled_lib} INTERFACE IMPORTED)
endif()
set(Qt6Bundled${bundled_lib}_VERSION "${QT_VERSION}")
EOF
      echo "Created Qt6Bundled${bundled_lib}Config.cmake"
    fi
  done


  echo "Running CMake configure..."

  cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$(cygpath -w $Qt6_PREFIX)" \
    -DQt6LinguistTools_DIR="${QT_LINGUIST_DIR}" \
    -DCMAKE_IGNORE_PATH="${MSYSTEM_PREFIX}/lib/cmake/Qt6" \
    ..

  if [ $? -ne 0 ]; then
    echo "CMake configuration failed"
    exit 1
  fi


echo "Compiling base build..."
ninja || exit 1

echo "Assembling per-game launchers from GameList..."
while IFS= read -r line || [[ -n "$line" ]]; do
  gameName=$(echo "$line" | tr -cd '[:alnum:]_-')
  gameDisplayName=$(echo "$line" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')

  # Bake this game's profile into the embedded launch.ini, then relink only.
  echo "Updating ${LAUNCH_INI_PATH} for MUDLET_PROFILES=${gameDisplayName}..."
  sed -i.bak "s/^MUDLET_PROFILES=.*/MUDLET_PROFILES=${gameDisplayName}/" "$LAUNCH_INI_PATH"

  echo "Relinking for ${gameName}..."
  ninja || exit 1

  # Stage the artifact where package-win.sh expects it (build-<game>/)
  OUT_DIR="${GITHUB_WORKSPACE}/build-${gameName}"
  rm -rf "${OUT_DIR}"
  mkdir -p "${OUT_DIR}"
  cp "${BUILD_DIR}/MudletInstaller.exe" "${OUT_DIR}/"
  if [ -f "${BUILD_DIR}/MudletInstaller.exe.debug" ]; then
    cp "${BUILD_DIR}/MudletInstaller.exe.debug" "${OUT_DIR}/"
  fi

  echo " ${gameName} ... done"
done < "${GITHUB_WORKSPACE}/GameList.txt"

cd ~ || exit 1
exit 0