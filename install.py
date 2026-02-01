#!/usr/bin/env python3
"""
OUROBOROS installer

Builds and installs OUROBOROS (terminal music player).

Usage:
    ./install.py              # Build and install (default)
    ./install.py --debug      # Debug build
    ./install.py --debug-log  # Debug build with logging enabled
    ./install.py --prefix /usr # Install to /usr instead of /usr/local
    ./install.py build        # Build only, don't install
    ./install.py clean        # Clean build directory
    ./install.py uninstall    # Remove installed files
"""

import argparse
import os
import sys
import shutil
import subprocess
from pathlib import Path
from datetime import datetime


# =============================================================================
# LOGGING
# =============================================================================

def _timestamp() -> str:
    """Get current timestamp in [HH:MM:SS] format."""
    return datetime.now().strftime("[%H:%M:%S]")


def log_debug(msg: str) -> None:
    print(f"{_timestamp()} [DEBUG]  {msg}")


def log_info(msg: str) -> None:
    print(f"{_timestamp()} [INFO]   {msg}")


def log_warn(msg: str) -> None:
    print(f"{_timestamp()} [WARN]   {msg}")


def log_error(msg: str) -> None:
    print(f"{_timestamp()} [ERROR]  {msg}")


# =============================================================================
# COMMAND EXECUTION
# =============================================================================

def run_cmd(cmd: str | list, shell: bool = False, cwd: Path | None = None) -> int:
    """
    Run a command with real-time output to terminal.
    Returns the exit code.
    """
    print(f">>> {cmd if isinstance(cmd, str) else ' '.join(cmd)}")
    result = subprocess.run(
        cmd,
        shell=shell,
        cwd=cwd,
        # stdout and stderr go directly to terminal (no capture)
    )
    return result.returncode


def run_cmd_capture(cmd: list, cwd: Path | None = None) -> tuple[int, str, str]:
    """Run a command and capture output."""
    result = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd)
    return result.returncode, result.stdout, result.stderr


def run_cmd_sudo(cmd: list, cwd: Path | None = None) -> int:
    """Run a command with sudo."""
    return run_cmd(["sudo"] + cmd, cwd=cwd)


# =============================================================================
# DEPENDENCY CHECKS
# =============================================================================

def check_cmake() -> bool:
    """Check if cmake is available."""
    ret, _, _ = run_cmd_capture(["which", "cmake"])
    return ret == 0


def check_compiler() -> bool:
    """Check if a C++ compiler is available."""
    for compiler in ["g++", "clang++"]:
        ret, _, _ = run_cmd_capture(["which", compiler])
        if ret == 0:
            return True
    return False


def check_pkg_config() -> bool:
    """Check if pkg-config is available."""
    ret, _, _ = run_cmd_capture(["which", "pkg-config"])
    return ret == 0


def check_dependency(pkg_name: str) -> bool:
    """Check if a pkg-config dependency is available."""
    ret, _, _ = run_cmd_capture(["pkg-config", "--exists", pkg_name])
    return ret == 0


# pkg-config name -> friendly name for error messages
REQUIRED_DEPS = [
    ("libpipewire-0.3", "PipeWire"),
    ("libspa-0.2", "SPA (PipeWire)"),
    ("libmpg123", "mpg123"),
    ("sndfile", "libsndfile"),
    ("vorbisfile", "libvorbis"),
    ("icu-uc", "ICU"),
    ("icu-i18n", "ICU i18n"),
]


# =============================================================================
# COMMANDS
# =============================================================================

def cmd_build(args, source_dir: Path) -> bool:
    """Build OUROBOROS."""
    build_dir = source_dir / "build"

    log_info("CONFIGURING BUILD")

    cmake_args = ["cmake", "-S", str(source_dir), "-B", str(build_dir)]

    if args.debug or args.debug_log:
        cmake_args.append("-DCMAKE_BUILD_TYPE=Debug")
        log_info("Build type: Debug")
    else:
        cmake_args.append("-DCMAKE_BUILD_TYPE=Release")
        log_info("Build type: Release")

    if args.debug_log:
        cmake_args.append("-DOUROBOROS_DEBUG_LOG=ON")
        log_info("Debug logging: enabled")

    if args.prefix:
        cmake_args.append(f"-DCMAKE_INSTALL_PREFIX={args.prefix}")
        log_info(f"Install prefix: {args.prefix}")

    ret = run_cmd(cmake_args)
    if ret != 0:
        log_error("cmake configure failed!")
        return False
    log_info("Configuration complete")

    # Build
    import multiprocessing
    jobs = multiprocessing.cpu_count()

    log_info("BUILDING")
    log_info(f"Using {jobs} parallel jobs")

    build_cmd = ["cmake", "--build", str(build_dir), f"-j{jobs}"]

    ret = run_cmd(build_cmd)
    if ret != 0:
        log_error("Build failed!")
        return False

    binary = build_dir / "ouroboros"
    if binary.exists():
        size = binary.stat().st_size
        log_info(f"Built {binary} ({size} bytes)")

    return True


def cmd_install(args, source_dir: Path) -> bool:
    """Build and install OUROBOROS."""
    if not cmd_build(args, source_dir):
        return False

    build_dir = source_dir / "build"
    prefix = Path(args.prefix) if args.prefix else Path("/usr/local")
    install_path = prefix / "bin" / "ouroboros"

    log_info("INSTALLING")
    log_info(f"Destination: {install_path}")

    ret = run_cmd(["sudo", "cmake", "--install", str(build_dir)])
    if ret != 0:
        log_error("Install failed!")
        return False

    if install_path.exists():
        size = install_path.stat().st_size
        log_info(f"Installed {install_path} ({size} bytes)")

    print()
    log_info("SUCCESS. Installation complete.")
    log_info("RUN COMMAND: ouroboros")
    log_info("MAN PAGE: man ouroboros")

    return True


def cmd_clean(args, source_dir: Path) -> bool:
    """Clean build directory."""
    build_dir = source_dir / "build"

    log_info("CLEANING")

    if build_dir.exists():
        log_info(f"Removing {build_dir}")
        shutil.rmtree(build_dir)
        log_info("Clean complete")
    else:
        log_info("Nothing to clean")

    return True


def cmd_uninstall(args, source_dir: Path) -> bool:
    """Remove installed files."""
    prefix = Path(args.prefix) if args.prefix else Path("/usr/local")

    files = [
        prefix / "bin" / "ouroboros",
        prefix / "share" / "man" / "man1" / "ouroboros.1",
    ]

    log_info("UNINSTALLING")

    removed = False
    for f in files:
        if f.exists() or f.is_symlink():
            log_info(f"Removing {f}")
            ret = run_cmd_sudo(["rm", "-f", str(f)])
            if ret == 0:
                log_info("Removed")
                removed = True
            else:
                log_error("Failed to remove")

    if not removed:
        log_warn("No installed files found")

    return True


def cmd_test(args, source_dir: Path) -> bool:
    """Run tests."""
    build_dir = source_dir / "build"

    log_info("RUNNING TESTS")

    if not (build_dir / "Makefile").exists():
        log_info("Project not built. Building first...")
        if not cmd_build(args, source_dir):
            return False

    ret = run_cmd(["ctest", "--test-dir", str(build_dir), "--output-on-failure"])

    if ret == 0:
        log_info("All tests passed")
    else:
        log_error("Some tests failed")

    return ret == 0


# =============================================================================
# MAIN
# =============================================================================

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build and install OUROBOROS (terminal music player)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Commands:
  (default)   Build and install
  build       Build only
  clean       Remove build directory
  uninstall   Remove installed files
  test        Run tests

Examples:
  ./install.py                    # Build and install to /usr/local
  ./install.py --debug            # Debug build
  ./install.py --debug-log        # Debug build with logging
  ./install.py --prefix /usr      # Install to /usr
  ./install.py build              # Build only
  ./install.py clean              # Clean build
"""
    )

    parser.add_argument("command", nargs="?", default="install",
                       choices=["install", "build", "clean", "uninstall", "test"],
                       help="Command to run (default: install)")
    parser.add_argument("--debug", action="store_true",
                       help="Build with debug symbols")
    parser.add_argument("--debug-log", action="store_true",
                       help="Build with debug symbols and logging enabled")
    parser.add_argument("--prefix", type=str, default=None,
                       help="Installation prefix (default: /usr/local)")

    args = parser.parse_args()

    source_dir = Path(__file__).parent.resolve()

    print()
    log_info("OUROBOROS installer")
    log_info(f"Source: {source_dir}")
    print()

    # Check dependencies
    if args.command in ["install", "build", "test"]:
        log_info("CHECKING DEPENDENCIES")

        if not check_cmake():
            log_error("cmake not found!")
            print()
            log_info("Install it first:")
            print("         Arch Linux:    sudo pacman -S cmake")
            print("         Debian/Ubuntu: sudo apt install cmake")
            print("         Fedora:        sudo dnf install cmake")
            print()
            return 1
        log_info("cmake found")

        if not check_compiler():
            log_error("C++ compiler not found!")
            print()
            log_info("Install it first:")
            print("         Arch Linux:    sudo pacman -S gcc")
            print("         Debian/Ubuntu: sudo apt install build-essential")
            print("         Fedora:        sudo dnf install gcc-c++")
            print()
            return 1
        log_info("C++ compiler found")

        if not check_pkg_config():
            log_error("pkg-config not found!")
            print()
            log_info("Install it first:")
            print("         Arch Linux:    sudo pacman -S pkgconf")
            print("         Debian/Ubuntu: sudo apt install pkg-config")
            print("         Fedora:        sudo dnf install pkgconf-pkg-config")
            print()
            return 1
        log_info("pkg-config found")

        # Check audio/media dependencies
        missing = []
        for pkg_config_name, friendly_name in REQUIRED_DEPS:
            if not check_dependency(pkg_config_name):
                missing.append((pkg_config_name, friendly_name))

        if missing:
            log_error("Missing dependencies!")
            print()
            log_info("The following libraries are required:")
            for pkg_config_name, friendly_name in missing:
                print(f"           - {friendly_name} ({pkg_config_name})")
            print()
            log_info("Install them:")
            print()
            print("         Arch Linux:")
            print("           sudo pacman -S pipewire mpg123 libsndfile libvorbis icu openssl")
            print()
            print("         Debian/Ubuntu:")
            print("           sudo apt install libpipewire-0.3-dev libspa-0.2-dev \\")
            print("             libmpg123-dev libsndfile1-dev libvorbis-dev \\")
            print("             libicu-dev libssl-dev")
            print()
            print("         Fedora:")
            print("           sudo dnf install pipewire-devel mpg123-devel libsndfile-devel \\")
            print("             libvorbis-devel libicu-devel openssl-devel")
            print()
            return 1

        log_info("All dependencies found")
        print()

    # Run command
    commands = {
        "install": cmd_install,
        "build": cmd_build,
        "clean": cmd_clean,
        "uninstall": cmd_uninstall,
        "test": cmd_test,
    }

    success = commands[args.command](args, source_dir)
    return 0 if success else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nInterrupted by user.")
        sys.exit(130)
