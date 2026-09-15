#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
prefix="${repo_root}/.vulkan-toolchain"
packages="${prefix}/packages"
extract="${prefix}/root"
mkdir -p "${packages}" "${extract}"

host_arch="$(dpkg --print-architecture)"
[[ "${host_arch}" == arm64 ]] || { echo "Vulkan manifest requires arm64, host is ${host_arch}" >&2; exit 1; }
manifest="${repo_root}/scripts/vulkan-toolchain-manifest.txt"
while read -r name version arch hash; do
  [[ -z "${name}" || "${name}" == \#* ]] && continue
  [[ "${arch}" == arm64 ]] || { echo "unsupported manifest architecture: ${arch}" >&2; exit 1; }
  deb="${packages}/${name}_${version}_${arch}.deb"
  if [[ ! -f "${deb}" ]]; then
    tmp="$(mktemp -d "${packages}/.download.XXXXXX")"
    trap 'rm -rf "${tmp}"' EXIT
    (cd "${tmp}" && apt-get download "${name}:arm64=${version}")
    downloaded="$(find "${tmp}" -maxdepth 1 -type f -name '*.deb' -print -quit)"
    [[ -n "${downloaded}" ]] || { echo "download produced no ${name} package" >&2; exit 1; }
    mv "${downloaded}" "${deb}"
    rm -rf "${tmp}"; trap - EXIT
  fi
  actual="$(sha256sum "${deb}" | awk '{print $1}')"
  [[ "${actual}" == "${hash}" ]] || { echo "SHA256 mismatch: ${deb}" >&2; exit 1; }
  [[ "$(dpkg-deb -f "${deb}" Package)" == "${name}" &&
     "$(dpkg-deb -f "${deb}" Version)" == "${version}" &&
     "$(dpkg-deb -f "${deb}" Architecture)" == "${arch}" ]] || {
       echo "package metadata mismatch: ${deb}" >&2; exit 1; }
  dpkg-deb -x "${deb}" "${extract}"
done < "${manifest}"

echo "Vulkan toolchain ready: ${prefix}"
echo "VULKAN_SDK=${prefix}/root/usr"
echo "PATH=${prefix}/root/usr/bin:\$PATH"
echo "Vulkan_INCLUDE_DIR=${prefix}/root/usr/include"
echo "Vulkan_LIBRARY=/lib/aarch64-linux-gnu/libvulkan.so.1"
