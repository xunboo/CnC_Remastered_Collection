"""Build the portable Windows bridge; runtime users need no Python or shell.

Build-time dependencies stay in build/llm-packager, never in system Python.
Only the three bridge modules and Python runtime are bundled, never llm.ini.
"""
from pathlib import Path
import argparse
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PYINSTALLER_VERSION = "6.22.3"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install-dependencies", action="store_true", help="Install the pinned packager into the local build directory.")
    parser.add_argument("--output", type=Path, default=ROOT / "build/redalert")
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("build the Windows executable on Windows")
    dependencies = ROOT / "build/llm-packager"
    work = ROOT / "build/llm-package"
    for path in (dependencies, work, args.output):
        path.mkdir(parents=True, exist_ok=True)
    if args.install_dependencies:
        subprocess.run([sys.executable, "-m", "pip", "install", "--disable-pip-version-check", "--no-cache-dir",
                        "--target", str(dependencies), "--upgrade", "pyinstaller==" + PYINSTALLER_VERSION], check=True)
    environment = dict(os.environ)
    environment["PYTHONPATH"] = str(dependencies)
    environment["PYINSTALLER_CONFIG_DIR"] = str(work / "cache")
    subprocess.run([sys.executable, "-m", "PyInstaller", "--noconfirm", "--onefile", "--console", "--noupx",
                    "--name", "LLMBridge", "--distpath", str(args.output.resolve()),
                    "--workpath", str(work / "work"), "--specpath", str(work),
                    "--paths", str(ROOT / "SCRIPTS"), "--log-level", "WARN",
                    str(ROOT / "SCRIPTS/llm_bridge.py")], cwd=ROOT, env=environment, check=True)
    executable = args.output.resolve() / "LLMBridge.exe"
    subprocess.run([str(executable), "--show-schema"], check=True, stdout=subprocess.DEVNULL)
    print("Built and smoke-tested " + str(executable))


if __name__ == "__main__":
    main()
