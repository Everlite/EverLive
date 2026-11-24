#!/usr/bin/env bash
# Generate SHA256SUMS and update sha256sums in packaging/arch/everlive/PKGBUILD
# for the tagged release currently set in PKGBUILD.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PKGBUILD="${DIR}/PKGBUILD"
SUMS_FILE="${DIR}/SHA256SUMS"

pkgver="$(sed -n 's/^pkgver=//p' "${PKGBUILD}" | head -n 1)"
url="$(sed -n 's/^url="\([^"]*\)"/\1/p' "${PKGBUILD}" | head -n 1)"
if [ -z "${pkgver}" ] || [ -z "${url}" ]; then
    echo "error: could not read pkgver or url from ${PKGBUILD}" >&2
    exit 1
fi

tar_name="everlive-${pkgver}.tar.gz"
tar_url="${url}/archive/refs/tags/v${pkgver}.tar.gz"

tmp_dir="$(mktemp -d)"
chmod 700 "${tmp_dir}"
trap 'rm -rf -- "${tmp_dir}"' EXIT

curl --proto '=https' --tlsv1.2 --fail --location --silent --show-error \
    --max-time 60 "${tar_url}" -o "${tmp_dir}/${tar_name}"

sha="$(sha256sum "${tmp_dir}/${tar_name}" | awk '{print $1}')"
printf '%s  %s\n' "${sha}" "${tar_name}" > "${SUMS_FILE}"

sed -i "s/^sha256sums=(.*)/sha256sums=('${sha}')/" "${PKGBUILD}"
printf 'Updated %s and %s for v%s (%s)\n' "${SUMS_FILE}" "${PKGBUILD}" "${pkgver}" "${sha}"
