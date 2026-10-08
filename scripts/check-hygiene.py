"""Check changed source without reformatting inherited code or printing secrets."""

import argparse
import difflib
from pathlib import Path
import re
import subprocess
import sys
import unicodedata
import xml.etree.ElementTree as ET


STABLE_BASELINE = "1a83fec8b5c565b14b06ffa8e1eb7e4768057573"
CPP_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
ARTIFACT_SUFFIXES = {
    ".dll", ".exe", ".lib", ".pdb", ".obj", ".o", ".a", ".so", ".dylib",
    ".pfx", ".p12", ".pyc", ".msi", ".nupkg", ".zip", ".7z", ".etl",
    ".dmp", ".pcap", ".onnx", ".pt", ".pth", ".safetensors", ".gguf",
}
ARTIFACT_DIRECTORIES = {"bin", "obj", "packages", ".vs", "__pycache__", "node_modules"}
SECRET_PATTERNS = (
    re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----"),
    re.compile(r"\bgh[pousr]_[A-Za-z0-9]{36,}\b|\bgithub_pat_[A-Za-z0-9_]{40,}\b"),
    re.compile(r"\bAKIA[A-Z0-9]{16}\b|\bxox[baprs]-[A-Za-z0-9-]{20,}\b"),
    re.compile(r"https?://[^\s/:@]+:[^\s/@]+@", re.IGNORECASE),
    re.compile(r"\b(?:password|client_secret|api_key|access_token)\s*[:=]\s*['\"][^'\"\s]{8,}['\"]", re.IGNORECASE),
)
MACHINE_PATH = re.compile(r"[A-Za-z]:[/\\](?:Users|Documents and Settings)[/\\]|/(?:Users|home)/[^/\s]+/", re.IGNORECASE)


class CheckError(Exception):
    pass


def run_checked(command, repo, *, input=None, boundary="git"):
    try:
        result = subprocess.run(command, cwd=repo, input=input, capture_output=True, check=True)
    except (OSError, subprocess.CalledProcessError) as error:
        # External stderr can include source or credentials. Report only the boundary.
        raise CheckError(f"{boundary} command failed ({type(error).__name__})") from error
    return result.stdout


def git(repo, *args):
    return run_checked(["git", *args], repo)


def normalized_text(data):
    if b"\0" in data:
        return None
    try:
        return data.decode("utf-8-sig").replace("\r\n", "\n").replace("\r", "\n")
    except UnicodeDecodeError:
        return None


def added_ranges(before, after):
    return [
        (start + 1, end)
        for tag, _, _, start, end in difflib.SequenceMatcher(
            None, before.splitlines(keepends=True), after.splitlines(keepends=True), autojunk=False
        ).get_opcodes()
        if tag in {"insert", "replace"} and start < end
    ]


def check_format(repo, path, text, ranges, formatter):
    command = [formatter, "--style=file", f"--assume-filename={repo / path}", "--output-replacements-xml"]
    command.extend(f"--lines={start}:{end}" for start, end in ranges)
    output = run_checked(command, repo, input=text.encode("utf-8"), boundary="formatter")
    try:
        replacements = ET.fromstring(output)
    except ET.ParseError as error:
        raise CheckError("formatter returned invalid replacement XML") from error
    if replacements.tag != "replacements" or replacements.get("incomplete_format") != "false":
        raise CheckError("formatter did not complete formatting")
    return len(replacements.findall("replacement"))


def check_repository(repo, base, formatter):
    repo = Path(git(repo, "rev-parse", "--show-toplevel").decode().strip())
    try:
        base_sha = git(repo, "rev-parse", "--verify", f"{base}^{{commit}}").decode().strip()
    except CheckError as error:
        raise CheckError("base must resolve to an existing commit") from error

    base_tree = {}
    for entry in git(repo, "ls-tree", "-r", "-z", base_sha).split(b"\0"):
        if entry:
            metadata, raw_path = entry.split(b"\t", 1)
            mode, kind, oid = metadata.decode().split()
            if kind == "blob":
                base_tree[raw_path.decode("utf-8")] = oid

    tracked = {path.decode("utf-8") for path in git(repo, "ls-files", "--cached", "-z").split(b"\0") if path}
    untracked = {path.decode("utf-8") for path in git(repo, "ls-files", "--others", "--exclude-standard", "-z").split(b"\0") if path}
    failures = []
    folded = {}
    for path in sorted(tracked | untracked):
        key = unicodedata.normalize("NFC", path).casefold()
        previous = folded.get(key)
        if previous is not None and previous != path:
            failures.append(f"case collision: {previous} / {path}")
        folded[key] = path

    format_inputs = []
    checked = 0
    for path in sorted(tracked | untracked):
        physical_path = repo / path
        if not physical_path.is_file():
            continue  # Deleted files and submodules contain no current source to check.
        if physical_path.is_symlink():
            failures.append(f"symlink: {path} (source checks require a regular file)")
            continue
        current = physical_path.read_bytes()
        previous = git(repo, "cat-file", "blob", base_tree[path]) if path in base_tree else None
        current_text = normalized_text(current)
        previous_text = normalized_text(previous) if previous is not None else ""
        if current == previous or (current_text is not None and current_text == previous_text):
            continue
        checked += 1
        relative_path = Path(path)
        if relative_path.suffix.lower() in ARTIFACT_SUFFIXES or any(
            part.casefold() in ARTIFACT_DIRECTORIES for part in relative_path.parts
        ):
            failures.append(f"artifact: {path} (new or changed generated/binary payload)")
        if current_text is None:
            failures.append(f"binary: {path} (new or changed non-UTF-8 source)")
            continue
        ranges = added_ranges(previous_text or "", current_text)
        lines = current_text.splitlines(keepends=True)
        for start, end in ranges:
            for number in range(start, end + 1):
                line = lines[number - 1].rstrip("\n")
                if line.endswith((" ", "\t")):
                    failures.append(f"whitespace: {path}:{number}")
                if any(pattern.search(line) for pattern in SECRET_PATTERNS):
                    failures.append(f"secret: {path}:{number} (review privately; value suppressed)")
                if MACHINE_PATH.search(line):
                    failures.append(f"machine path: {path}:{number}")
        if current_text.endswith("\n\n") and not (previous_text or "").endswith("\n\n"):
            failures.append(f"whitespace: {path} (new blank line at end of file)")
        if relative_path.suffix.lower() in CPP_SUFFIXES and not path.casefold().startswith("external/"):
            if path not in base_tree:
                format_inputs.append((path, current_text, []))
            elif ranges:
                format_inputs.append((path, current_text, ranges))

    if format_inputs:
        version = run_checked([formatter, "--version"], repo, boundary="formatter").decode("utf-8")
        if not re.search(r"\bclang-format version 22\.1\.1\b", version):
            raise CheckError("formatter must be clang-format 22.1.1")
        for path, text, ranges in format_inputs:
            count = check_format(repo, path, text, ranges, formatter)
            if count:
                failures.append(f"format: {path} ({count} replacements; {'changed lines' if ranges else 'full new file'})")
    return failures, checked, len(format_inputs), base_sha


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--base", default=STABLE_BASELINE, help=f"Existing comparison commit/ref (default: {STABLE_BASELINE})")
    parser.add_argument("--clang-format", default="clang-format", help="clang-format 22.1.1 executable")
    args = parser.parse_args()
    try:
        failures, checked, formatted, base = check_repository(args.repo, args.base, args.clang_format)
    except (CheckError, OSError, UnicodeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    for failure in failures:
        print(f"FAIL: {failure}")
    print(f"{'FAIL' if failures else 'PASS'}: {checked} changed files; {formatted} format checks; base {base}")
    return int(bool(failures))


if __name__ == "__main__":
    sys.exit(main())
