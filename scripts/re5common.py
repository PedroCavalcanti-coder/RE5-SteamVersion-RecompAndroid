"""Shared paths and helpers for the re5-recompiled pipeline scripts.

Every script in scripts/ imports this instead of hard-coding paths, so the
repository can be moved (or checked out in a fresh cloud container) without
editing anything. The configuration lives in config/re5_config.toml.
"""
import hashlib
import json
import os
import sys
import tomllib

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
CONFIG_DIR = os.path.join(REPO, "config")
WORK_DIR = os.path.join(REPO, "work")          # derived from the game binary: gitignored
DOCS_DIR = os.path.join(REPO, "docs")
PCRECOMP = os.environ.get("PCRECOMP", os.path.join(REPO, "tools", "pcrecomp"))

LFS_POINTER_PREFIX = b"version https://git-lfs.github.com/spec/v1"


def load_config():
    with open(os.path.join(CONFIG_DIR, "re5_config.toml"), "rb") as f:
        return tomllib.load(f)


def load_known_builds():
    with open(os.path.join(CONFIG_DIR, "known_builds.json"), encoding="utf-8") as f:
        return json.load(f)["builds"]


def repo_path(rel):
    return rel if os.path.isabs(rel) else os.path.join(REPO, rel)


def is_lfs_pointer(path):
    """True when `path` is a Git LFS pointer file instead of the real content
    (the checkout happened without `git lfs pull`)."""
    try:
        with open(path, "rb") as f:
            return f.read(len(LFS_POINTER_PREFIX)) == LFS_POINTER_PREFIX
    except OSError:
        return False


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def add_pcrecomp_to_path(*subdirs):
    """Make pcrecomp's tool modules importable (pe_analyze, disasm32, lift32_cpu...)."""
    for sub in subdirs:
        p = os.path.join(PCRECOMP, "tools", sub)
        if p not in sys.path:
            sys.path.insert(0, p)


def ensure_work_dir():
    os.makedirs(WORK_DIR, exist_ok=True)
    return WORK_DIR
