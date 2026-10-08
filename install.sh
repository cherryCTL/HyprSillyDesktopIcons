#!/usr/bin/env bash
set -Eeuo pipefail

# ┌──────────────────────────────────────────────────────────────────────┐
# │ HyprSillyDesktopIcons · Installer                                     │
# │ Install / build / uninstall                                           │
# └──────────────────────────────────────────────────────────────────────┘

APP_NAME="HyprSillyDesktopIcons"
REPO_URL="https://github.com/cherryCTL/HyprSillyDesktopIcons.git"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
RELEASE_DIR="${SCRIPT_DIR}/release"
BUILD_DIR="${SCRIPT_DIR}/build"
INSTALL_DIR="${HOME}/.local/bin"
INSTALL_PATH="${INSTALL_DIR}/${APP_NAME}"

TEMP_DIR=""
BACKUP_PATH=""
SOURCE_DIR=""
BUILT_BINARY=""

if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
    C_RESET=$'\033[0m'
    C_BOLD=$'\033[1m'
    C_DIM=$'\033[2m'
    C_RED=$'\033[31m'
    C_GREEN=$'\033[32m'
    C_YELLOW=$'\033[33m'
    C_CYAN=$'\033[36m'
    C_BLUE=$'\033[34m'
    C_MAGENTA=$'\033[35m'
else
    C_RESET=''; C_BOLD=''; C_DIM=''; C_RED=''; C_GREEN=''
    C_YELLOW=''; C_CYAN=''; C_BLUE=''; C_MAGENTA=''
fi

cleanup() {
    local status=$?

    # Restore the previous executable if installation was cancelled or failed
    # after it had been backed up but before a replacement was installed.
    if [[ -n "${BACKUP_PATH}" && -f "${BACKUP_PATH}" ]]; then
        if [[ ! -e "${INSTALL_PATH}" ]]; then
            mkdir -p "${INSTALL_DIR}"
            cp -p -- "${BACKUP_PATH}" "${INSTALL_PATH}" || true
            printf '\n%s↶ Previous version restored.%s\n' "${C_YELLOW}" "${C_RESET}" >&2
        fi
        rm -f -- "${BACKUP_PATH}" || true
    fi

    if [[ -n "${TEMP_DIR}" && -d "${TEMP_DIR}" ]]; then
        rm -rf -- "${TEMP_DIR}" || true
    fi

    return "$status"
}
trap cleanup EXIT

on_error() {
    local code="$1" line="$2" command="$3"
    trap - ERR
    printf '\n%s╭────────────────────────────────────────────╮%s\n' "${C_RED}" "${C_RESET}" >&2
    printf '%s│  ERROR · Operation failed                 │%s\n' "${C_RED}" "${C_RESET}" >&2
    printf '%s╰────────────────────────────────────────────╯%s\n' "${C_RED}" "${C_RESET}" >&2
    printf '%sStep:%s line %s\n' "${C_BOLD}" "${C_RESET}" "$line" >&2
    printf '%sCommand:%s %s\n' "${C_BOLD}" "${C_RESET}" "$command" >&2
    printf '%sExit code:%s %s\n' "${C_BOLD}" "${C_RESET}" "$code" >&2
    printf '\n%sNothing else will be changed by the installer.%s\n' "${C_DIM}" "${C_RESET}" >&2
    exit "$code"
}
trap 'on_error "$?" "$LINENO" "$BASH_COMMAND"' ERR

line() {
    printf '%s────────────────────────────────────────────────────────────%s\n' "${C_DIM}" "${C_RESET}"
}

banner() {
    clear 2>/dev/null || true
    printf '\n'
    printf '%s%s╭────────────────────────────────────────────────────────────╮%s\n' "${C_CYAN}" "${C_BOLD}" "${C_RESET}"
    printf '%s%s│                  HyprSillyDesktopIcons                     │%s\n' "${C_CYAN}" "${C_BOLD}" "${C_RESET}"
    printf '%s%s│              INSTALLER  ·  BUILD  ·  REMOVE                │%s\n' "${C_CYAN}" "${C_BOLD}" "${C_RESET}"
    printf '%s%s╰────────────────────────────────────────────────────────────╯%s\n' "${C_CYAN}" "${C_BOLD}" "${C_RESET}"
    printf '\n'
}

step() {
    printf '\n%s◆ %s%s\n' "${C_MAGENTA}" "$1" "${C_RESET}"
    line
}

success() {
    printf '%s✓%s %s\n' "${C_GREEN}" "${C_RESET}" "$1"
}

info() {
    printf '%s•%s %s\n' "${C_CYAN}" "${C_RESET}" "$1"
}

warning() {
    printf '%s!%s %s\n' "${C_YELLOW}" "${C_RESET}" "$1"
}

ask_yes_no() {
    local prompt="$1" reply
    while true; do
        printf '\n%s?%s %s %s[y/N]%s ' "${C_YELLOW}" "${C_RESET}" "$prompt" "${C_DIM}" "${C_RESET}"
        if ! IFS= read -r reply; then
            printf '\n'
            return 1
        fi
        case "${reply,,}" in
            y|yes|bale|بله) return 0 ;;
            n|no|na|نه|'') return 1 ;;
            *) printf '%sPlease answer y or n.%s\n' "${C_DIM}" "${C_RESET}" ;;
        esac
    done
}

say_goodbye() {
    printf '\n%s✦ خداحافظ! هیچ تغییر دیگری انجام نمی‌دهم.%s\n\n' "${C_CYAN}" "${C_RESET}"
}

require_sudo() {
    if [[ "${EUID}" -eq 0 ]]; then
        SUDO=()
    else
        command -v sudo >/dev/null 2>&1 || {
            printf '%sThis operation needs sudo, but sudo was not found.%s\n' "${C_RED}" "${C_RESET}" >&2
            return 1
        }
        SUDO=(sudo)
    fi
}

get_cpu_count() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    elif command -v getconf >/dev/null 2>&1; then
        getconf _NPROCESSORS_ONLN
    else
        printf '2\n'
    fi
}

find_release_binary() {
    local candidate
    [[ -d "${RELEASE_DIR}" ]] || return 1

    # Expected release layout: ./release/HyprSillyDesktopIcons
    candidate="${RELEASE_DIR}/${APP_NAME}"
    if [[ -f "${candidate}" ]]; then
        printf '%s\n' "${candidate}"
        return 0
    fi

    # Also allow a small subfolder layout while avoiding random files/assets.
    candidate="$(find "${RELEASE_DIR}" -maxdepth 3 -type f -name "${APP_NAME}" -print -quit 2>/dev/null || true)"
    [[ -n "${candidate}" ]] || return 1
    printf '%s\n' "${candidate}"
}

find_built_binary() {
    local source_dir="$1" candidate
    local build_dir="${source_dir}/build"

    for candidate in \
        "${build_dir}/${APP_NAME}" \
        "${source_dir}/${APP_NAME}"; do
        if [[ -f "${candidate}" ]]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done

    if [[ -d "${build_dir}" ]]; then
        candidate="$(find "${build_dir}" -maxdepth 6 -type f -name "${APP_NAME}" -print -quit 2>/dev/null || true)"
        if [[ -n "${candidate}" ]]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    fi
    return 1
}

find_local_build_binary() {
    local candidate
    [[ -d "${BUILD_DIR}" ]] || return 1

    candidate="$(find_built_binary "${SCRIPT_DIR}" 2>/dev/null || true)"
    if [[ -n "${candidate}" && "${candidate}" == "${BUILD_DIR}"/* ]]; then
        printf '%s\n' "${candidate}"
        return 0
    fi

    candidate="$(find "${BUILD_DIR}" -maxdepth 6 -type f -name "${APP_NAME}" -print -quit 2>/dev/null || true)"
    [[ -n "${candidate}" ]] || return 1
    printf '%s\n' "${candidate}"
}

# Report a useful build/version identifier without executing an unknown binary.
# Priority: sidecar metadata, VERSION file, CMake cache/project declaration,
# then the source Git tag/commit. Older binaries may not embed a semantic version.
get_binary_version() {
    local binary="$1" source_dir="$2" artifact_dir candidate version project_block git_desc git_sha git_dirty
    local allow_git_fallback="false"

    case "${binary}" in
        "${source_dir}/build/"*|"${BUILD_DIR}/"*) allow_git_fallback="true" ;;
    esac

    artifact_dir="$(dirname -- "${binary}")"
    for candidate in \
        "${binary}.version" \
        "${artifact_dir}/${APP_NAME}.version" \
        "${artifact_dir}/VERSION" \
        "${artifact_dir}/version.txt" \
        "${source_dir}/VERSION"; do
        if [[ -f "${candidate}" ]]; then
            version="$(head -n 1 "${candidate}" | tr -d '\r' | xargs || true)"
            if [[ -n "${version}" ]]; then
                printf '%s\n' "${version}"
                return 0
            fi
        fi
    done

    for candidate in \
        "$(dirname -- "${binary}")/CMakeCache.txt" \
        "${source_dir}/build/CMakeCache.txt"; do
        if [[ -f "${candidate}" ]]; then
            version="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "${candidate}" | head -n 1)"
            if [[ -n "${version}" && "${version}" != "(NOTFOUND)" ]]; then
                printf '%s\n' "${version}"
                return 0
            fi
        fi
    done

    if [[ -f "${source_dir}/CMakeLists.txt" ]]; then
        project_block="$(awk 'BEGIN { IGNORECASE=1 } /^[[:space:]]*project[[:space:]]*\(/ { inside=1 } inside { print; if ($0 ~ /\)/) exit }' "${source_dir}/CMakeLists.txt")"
        version="$(printf '%s\n' "${project_block}" | grep -Eio 'VERSION[[:space:]]+[0-9]+([.][0-9]+){1,3}([-+][[:alnum:].-]+)?' | head -n 1 | awk '{print $2}')"
        if [[ -n "${version}" ]]; then
            printf '%s\n' "${version}"
            return 0
        fi
    fi

    # A Git revision is useful for a build-folder binary because it is built
    # from this source tree. Do not claim the current Git revision as the exact
    # version of an arbitrary prebuilt binary in ./release/.
    if [[ "${allow_git_fallback}" == "true" ]] &&
       git -C "${source_dir}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        git_dirty="$(git -C "${source_dir}" status --porcelain --untracked-files=no 2>/dev/null || true)"
        if [[ -z "${git_dirty}" ]]; then
            git_desc="$(git -C "${source_dir}" describe --tags --exact-match HEAD 2>/dev/null || true)"
            if [[ -n "${git_desc}" ]]; then
                printf '%s\n' "${git_desc}"
                return 0
            fi
        fi
        git_sha="$(git -C "${source_dir}" rev-parse --short HEAD 2>/dev/null || true)"
        if [[ -n "${git_sha}" ]]; then
            if [[ -n "${git_dirty}" ]]; then
                printf 'dev-%s-dirty\n' "${git_sha}"
            else
                printf 'dev-%s\n' "${git_sha}"
            fi
            return 0
        fi
    fi

    printf 'unknown\n'
}

show_binary_info() {
    local label="$1" binary="$2" source_dir="$3" version
    version="$(get_binary_version "${binary}" "${source_dir}")"
    success "Found in ${label}: ${binary#"${SCRIPT_DIR}/"}"
    printf '  %sVersion:%s %s\n' "${C_BOLD}" "${C_RESET}" "${version}"
    if [[ "${version}" == "unknown" ]]; then
        warning "No version metadata was found for this binary. Add a VERSION file or version sidecar for an exact release version."
    fi
}

resolve_source_dir() {
    if [[ -f "${SCRIPT_DIR}/CMakeLists.txt" ]]; then
        SOURCE_DIR="${SCRIPT_DIR}"
        return 0
    fi

    step "Source tree not found next to installer"
    info "Cloning the project into a temporary folder…"
    TEMP_DIR="$(mktemp -d)"
    git clone --depth 1 "${REPO_URL}" "${TEMP_DIR}/source"
    SOURCE_DIR="${TEMP_DIR}/source"
}

install_dependencies() {
    step "Detecting Linux distribution"
    [[ -r /etc/os-release ]] || {
        printf '%sCannot read /etc/os-release; unable to identify this system.%s\n' "${C_RED}" "${C_RESET}" >&2
        return 1
    }
    # shellcheck disable=SC1091
    source /etc/os-release
    local distro="${ID,,}" like="${ID_LIKE:-}"
    printf '%sDetected:%s %s\n' "${C_BOLD}" "${C_RESET}" "${PRETTY_NAME:-$distro}"

    require_sudo
    step "Installing build dependencies"

    case "${distro}" in
        arch|endeavouros|manjaro|garuda|arcolinux)
            "${SUDO[@]}" pacman -Sy --needed --noconfirm \
                base-devel cmake git pkgconf qt6-base qt6-wayland layer-shell-qt
            ;;
        fedora)
            "${SUDO[@]}" dnf install -y \
                gcc-c++ cmake git pkgconf-pkg-config qt6-qtbase-devel \
                qt6-qtwayland-devel wayland-devel layer-shell-qt-devel
            ;;
        debian|ubuntu|linuxmint|pop)
            "${SUDO[@]}" apt-get update
            "${SUDO[@]}" apt-get install -y \
                build-essential cmake git pkg-config extra-cmake-modules \
                qt6-base-dev qt6-wayland-dev qt6-declarative-dev \
                libwayland-dev libxkbcommon-dev wayland-protocols

            # Qt6 LayerShellQt development package name used by current Debian/Ubuntu releases.
            if apt-cache show liblayershellqtinterface-dev >/dev/null 2>&1; then
                "${SUDO[@]}" apt-get install -y liblayershellqtinterface-dev layer-shell-qt
            else
                warning "Your repositories do not expose liblayershellqtinterface-dev."
                warning "CMake may report LayerShellQt missing; use a distro release with Qt6 LayerShellQt development files."
            fi
            ;;
        *)
            if [[ "${like}" == *arch* ]]; then
                "${SUDO[@]}" pacman -Sy --needed --noconfirm \
                    base-devel cmake git pkgconf qt6-base qt6-wayland layer-shell-qt
            elif [[ "${like}" == *fedora* ]]; then
                "${SUDO[@]}" dnf install -y \
                    gcc-c++ cmake git pkgconf-pkg-config qt6-qtbase-devel \
                    qt6-qtwayland-devel wayland-devel layer-shell-qt-devel
            elif [[ "${like}" == *debian* || "${like}" == *ubuntu* ]]; then
                "${SUDO[@]}" apt-get update
                "${SUDO[@]}" apt-get install -y \
                    build-essential cmake git pkg-config extra-cmake-modules \
                    qt6-base-dev qt6-wayland-dev qt6-declarative-dev \
                    libwayland-dev libxkbcommon-dev wayland-protocols
                if apt-cache show liblayershellqtinterface-dev >/dev/null 2>&1; then
                    "${SUDO[@]}" apt-get install -y liblayershellqtinterface-dev layer-shell-qt
                else
                    warning "Qt6 LayerShellQt development package was not found in the configured repositories."
                fi
            else
                printf '%sUnsupported distribution: %s%s\n' "${C_RED}" "$distro" "${C_RESET}" >&2
                printf 'Supported package families: Arch, Debian/Ubuntu, Fedora.\n' >&2
                return 1
            fi
            ;;
    esac
    success "Build dependencies are installed."
}

build_project() {
    local source_dir="$1" build_dir="${1}/build" binary

    [[ -f "${source_dir}/CMakeLists.txt" ]] || {
        printf '%sCMakeLists.txt was not found in %s%s\n' "${C_RED}" "$source_dir" "${C_RESET}" >&2
        return 1
    }

    step "Configuring project"
    cmake -S "${source_dir}" -B "${build_dir}" -DCMAKE_BUILD_TYPE=Release

    step "Compiling ${APP_NAME}"
    cmake --build "${build_dir}" --parallel "$(get_cpu_count)"

    binary="$(find_built_binary "${source_dir}")" || {
        printf '%sBuild finished, but the executable %s could not be located.%s\n' "${C_RED}" "${APP_NAME}" "${C_RESET}" >&2
        return 1
    }
    [[ -f "${binary}" ]] || return 1
    BUILT_BINARY="${binary}"
}

install_binary() {
    local source_binary="$1" staged
    mkdir -p "${INSTALL_DIR}"
    staged="${INSTALL_DIR}/.${APP_NAME}.new.$$"

    step "Installing executable"
    install -m 0755 -- "${source_binary}" "${staged}"
    mv -f -- "${staged}" "${INSTALL_PATH}"

    if [[ -n "${BACKUP_PATH}" && -f "${BACKUP_PATH}" ]]; then
        rm -f -- "${BACKUP_PATH}"
        BACKUP_PATH=""
    fi

    success "Installed to ${INSTALL_PATH}"
    if [[ ":${PATH}:" != *":${INSTALL_DIR}:"* ]]; then
        warning "${INSTALL_DIR} is not in PATH for this shell."
        printf 'Add this to your shell profile if needed:\n  export PATH="$HOME/.local/bin:$PATH"\n'
    fi
}

prepare_previous_install() {
    [[ -e "${INSTALL_PATH}" ]] || return 0

    warning "An existing installation was found:"
    printf '  %s\n' "${INSTALL_PATH}"
    if ! ask_yes_no "ادامه‌ی نصب، نسخه‌ی قبلی را جایگزین می‌کند. ادامه بدهم؟"; then
        info "Installation cancelled; the current version was kept."
        return 1
    fi

    # Keep a temporary rollback copy so cancelling/failing later does not
    # accidentally leave the user without an executable.
    BACKUP_PATH="$(mktemp "${TMPDIR:-/tmp}/${APP_NAME}.backup.XXXXXX")"
    cp -p -- "${INSTALL_PATH}" "${BACKUP_PATH}"
    rm -f -- "${INSTALL_PATH}"
    success "Previous executable moved aside; it will be restored if installation is cancelled or fails."
}

do_install() {
    local release_binary build_binary source_dir choice

    banner
    step "Install / update"
    if ! prepare_previous_install; then
        return 0
    fi

    step "Scanning local binaries"
    release_binary="$(find_release_binary || true)"
    build_binary="$(find_local_build_binary || true)"
    source_dir="${SCRIPT_DIR}"

    if [[ -n "${release_binary}" ]]; then
        show_binary_info "./release/" "${release_binary}" "${source_dir}"
    else
        info "No executable was found in ./release/."
    fi

    if [[ -n "${build_binary}" ]]; then
        show_binary_info "./build/" "${build_binary}" "${source_dir}"
    else
        info "No executable was found in ./build/."
    fi

    if [[ -n "${release_binary}" && -n "${build_binary}" ]]; then
        printf '\n  %s1%s  Install the release binary\n' "${C_GREEN}" "${C_RESET}"
        printf '  %s2%s  Install the build-folder binary\n' "${C_CYAN}" "${C_RESET}"
        printf '  %s3%s  Build from source, then install\n' "${C_MAGENTA}" "${C_RESET}"
        printf '  %s0%s  Cancel\n\n' "${C_DIM}" "${C_RESET}"
        printf '%sChoose [0-3]:%s ' "${C_YELLOW}" "${C_RESET}"
        IFS= read -r choice || choice="0"
        case "${choice}" in
            1) install_binary "${release_binary}"; success "Installation complete."; return 0 ;;
            2) install_binary "${build_binary}"; success "Installation complete."; return 0 ;;
            3) ;;
            *) say_goodbye; return 0 ;;
        esac
    elif [[ -n "${release_binary}" || -n "${build_binary}" ]]; then
        local candidate label
        if [[ -n "${release_binary}" ]]; then
            candidate="${release_binary}"
            label="./release/"
        else
            candidate="${build_binary}"
            label="./build/"
        fi

        if ask_yes_no "باینری ${label} را مستقیم داخل ~/.local/bin نصب کنم؟"; then
            install_binary "${candidate}"
            success "Installation complete."
            return 0
        fi
        if ! ask_yes_no "پس سورس را کامپایل کنم؟"; then
            say_goodbye
            return 0
        fi
    else
        warning "No application binary was found in either ./release/ or ./build/."
        if ! ask_yes_no "وابستگی‌ها را نصب کنم و برنامه را از سورس build کنم؟"; then
            say_goodbye
            return 0
        fi
    fi

    install_dependencies
    resolve_source_dir
    build_project "${SOURCE_DIR}"
    install_binary "${BUILT_BINARY}"
    success "Build and installation completed successfully."
}

do_build_only() {
    local source_dir built_binary
    banner
    step "Build release binary"
    if ! ask_yes_no "وابستگی‌های لازم را نصب و نسخه‌ی Release را کامپایل کنم؟"; then
        say_goodbye
        exit 0
    fi

    install_dependencies
    resolve_source_dir
    build_project "${SOURCE_DIR}"
    mkdir -p "${RELEASE_DIR}"
    install -m 0755 -- "${BUILT_BINARY}" "${RELEASE_DIR}/${APP_NAME}"
    get_binary_version "${BUILT_BINARY}" "${SOURCE_DIR}" > "${RELEASE_DIR}/${APP_NAME}.version"
    success "Release binary created: ${RELEASE_DIR}/${APP_NAME}"
    printf '  Version: %s\n' "$(cat "${RELEASE_DIR}/${APP_NAME}.version")"
    info "You can now choose direct binary installation from the Install / Update option."
}

do_uninstall() {
    banner
    step "Uninstall"
    if [[ ! -e "${INSTALL_PATH}" ]]; then
        info "No executable is installed at ${INSTALL_PATH}"
        return 0
    fi

    warning "This removes only the executable, not your settings or icon positions."
    if ask_yes_no "برنامه را از ~/.local/bin حذف کنم؟"; then
        rm -f -- "${INSTALL_PATH}"
        success "Executable removed. Your configuration was kept."
    else
        info "Uninstall cancelled."
    fi
}

main_menu() {
    while true; do
        banner
        printf '  %s1%s  Install / Update\n' "${C_GREEN}" "${C_RESET}"
        printf '  %s2%s  Build release binary\n' "${C_CYAN}" "${C_RESET}"
        printf '  %s3%s  Uninstall\n' "${C_RED}" "${C_RESET}"
        printf '  %s0%s  Exit\n' "${C_DIM}" "${C_RESET}"
        printf '\n'
        printf '%sInstall path:%s %s\n' "${C_BOLD}" "${C_RESET}" "${INSTALL_PATH}"
        printf '%sRelease folder:%s %s\n' "${C_BOLD}" "${C_RESET}" "${RELEASE_DIR}"
        printf '%sBuild folder:%s   %s\n' "${C_BOLD}" "${C_RESET}" "${BUILD_DIR}"
        printf '\n%sChoose an action [0-3]:%s ' "${C_YELLOW}" "${C_RESET}"

        local choice
        IFS= read -r choice || choice="0"
        case "${choice}" in
            1) do_install; printf '\n'; read -r -p 'Press Enter to return to menu…' _ || true ;;
            2) do_build_only; printf '\n'; read -r -p 'Press Enter to return to menu…' _ || true ;;
            3) do_uninstall; printf '\n'; read -r -p 'Press Enter to return to menu…' _ || true ;;
            0) say_goodbye; exit 0 ;;
            *) warning "Please choose 0, 1, 2 or 3."; sleep 1 ;;
        esac
    done
}

main_menu "$@"
