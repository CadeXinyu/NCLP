#!/usr/bin/env bash
set -euo pipefail

PLUGIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NCLP_DIR="$(cd "${PLUGIN_DIR}/../.." && pwd)"
CLE_DIR="$(cd "${NCLP_DIR}/.." && pwd)"
PLUGIN_GUI_DIR=""
BUILD_ROOT="${NCLP_DIR}/open_ephys_project"
BUILD_TYPE="Release"
INSTALL=1
EXPLICIT_PLUGIN_OUTPUT=""
INSTALL_USER=1
INSTALL_SOURCE_GUI=1
CHECK_HOST=1
CONFIGURE_HOST=0
HOST_DRY_RUN=0
HOST_YES=0
HOST_IFACE="eno1"

usage() {
  cat <<USAGE
Usage: $0 [options]

Build and install the NCLP Intan Source Open Ephys plugin.

All generated CMake/build files stay under:
  NCLP/open_ephys_project/<BuildType>/

Options:
  --plugin-gui-dir DIR  Path to Open Ephys plugin-GUI checkout
  --plugin-dir DIR      Copy the compiled plugin to this Open Ephys plugin directory
  --build-root DIR      Generated CMake build directory (default: NCLP/open_ephys_project)
  --build-type TYPE     Debug or Release (default: Release)
  --local-only          Build only into NCLP/open_ephys_project; do not copy to Open Ephys
  --skip-install        Same as --local-only
  --no-user-install     Do not copy to ~/.config/open-ephys/plugins-api10
  --no-source-install   Do not copy to plugin-GUI/Build/<BuildType>/plugins
  --no-check-host       Skip host-network preflight reporting
  --check-host          Force host-network preflight reporting
  --configure-host      Run setup_nclp_host.sh after the preflight check
  --iface IFACE         Interface to pass to setup_nclp_host.sh (default: eno1)
  --host-dry-run        Preview host-network commands when --configure-host is used
  --host-yes            Do not prompt in setup_nclp_host.sh when configuring host networking
  --help                Show this help
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --plugin-gui-dir) PLUGIN_GUI_DIR="$2"; shift 2 ;;
    --plugin-dir) EXPLICIT_PLUGIN_OUTPUT="$(mkdir -p "$2" && cd "$2" && pwd)"; shift 2 ;;
    --build-root) BUILD_ROOT="$(mkdir -p "$2" && cd "$2" && pwd)"; shift 2 ;;
    --build-type) BUILD_TYPE="$2"; shift 2 ;;
    --local-only|--skip-install) INSTALL=0; shift ;;
    --no-user-install) INSTALL_USER=0; shift ;;
    --no-source-install) INSTALL_SOURCE_GUI=0; shift ;;
    --no-check-host) CHECK_HOST=0; shift ;;
    --check-host) CHECK_HOST=1; shift ;;
    --configure-host) CONFIGURE_HOST=1; shift ;;
    --iface) HOST_IFACE="$2"; shift 2 ;;
    --host-dry-run) HOST_DRY_RUN=1; shift ;;
    --host-yes) HOST_YES=1; shift ;;
    --help|-h) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage; exit 2 ;;
  esac
done

HOST_SETUP_SCRIPT="${PLUGIN_DIR}/scripts/setup_nclp_host.sh"

if [[ -z "${PLUGIN_GUI_DIR}" ]]; then
  SIBLING_PLUGIN_GUI_DIR="${CLE_DIR}/plugin-GUI"
  LOCAL_PLUGIN_GUI_DIR="${NCLP_DIR}/open_ephys_project/plugin-GUI"
  if [[ -f "${SIBLING_PLUGIN_GUI_DIR}/Plugins/PluginRules.cmake" ]]; then
    PLUGIN_GUI_DIR="${SIBLING_PLUGIN_GUI_DIR}"
  elif [[ -f "${LOCAL_PLUGIN_GUI_DIR}/Plugins/PluginRules.cmake" ]]; then
    PLUGIN_GUI_DIR="${LOCAL_PLUGIN_GUI_DIR}"
    echo "Using workspace-local Open Ephys SDK: ${PLUGIN_GUI_DIR}"
  else
    PLUGIN_GUI_DIR="${SIBLING_PLUGIN_GUI_DIR}"
  fi
fi

if [[ -d "${PLUGIN_GUI_DIR}" ]]; then
  PLUGIN_GUI_DIR="$(cd "${PLUGIN_GUI_DIR}" && pwd)"
fi

if [[ ! -f "${PLUGIN_GUI_DIR}/Plugins/PluginRules.cmake" ]]; then
  cat >&2 <<ERR
Could not find a source Open Ephys plugin-GUI checkout at:
  ${PLUGIN_GUI_DIR}

Clone it first, for example:
  git clone --depth 1 --branch v1.1.0 https://github.com/open-ephys/plugin-GUI.git ${CLE_DIR}/plugin-GUI

The ignored workspace fallback is also supported:
  git clone --depth 1 --branch v1.1.0 https://github.com/open-ephys/plugin-GUI.git ${NCLP_DIR}/open_ephys_project/plugin-GUI

Or pass --plugin-gui-dir /path/to/plugin-GUI
ERR
  exit 1
fi

# CMake generates JuceHeader.h directly from the template.  Do not configure the
# full GUI here; that would pull in GUI-only dependencies such as libcurl-dev.
if [[ ! -f "${PLUGIN_GUI_DIR}/JuceLibraryCode/JuceHeader.h.in" || ! -f "${PLUGIN_GUI_DIR}/JuceLibraryCode/BinaryData.h" ]]; then
  cat >&2 <<ERR
Missing Open Ephys JUCE source headers in:
  ${PLUGIN_GUI_DIR}/JuceLibraryCode

Use the exact Open Ephys GUI v1.1.0 source checkout.
ERR
  exit 1
fi

check_link_deps() {
  if [[ "$(uname -s)" != "Linux" ]]; then
    return
  fi

  local missing=()
  local runtime_fallback=()
  local libdir
  libdir="$(gcc -print-multiarch 2>/dev/null || true)"
  local search_dirs=("/lib" "/usr/lib" "/usr/local/lib")
  if [[ -n "${libdir}" ]]; then
    search_dirs+=("/lib/${libdir}" "/usr/lib/${libdir}" "/usr/local/lib/${libdir}")
  fi

  local libs=("libXinerama.so" "libasound.so" "libfreetype.so")
  local runtime_libs=("libXinerama.so.1" "libasound.so.2" "libfreetype.so.6")
  local packages=("libxinerama-dev" "libasound2-dev" "libfreetype6-dev")

  for i in "${!libs[@]}"; do
    local found_dev=0
    local found_runtime=0
    for dir in "${search_dirs[@]}"; do
      if [[ -e "${dir}/${libs[$i]}" ]]; then
        found_dev=1
        break
      fi
      if [[ -e "${dir}/${runtime_libs[$i]}" ]]; then
        found_runtime=1
      fi
    done
    if [[ "${found_dev}" -eq 0 && "${found_runtime}" -eq 1 ]]; then
      runtime_fallback+=("${runtime_libs[$i]}")
    elif [[ "${found_dev}" -eq 0 ]]; then
      missing+=("${packages[$i]}")
    fi
  done

  if [[ "${#missing[@]}" -gt 0 ]]; then
    cat >&2 <<ERR
Missing Open Ephys Linux development libraries needed for plugin linking:
  ${missing[*]}

Install them with:
  sudo apt-get update
  sudo apt-get install ${missing[*]}

If you later build the full Open Ephys GUI, also install:
  sudo apt-get install libcurl4-openssl-dev
ERR
    exit 1
  fi

  if [[ "${#runtime_fallback[@]}" -gt 0 ]]; then
    echo "Using build-local linker aliases for installed runtime libraries: ${runtime_fallback[*]}"
  fi
}

copy_plugin() {
  local src="$1"
  local dst_dir="$2"
  local staged
  mkdir -p "${dst_dir}"
  # Replace the directory entry without modifying a library already loaded by a GUI.
  staged="$(mktemp "${dst_dir}/.NCLPIntanSource.XXXXXX")"
  if ! cp -p "${src}" "${staged}" ||
     ! mv -f "${staged}" "${dst_dir}/$(basename "${src}")"; then
    rm -f "${staged}"
    return 1
  fi
  echo "Installed plugin copy: ${dst_dir}/$(basename "${src}")"
}

run_host_preflight() {
  if [[ "${CHECK_HOST}" -eq 0 ]]; then
    return
  fi

  echo "== Host network preflight =="
  local args=(--check)
  if [[ -n "${HOST_IFACE}" ]]; then
    args+=(--iface "${HOST_IFACE}")
  fi

  if "${HOST_SETUP_SCRIPT}" "${args[@]}"; then
    echo "Host network preflight: OK"
  else
    echo "Host network preflight: needs attention" >&2
    echo "Use --configure-host to apply the recommended NCLP IP/firewall setup." >&2
  fi
}

configure_host_if_requested() {
  if [[ "${CONFIGURE_HOST}" -eq 0 ]]; then
    return
  fi

  local args=()
  if [[ -n "${HOST_IFACE}" ]]; then
    args+=(--iface "${HOST_IFACE}")
  fi
  if [[ "${HOST_DRY_RUN}" -eq 1 ]]; then
    args+=(--dry-run)
  fi
  if [[ "${HOST_YES}" -eq 1 ]]; then
    args+=(--yes)
  fi

  echo "== Host network configuration =="
  if [[ "${HOST_DRY_RUN}" -eq 1 || "${EUID}" -eq 0 ]]; then
    "${HOST_SETUP_SCRIPT}" "${args[@]}"
  else
    sudo "${HOST_SETUP_SCRIPT}" "${args[@]}"
  fi
}

check_link_deps
run_host_preflight
configure_host_if_requested

BUILD_DIR="${BUILD_ROOT}/${BUILD_TYPE}"
LOCAL_PLUGIN_DIR="${BUILD_DIR}/plugins"
mkdir -p "${BUILD_DIR}" "${LOCAL_PLUGIN_DIR}"
cd "${BUILD_DIR}"

cmake -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DPLUGIN_GUI_DIR="${PLUGIN_GUI_DIR}" \
  -DBIN_PLUGIN_DIR="${LOCAL_PLUGIN_DIR}" \
  "${PLUGIN_DIR}"

cmake --build . --parallel "${NCLP_BUILD_JOBS:-4}"

PLUGIN_SO="${LOCAL_PLUGIN_DIR}/NCLPIntanSource.so"
if [[ ! -f "${PLUGIN_SO}" ]]; then
  PLUGIN_SO="$(find "${LOCAL_PLUGIN_DIR}" -maxdepth 2 -type f \
    -name '*NCLPIntanSource*.so' -print -quit)"
fi

if [[ -z "${PLUGIN_SO}" || ! -f "${PLUGIN_SO}" ]]; then
  echo "Build finished, but could not find NCLPIntanSource .so under ${LOCAL_PLUGIN_DIR}" >&2
  exit 1
fi

if [[ "${INSTALL}" -eq 1 ]]; then
  if [[ -n "${EXPLICIT_PLUGIN_OUTPUT}" ]]; then
    copy_plugin "${PLUGIN_SO}" "${EXPLICIT_PLUGIN_OUTPUT}"
  else
    if [[ "${INSTALL_SOURCE_GUI}" -eq 1 ]]; then
      copy_plugin "${PLUGIN_SO}" "${PLUGIN_GUI_DIR}/Build/${BUILD_TYPE}/plugins"
    fi

    # Modern packaged Linux Open Ephys also searches the per-user plugin API dir.
    # PluginManager.cpp uses File::userApplicationDataDirectory/open-ephys/plugins-api10.
    if [[ "${INSTALL_USER}" -eq 1 && "$(uname -s)" == "Linux" ]]; then
      copy_plugin "${PLUGIN_SO}" "${HOME}/.config/open-ephys/plugins-api10"
    fi
  fi
fi

cat <<DONE

Build complete.
Generated build directory:
  ${BUILD_DIR}
Local plugin output:
  ${PLUGIN_SO}

Open Ephys loads plugins when the GUI starts. Restart Open Ephys, then look for:
  NCLP Intan Source

If it does not appear, rerun with the exact plugin folder shown by your Open Ephys install:
  $0 --plugin-dir /path/to/open-ephys/plugins
DONE
