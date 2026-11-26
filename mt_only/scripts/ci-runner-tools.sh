#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Source from ci-qualify.sh. All downloaded packages and tools stay in the job directory.
install_runner_tools() {
  local group=$1 work=$2 arch
  arch=$(gcc -dumpmachine)
  local pkg="$work/packages" prefix="$work/tools"
  mkdir -p "$pkg"/{lists/partial,archives/partial,empty} "$prefix"
  # APT reads this file before the image-wide Docker hooks. Its lists, archives
  # and status snapshot belong exclusively to this job. No dpkg transaction runs.
  printf 'Dir::Etc::parts "%s";\nDir::Etc::main "/dev/null";\n' "$pkg/empty" > "$pkg/apt.conf"
  cp /var/lib/dpkg/status "$pkg/status"
  local -a apt=(env "APT_CONFIG=$pkg/apt.conf" apt-get -qq -o Debug::NoLocking=1
    -o "Dir::State::status=$pkg/status" -o "Dir::State::Lists=$pkg/lists"
    -o "Dir::Cache::Archives=$pkg/archives" -o "Dir::Etc::parts=$pkg/empty"
    -o "Dir::Cache::pkgcache=" -o "Dir::Cache::srcpkgcache="
    -o "APT::Sandbox::User=$(id -un)")
  local -a packages
  if [[ $group == native ]]; then
    packages=(clang libclang-rt-18-dev libssl-dev)
  else
    packages=(device-tree-compiler gperf file wget xz-utils)
  fi
  "${apt[@]}" update
  "${apt[@]}" -y --no-install-recommends --download-only install "${packages[@]}"
  local deb
  for deb in "$pkg/archives"/*.deb; do
    [[ -f $deb ]] || continue
    dpkg-deb -x "$deb" "$prefix"
  done
  export PATH="$prefix/usr/bin:$PATH"
  export LD_LIBRARY_PATH="$prefix/usr/lib/$arch${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  if [[ $group == native ]]; then
    export CPLUS_INCLUDE_PATH="$prefix/usr/include:$prefix/usr/include/$arch${CPLUS_INCLUDE_PATH:+:$CPLUS_INCLUDE_PATH}"
    export C_INCLUDE_PATH="$prefix/usr/include:$prefix/usr/include/$arch${C_INCLUDE_PATH:+:$C_INCLUDE_PATH}"
    export LIBRARY_PATH="$prefix/usr/lib/$arch${LIBRARY_PATH:+:$LIBRARY_PATH}"
  fi
  python3 -m venv "$work/venv"
  source "$work/venv/bin/activate"
}
