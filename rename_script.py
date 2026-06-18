#!/usr/bin/env python3
"""Replace a placeholder word (in all its case forms) with a new word
throughout the project, including file/directory names.

Usage:
    python rename_script.py --old sensor --new ctd

This rewrites (for --old sensor --new ctd):
    sensor       -> ctd
    Sensor       -> Ctd
    SENSOR       -> CTD
    SensorManager -> CtdManager
and renames any file/directory whose name contains "sensor" (case-insensitively).
"""
import argparse
import os
import re
import sys

EXTENSIONS = {
    ".c", ".h", ".hpp", ".cpp", ".json", ".ini", ".txt", ".yml", ".yaml",
    ".md", ".py",
}

EXCLUDED_DIRS = {".git", "build"}

# Allow CMakeLists.txt (no extension match otherwise) and other extensionless
# files explicitly handled by name.
EXTRA_FILENAMES = {"CMakeLists.txt", "Dockerfile"}

SELF_FILENAME = os.path.basename(__file__)


def build_replacements(old, new):
    return {
        old.lower(): new.lower(),
        old.capitalize(): new.capitalize(),
        old.upper(): new.upper(),
    }


def replace_content(text, replacements):
    pattern = re.compile("|".join(re.escape(k) for k in replacements))
    return pattern.sub(lambda m: replacements[m.group(0)], text)


def replace_name(filename, replacements):
    pattern = re.compile("|".join(re.escape(k) for k in replacements))
    return pattern.sub(lambda m: replacements[m.group(0)], filename)


def should_process_file(filename):
    if filename == SELF_FILENAME:
        return False
    if filename in EXTRA_FILENAMES:
        return True
    _, ext = os.path.splitext(filename)
    return ext in EXTENSIONS


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--old", required=True, help="Existing placeholder word, e.g. sensor")
    parser.add_argument("--new", required=True, help="New word to replace it with, e.g. ctd")
    parser.add_argument("--root", default=".", help="Project root (default: current dir)")
    args = parser.parse_args()

    for label, value in (("--old", args.old), ("--new", args.new)):
        if not re.match(r"^[A-Za-z][A-Za-z0-9_]*$", value):
            sys.exit(f"{label} must start with a letter and contain only letters, digits, underscores")

    root = os.path.abspath(args.root)
    replacements = build_replacements(args.old, args.new)

    # 1. Rewrite file contents.
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in EXCLUDED_DIRS]
        for filename in filenames:
            if not should_process_file(filename):
                continue
            path = os.path.join(dirpath, filename)
            with open(path, "r", encoding="utf-8") as f:
                content = f.read()
            new_content = replace_content(content, replacements)
            if new_content != content:
                with open(path, "w", encoding="utf-8") as f:
                    f.write(new_content)
                print(f"updated: {os.path.relpath(path, root)}")

    # 2. Rename files, deepest first so renaming a dir doesn't break child paths.
    paths_to_rename = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in EXCLUDED_DIRS]
        for filename in filenames:
            if filename == SELF_FILENAME:
                continue
            paths_to_rename.append(os.path.join(dirpath, filename))
        for dirname in dirnames:
            paths_to_rename.append(os.path.join(dirpath, dirname))

    paths_to_rename.sort(key=lambda p: p.count(os.sep), reverse=True)

    for path in paths_to_rename:
        dirpath, base = os.path.split(path)
        new_base = replace_name(base, replacements)
        if new_base != base:
            new_path = os.path.join(dirpath, new_base)
            os.rename(path, new_path)
            print(f"renamed: {os.path.relpath(path, root)} -> {os.path.relpath(new_path, root)}")


if __name__ == "__main__":
    main()
