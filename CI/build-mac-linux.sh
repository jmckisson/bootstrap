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

RUNNER_WORKSPACE_UNIX_PATH=$(echo "${RUNNER_WORKSPACE}" | sed 's|\\|/|g' | sed 's|D:|/d|g')
export CCACHE_DIR=${RUNNER_WORKSPACE_UNIX_PATH}/ccache

echo "CCACHE_DIR is: ${CCACHE_DIR}"
echo "PATH is now:"
echo "${PATH}"
echo ""

cd $GITHUB_WORKSPACE || exit 1

LAUNCH_INI_PATH="${GITHUB_WORKSPACE}/resources/launch.ini"
echo "CMAKE_PREFIX_PATH is: ${CMAKE_PREFIX_PATH}"

# Configure and compile ONCE. Every game produces an identical binary apart from
# the embedded launch.ini, so we build a single tree and only relink per game.
# Changing launch.ini triggers just an rcc regen + relink
BUILD_DIR="${GITHUB_WORKSPACE}/build"
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}" || exit 1

echo "Running CMake configure..."
echo "cmake -G Ninja -DCMAKE_BUILD_TYPE=Release .."
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release .. || exit 1

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

  # Stage the artifact where the packaging script expects it (build-<game>/)
  OUT_DIR="${GITHUB_WORKSPACE}/build-${gameName}"
  rm -rf "${OUT_DIR}"
  mkdir -p "${OUT_DIR}"
  if [ -d "${BUILD_DIR}/MudletInstaller.app" ]; then
    cp -R "${BUILD_DIR}/MudletInstaller.app" "${OUT_DIR}/"
  else
    cp "${BUILD_DIR}/MudletInstaller" "${OUT_DIR}/"
  fi

  echo " ${gameName} ... done"
done < "${GITHUB_WORKSPACE}/GameList.txt"

cd ~ || exit 1
exit 0