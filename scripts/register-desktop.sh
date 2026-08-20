#!/usr/bin/env bash
# Register (or unregister) PCM Transport for the current user so file managers
# can open supported audio/playlist/CUE files and directories with it.
#
# Usage:
#   scripts/register-desktop.sh                 # install into ~/.local
#   scripts/register-desktop.sh --prefix DIR
#   scripts/register-desktop.sh --binary PATH   # use this pcm_transport binary
#   scripts/register-desktop.sh --set-defaults  # also claim default MIME handlers
#   scripts/register-desktop.sh --unregister
#
# Requires a built binary (default: <repo>/build/pcm_transport).
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)

PREFIX="${HOME}/.local"
BINARY="${REPO_ROOT}/build/pcm_transport"
DESKTOP_ID="org.berestov.pcmtransport"
DESKTOP_SRC="${REPO_ROOT}/data/${DESKTOP_ID}.desktop"
ICON_NAME="${DESKTOP_ID}"
SET_DEFAULTS=0
UNREGISTER=0

usage() {
  sed -n '2,14p' "$0" | sed 's/^# \?//'
  exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix)
      PREFIX=$(cd "$2" && pwd)
      shift 2
      ;;
    --binary)
      BINARY=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
      shift 2
      ;;
    --set-defaults)
      SET_DEFAULTS=1
      shift
      ;;
    --unregister)
      UNREGISTER=1
      shift
      ;;
    -h|--help)
      usage 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage 1
      ;;
  esac
done

BIN_DIR="${PREFIX}/bin"
APP_DIR="${PREFIX}/share/applications"
USER_APP_DIR="${HOME}/.local/share/applications"
ICON_ROOT="${PREFIX}/share/icons/hicolor"
DESKTOP_DST="${APP_DIR}/${DESKTOP_ID}.desktop"
INSTALLED_BIN="${BIN_DIR}/pcm_transport"
MIMEAPPS_LIST="${HOME}/.config/mimeapps.list"

# MIME types from data/org.berestov.pcmtransport.desktop (kept in sync manually).
MIME_TYPES=(
  audio/flac audio/x-flac audio/mpeg audio/mp2 audio/mp4 audio/x-m4a audio/x-m4r
  audio/aac audio/x-aac audio/vnd.wave audio/wav audio/x-wav audio/x-w64
  audio/x-aiff audio/aiff audio/basic audio/x-caf audio/x-voc
  audio/vnd.rn-realaudio audio/x-pn-realaudio audio/x-ape audio/x-wavpack
  audio/x-tak audio/x-tta audio/x-dsf audio/x-dff audio/ac3 audio/vnd.dts
  audio/ogg application/ogg audio/x-vorbis+ogg audio/x-flac+ogg audio/x-opus+ogg
  audio/opus audio/x-speex+ogg audio/x-speex audio/x-ms-wma application/vnd.ms-asf
  video/x-ms-wmv audio/x-oma audio/x-musepack application/x-cue audio/x-mpegurl
  application/vnd.apple.mpegurl inode/directory
)

refresh_caches() {
  if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "${APP_DIR}" 2>/dev/null || true
    if [[ "${USER_APP_DIR}" != "${APP_DIR}" && -d "${USER_APP_DIR}" ]]; then
      update-desktop-database "${USER_APP_DIR}" 2>/dev/null || true
    fi
  fi
  if command -v gtk-update-icon-cache >/dev/null 2>&1 && [[ -d "${ICON_ROOT}" ]]; then
    gtk-update-icon-cache -f -t "${ICON_ROOT}" 2>/dev/null || true
  fi
  if command -v xdg-desktop-menu >/dev/null 2>&1 && [[ -f "${DESKTOP_DST}" ]]; then
    xdg-desktop-menu forceupdate 2>/dev/null || true
  fi
}

# File managers create userapp-pcm_transport-XXXX.desktop stubs (often with an
# empty Name=) when opening a file via “Open With → Other Application”. Those
# stubs show up as blank rows in Open With menus — remove them.
cleanup_userapp_stubs() {
  local stub
  local removed=0
  shopt -s nullglob
  for stub in "${USER_APP_DIR}"/userapp-pcm_transport-*.desktop \
              "${USER_APP_DIR}"/userapp-*pcm_transport*.desktop; do
    [[ -f "${stub}" ]] || continue
    rm -f "${stub}"
    removed=$((removed + 1))
  done
  shopt -u nullglob

  if [[ -f "${MIMEAPPS_LIST}" ]]; then
    local tmp
    tmp=$(mktemp)
    # Drop any leftover associations pointing at the removed stubs.
    sed -E 's/userapp-pcm_transport-[^.;]+\.desktop;?//g; s/;;+/;/g; s/=;/=/g; s/;$//' \
      "${MIMEAPPS_LIST}" > "${tmp}"
    if ! cmp -s "${MIMEAPPS_LIST}" "${tmp}"; then
      mv "${tmp}" "${MIMEAPPS_LIST}"
    else
      rm -f "${tmp}"
    fi
  fi

  if [[ "${removed}" -gt 0 ]]; then
    echo "Removed ${removed} empty file-manager stub(s) (userapp-pcm_transport-*)."
  fi
}

unregister() {
  cleanup_userapp_stubs
  rm -f "${INSTALLED_BIN}"
  rm -f "${DESKTOP_DST}"
  for size in 16 32 48 128 256; do
    rm -f "${ICON_ROOT}/${size}x${size}/apps/${ICON_NAME}.png"
  done
  refresh_caches
  echo "Unregistered PCM Transport from ${PREFIX}"
  echo "Note: default MIME associations may still point here until changed in the file manager."
}

register() {
  if [[ ! -x "${BINARY}" ]]; then
    echo "Binary not found or not executable: ${BINARY}" >&2
    echo "Build first: cmake --build build --target pcm_transport" >&2
    exit 1
  fi
  if [[ ! -f "${DESKTOP_SRC}" ]]; then
    echo "Desktop entry not found: ${DESKTOP_SRC}" >&2
    exit 1
  fi

  cleanup_userapp_stubs

  mkdir -p "${BIN_DIR}" "${APP_DIR}"
  install -m 755 "${BINARY}" "${INSTALLED_BIN}"

  for size in 16 32 48 128 256; do
    src="${REPO_ROOT}/data/icons/hicolor/${size}x${size}/apps/${ICON_NAME}.png"
    if [[ -f "${src}" ]]; then
      mkdir -p "${ICON_ROOT}/${size}x${size}/apps"
      install -m 644 "${src}" "${ICON_ROOT}/${size}x${size}/apps/${ICON_NAME}.png"
    fi
  done

  # Absolute Exec/TryExec so PATH does not need to include the prefix.
  awk -v bin="${INSTALLED_BIN}" '
    BEGIN { updated_exec = 0; updated_try = 0 }
    /^Exec=/ { print "Exec=" bin " %F"; updated_exec = 1; next }
    /^TryExec=/ { print "TryExec=" bin; updated_try = 1; next }
    { print }
    END {
      if (!updated_exec) print "Exec=" bin " %F"
      if (!updated_try) print "TryExec=" bin
    }
  ' "${DESKTOP_SRC}" > "${DESKTOP_DST}"
  chmod 644 "${DESKTOP_DST}"

  refresh_caches

  if [[ "${SET_DEFAULTS}" -eq 1 ]]; then
    if ! command -v xdg-mime >/dev/null 2>&1; then
      echo "xdg-mime not found; skipped --set-defaults" >&2
    else
      for mime in "${MIME_TYPES[@]}"; do
        xdg-mime default "${DESKTOP_ID}.desktop" "${mime}" || true
      done
      echo "Set PCM Transport as default handler for listed MIME types."
    fi
  fi

  echo "Registered PCM Transport:"
  echo "  binary : ${INSTALLED_BIN}"
  echo "  desktop: ${DESKTOP_DST}"
  echo "  icons  : ${ICON_ROOT}/*/apps/${ICON_NAME}.png"
  echo
  echo "Open audio files, playlists, CUE sheets, or directories via “Open With” / file manager."
  echo "Prefer the named “PCM Transport” entry — avoid “Other Application” (it creates blank stubs)."
  if [[ "${SET_DEFAULTS}" -eq 0 ]]; then
    echo "To also claim defaults: $0 --set-defaults"
  fi
  echo "If menus do not update, log out/in or restart the file manager."
}

if [[ "${UNREGISTER}" -eq 1 ]]; then
  unregister
else
  register
fi
