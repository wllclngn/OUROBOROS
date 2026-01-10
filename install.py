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

def run(cmd, check=True, capture=False, sudo=False, cwd=None):
    """Run a command, optionally with sudo."""
    if sudo:
        cmd = ["sudo"] + cmd
    if capture:
        result = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd)
        return result.returncode, result.stdout, result.stderr
    else:
        result = subprocess.run(cmd, cwd=cwd)
        return result.returncode, None, None

def check_cmake():
    """Check if cmake is available."""
    ret, _, _ = run(["which", "cmake"], capture=True)
    return ret == 0

def check_compiler():
    """Check if a C++ compiler is available."""
    for compiler in ["g++", "clang++"]:
        ret, _, _ = run(["which", compiler], capture=True)
        if ret == 0:
            return True
    return False

def check_pkg_config():
    """Check if pkg-config is available."""
    ret, _, _ = run(["which", "pkg-config"], capture=True)
    return ret == 0

def check_dependency(pkg_name):
    """Check if a pkg-config dependency is available."""
    ret, _, _ = run(["pkg-config", "--exists", pkg_name], capture=True)
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

def cmd_build(args, source_dir):
    """Build OUROBOROS."""
    build_dir = source_dir / "build"

    # Configure
    cmake_args = ["cmake", "-S", str(source_dir), "-B", str(build_dir)]

    if args.debug or args.debug_log:
        cmake_args.append("-DCMAKE_BUILD_TYPE=Debug")
    else:
        cmake_args.append("-DCMAKE_BUILD_TYPE=Release")

    if args.debug_log:
        cmake_args.append("-DOUROBOROS_DEBUG_LOG=ON")

    if args.prefix:
        cmake_args.append(f"-DCMAKE_INSTALL_PREFIX={args.prefix}")

    print("Configuring...")
    ret, _, stderr = run(cmake_args, capture=True)
    if ret != 0:
        print(f"ERROR: cmake configure failed!")
        print(stderr)
        return False

    # Build
    import multiprocessing
    jobs = multiprocessing.cpu_count()

    print(f"Building (using {jobs} jobs)...")
    ret, _, stderr = run(["cmake", "--build", str(build_dir), f"-j{jobs}"], capture=True)
    if ret != 0:
        print(f"ERROR: Build failed!")
        print(stderr)
        return False

    print("  OK: Build complete")
    return True

def cmd_install(args, source_dir):
    """Build and install OUROBOROS."""
    if not cmd_build(args, source_dir):
        return False

    build_dir = source_dir / "build"

    print("Installing...")
    ret, _, stderr = run(["cmake", "--install", str(build_dir)], sudo=True, capture=True)
    if ret != 0:
        print(f"ERROR: Install failed!")
        print(stderr)
        return False

    print("  OK: Installed")
    print()
    print("=" * 50)
    print("SUCCESS!")
    print("=" * 50)
    print()
    print("Run:")
    print("  ouroboros")
    print()
    print("Man page:")
    print("  man ouroboros")
    print()

    return True

def cmd_clean(args, source_dir):
    """Clean build directory."""
    build_dir = source_dir / "build"

    if build_dir.exists():
        print(f"Removing {build_dir}...")
        shutil.rmtree(build_dir)
        print("  OK: Cleaned")
    else:
        print("Nothing to clean")

    return True

def cmd_uninstall(args, source_dir):
    """Remove installed files."""
    prefix = Path(args.prefix) if args.prefix else Path("/usr/local")

    files = [
        prefix / "bin" / "ouroboros",
        prefix / "share" / "man" / "man1" / "ouroboros.1",
    ]

    removed = False
    for f in files:
        if f.exists() or f.is_symlink():
            print(f"Removing {f}...")
            ret, _, _ = run(["rm", "-f", str(f)], sudo=True)
            if ret == 0:
                print("  OK: Removed")
                removed = True
            else:
                print("  ERROR: Failed to remove")

    if not removed:
        print("No installed files found")

    return True

def cmd_test(args, source_dir):
    """Run tests."""
    build_dir = source_dir / "build"

    if not (build_dir / "Makefile").exists():
        print("Project not built. Building first...")
        if not cmd_build(args, source_dir):
            return False

    print("Running tests...")
    ret, _, _ = run(["ctest", "--test-dir", str(build_dir), "--output-on-failure"])
    return ret == 0

def main():
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

    print("OUROBOROS installer")
    print("===================")
    print(f"Source: {source_dir}")
    print()

    # Check dependencies
    if args.command in ["install", "build", "test"]:
        print("Checking dependencies...")

        if not check_cmake():
            print()
            print("ERROR: cmake not found!")
            print()
            print("Install it first:")
            print("  Arch Linux:    sudo pacman -S cmake")
            print("  Debian/Ubuntu: sudo apt install cmake")
            print("  Fedora:        sudo dnf install cmake")
            print()
            sys.exit(1)
        print("  OK: cmake found")

        if not check_compiler():
            print()
            print("ERROR: C++ compiler not found!")
            print()
            print("Install it first:")
            print("  Arch Linux:    sudo pacman -S gcc")
            print("  Debian/Ubuntu: sudo apt install build-essential")
            print("  Fedora:        sudo dnf install gcc-c++")
            print()
            sys.exit(1)
        print("  OK: C++ compiler found")

        if not check_pkg_config():
            print()
            print("ERROR: pkg-config not found!")
            print()
            print("Install it first:")
            print("  Arch Linux:    sudo pacman -S pkgconf")
            print("  Debian/Ubuntu: sudo apt install pkg-config")
            print("  Fedora:        sudo dnf install pkgconf-pkg-config")
            print()
            sys.exit(1)
        print("  OK: pkg-config found")

        # Check audio/media dependencies
        missing = []
        for pkg_config_name, friendly_name in REQUIRED_DEPS:
            if not check_dependency(pkg_config_name):
                missing.append((pkg_config_name, friendly_name))

        if missing:
            print()
            print("ERROR: Missing dependencies!")
            print()
            print("The following libraries are required:")
            for pkg_config_name, friendly_name in missing:
                print(f"  - {friendly_name} ({pkg_config_name})")
            print()
            print("Install them:")
            print()
            print("  Arch Linux:")
            print("    sudo pacman -S pipewire mpg123 libsndfile libvorbis icu openssl")
            print()
            print("  Debian/Ubuntu:")
            print("    sudo apt install libpipewire-0.3-dev libspa-0.2-dev \\")
            print("      libmpg123-dev libsndfile1-dev libvorbis-dev \\")
            print("      libicu-dev libssl-dev")
            print()
            print("  Fedora:")
            print("    sudo dnf install pipewire-devel mpg123-devel libsndfile-devel \\")
            print("      libvorbis-devel libicu-devel openssl-devel")
            print()
            sys.exit(1)

        print("  OK: All dependencies found")
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
    sys.exit(0 if success else 1)

if __name__ == "__main__":
    main()
