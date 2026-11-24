import os
import re
import shutil
import stat
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
INSTALL_SH = REPO_ROOT / "install.sh"
RELEASE_PKGBUILD = REPO_ROOT / "packaging" / "arch" / "everlive" / "PKGBUILD"
RELEASE_SUMS = REPO_ROOT / "packaging" / "arch" / "everlive" / "SHA256SUMS"


def _extract_lsfg_vk_helper() -> str:
    text = INSTALL_SH.read_text(encoding="utf-8")
    m = re.search(
        r'(LSFG_VK_VERSION=.*?^_install_pinned_lsfg_vk\(\)\s*\{.*?^\})',
        text,
        re.MULTILINE | re.DOTALL,
    )
    if not m:
        raise AssertionError("Could not locate _install_pinned_lsfg_vk in install.sh")
    return m.group(1)


class TestInstallerSupplyChain(unittest.TestCase):
    def test_install_sh_pins_lsfg_vk_and_hardens_curl(self):
        text = INSTALL_SH.read_text(encoding="utf-8")
        self.assertNotIn("Latest release", text)
        self.assertRegex(text, r'LSFG_VK_VERSION="[^"]+"')
        self.assertRegex(text, r'LSFG_VK_SHA256="[0-9a-fA-F]{64}"')
        self.assertIn("curl --proto '=https' --tlsv1.2 --fail --location", text)
        self.assertIn("mktemp -d", text)

    def test_release_pkgbuild_has_real_checksum_matching_sha256sums(self):
        pkgbuild = RELEASE_PKGBUILD.read_text(encoding="utf-8")
        sums = RELEASE_SUMS.read_text(encoding="utf-8").strip()

        self.assertNotIn("sha256sums=('SKIP')", pkgbuild)
        self.assertIn(
            'source=("$pkgname-$pkgver.tar.gz::$url/archive/refs/tags/v$pkgver.tar.gz")',
            pkgbuild,
        )
        m_pkg = re.search(r"sha256sums=\('([0-9a-fA-F]{64})'\)", pkgbuild)
        self.assertIsNotNone(m_pkg, "Release PKGBUILD must pin a 64-hex sha256sum")

        m_ver = re.search(r"^pkgver=([^\s]+)", pkgbuild, re.MULTILINE)
        self.assertIsNotNone(m_ver)
        expected_line = f"{m_pkg.group(1)}  everlive-{m_ver.group(1)}.tar.gz"
        self.assertEqual(sums, expected_line)

    def _run_pinned_helper(self, archive_bytes: bytes, sha256_override: str, target_home: Path) -> subprocess.CompletedProcess:
        helper_src = _extract_lsfg_vk_helper()
        with tempfile.TemporaryDirectory() as td:
            td_path = Path(td)
            src_tar = td_path / "fixture.tar.xz"
            src_tar.write_bytes(archive_bytes)

            bin_dir = td_path / "bin"
            bin_dir.mkdir()
            mock_curl = bin_dir / "curl"
            mock_curl.write_text(
                f"#!/usr/bin/env bash\n"
                f"out=''\n"
                f"while [ $# -gt 0 ]; do\n"
                f"  if [ \"$1\" = '-o' ]; then out=\"$2\"; shift 2; else shift; fi\n"
                f"done\n"
                f"cp '{src_tar}' \"$out\"\n",
                encoding="utf-8",
            )
            mock_curl.chmod(0o755)

            script = (
                "#!/usr/bin/env bash\n"
                "set -euo pipefail\n"
                "GREEN='' BLUE='' YELLOW='' RED='' NC=''\n"
                "TARGET_USER=root\n"
                f"TARGET_HOME='{target_home}'\n"
                f"{helper_src}\n"
                f"LSFG_VK_SHA256='{sha256_override}'\n"
                "_install_pinned_lsfg_vk\n"
            )
            env = os.environ.copy()
            env["PATH"] = f"{bin_dir}:{env['PATH']}"
            return subprocess.run(
                ["bash", "-c", script],
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )

    def test_wrong_checksum_fails_closed_without_extracting(self):
        with tempfile.TemporaryDirectory() as td:
            td_path = Path(td)
            payload = td_path / "payload.txt"
            payload.write_text("ok", encoding="utf-8")
            tar_path = td_path / "good.tar.xz"
            with tarfile.open(tar_path, "w:xz") as tf:
                tf.add(payload, arcname="share/vulkan/test.txt")

            target_home = td_path / "home"
            target_home.mkdir()
            res = self._run_pinned_helper(tar_path.read_bytes(), "0" * 64, target_home)
            self.assertNotEqual(res.returncode, 0)
            self.assertIn("SHA-256-Prüfung", res.stdout + res.stderr)
            self.assertFalse((target_home / ".local").exists())

    def test_traversal_path_is_rejected_even_with_matching_checksum(self):
        import hashlib

        with tempfile.TemporaryDirectory() as td:
            td_path = Path(td)
            payload = td_path / "evil.txt"
            payload.write_text("evil", encoding="utf-8")
            tar_path = td_path / "evil.tar.xz"
            with tarfile.open(tar_path, "w:xz") as tf:
                tf.add(payload, arcname="../escaped.txt")

            data = tar_path.read_bytes()
            sha = hashlib.sha256(data).hexdigest()
            target_home = td_path / "home"
            target_home.mkdir()
            res = self._run_pinned_helper(data, sha, target_home)
            self.assertNotEqual(res.returncode, 0)
            self.assertIn("Ungültige Archivpfade", res.stdout + res.stderr)
            self.assertFalse((td_path / "escaped.txt").exists())


if __name__ == "__main__":
    unittest.main()
