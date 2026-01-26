from pathlib import Path
from shutil import rmtree
from SCons.Script import Import

Import("env")

PIOENV = env.subst("$PIOENV")
PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
BUILD_DIR = Path(env.subst("$PROJECT_BUILD_DIR")) / PIOENV

# Paths where PlatformIO/IDF may place managed components
CANDIDATES = [
    PROJECT_DIR / "managed_components" / "chmorgan__esp-libhelix-mp3",
    BUILD_DIR / "managed_components" / "chmorgan__esp-libhelix-mp3",
    BUILD_DIR / "src" / "managed_components" / "chmorgan__esp-libhelix-mp3",
]


def _purge(path: Path):
    if path.exists():
        rmtree(path, ignore_errors=True)


def purge_helix_mp3(*args, **kwargs):
    for candidate in CANDIDATES:
        _purge(candidate)


# Run just before building to ensure the ARM-only helix MP3 component is absent
env.AddPreAction("buildprog", purge_helix_mp3)
