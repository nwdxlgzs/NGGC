#!/usr/bin/env python3
"""Check publishable files and Git objects without echoing sensitive values."""

import argparse
import getpass
import json
import os
from pathlib import Path, PurePosixPath
import re
import socket
import subprocess
import sys


ANONYMOUS_NAME = "NGGC Contributors"
ANONYMOUS_EMAIL = "nggc@example.invalid"
MAX_FILE_BYTES = 16 * 1024 * 1024
LOCAL_DIRECTORIES = {".git", ".vs", ".vscode", ".idea", ".codex", "__pycache__", "out"}
BLOCKED_SUFFIXES = {
    ".pem", ".key", ".p12", ".pfx", ".log", ".dmp", ".pdb", ".obj", ".o",
    ".exe", ".dll", ".so", ".dylib", ".a", ".lib", ".pyc", ".user", ".suo",
    ".zip", ".7z", ".bundle",
}
EMAIL_PATTERN = re.compile(r"[A-Za-z0-9_.+%-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}")
RULES = [
    ("absolute-windows-path", re.compile(r"(?i)(?<![A-Za-z0-9_])[A-Z]:[\\/][^\s\"'<>]+")),
    ("personal-home-path", re.compile(r"(?i)/(?:Users|home)/[^\s/\"'<>]+")),
    ("network-share", re.compile(r"\\\\[A-Za-z0-9_.-]+\\[A-Za-z0-9_$.-]+")),
    ("private-key", re.compile(r"-----BEGIN (?:[A-Z0-9 ]+ )?PRIVATE KEY-----")),
    ("github-token", re.compile(r"\b(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,})\b")),
    ("aws-access-key", re.compile(r"\b(?:AKIA|ASIA)[A-Z0-9]{16}\b")),
    ("slack-token", re.compile(r"\bxox[baprs]-[A-Za-z0-9-]{20,}\b")),
    ("api-secret", re.compile(r"\bsk-(?:proj-)?[A-Za-z0-9_-]{20,}\b")),
    ("literal-credential", re.compile(
        r"(?i)\b(?:password|api[_-]?key|access[_-]?token|client[_-]?secret)\b"
        r"\s*[=:]\s*[\"'][^\"'\r\n]{8,}[\"']")),
]


class PublicationError(Exception):
    pass


def safe_git_environment():
    env = os.environ.copy()
    for key in list(env):
        if key.startswith("GIT_"):
            env.pop(key)
    env.update({"GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": os.devnull,
                "GIT_ATTR_NOSYSTEM": "1", "GIT_TERMINAL_PROMPT": "0"})
    return env


def git(root, *args, env=None):
    result = subprocess.run(
        ["git", "-C", str(root), *args], stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, env=env or safe_git_environment(), check=False,
    )
    if result.returncode:
        raise PublicationError("Git operation failed; inspect it locally before publishing.")
    return result.stdout


def private_identities(root=None):
    values = set()
    for value in (getpass.getuser(), socket.gethostname(), Path.home().name):
        if len(value) >= 5:
            values.add(value)
    if root is not None:
        metadata = git(root, "log", "--all", "--format=%an%x00%ae%x00%cn%x00%ce").decode("utf-8")
        for value in re.split(r"[\x00\r\n]", metadata):
            if value and value not in {ANONYMOUS_NAME, ANONYMOUS_EMAIL}:
                values.add(value)
                if "@" in value and len(value.split("@", 1)[0]) >= 6:
                    values.add(value.split("@", 1)[0])
    return sorted(values)


def blocked_path(relative):
    parts = PurePosixPath(relative).parts
    if not parts or PurePosixPath(relative).is_absolute() or ".." in parts:
        return True
    if any(p.lower() in LOCAL_DIRECTORIES or p.lower().startswith("build") for p in parts):
        return True
    name = parts[-1].lower()
    return (Path(name).suffix in BLOCKED_SUFFIXES or name == ".env" or
            name.startswith(".env.") or name in {"id_rsa", "id_ed25519", "credentials", "thumbs.db", ".ds_store"})


def working_tree_paths(root):
    top = Path(git(root, "rev-parse", "--show-toplevel").decode("utf-8").strip()).resolve()
    if top != root.resolve():
        raise PublicationError("The working-tree root must be the repository root.")
    output = git(root, "ls-files", "--cached", "--others", "--exclude-standard", "-z")
    return sorted(set(p.decode("utf-8") for p in output.split(b"\0") if p))


def candidate_paths(root):
    paths = []
    for parent, directories, files in os.walk(root, followlinks=False):
        for name in directories[:]:
            path = Path(parent, name)
            if name == ".git" and path.parent == root:
                directories.remove(name)
            elif path.is_symlink():
                raise PublicationError("Candidate contains a directory symlink.")
        paths.extend(Path(parent, name).relative_to(root).as_posix() for name in files)
    return sorted(paths)


def allowed_email(value):
    domain = value.rsplit("@", 1)[1].lower()
    return domain.endswith(".invalid") or domain in {"example.com", "example.net", "example.org"}


def scan_text(text, location, identities):
    findings = []
    for number, line in enumerate(text.splitlines(), 1):
        rules = {name for name, pattern in RULES if pattern.search(line)}
        if any(not allowed_email(match.group()) for match in EMAIL_PATTERN.finditer(line)):
            rules.add("non-placeholder-email")
        if any(re.search(r"(?<!\w)" + re.escape(value) + r"(?!\w)", line, re.I)
               for value in identities):
            rules.add("local-private-identity")
        findings.extend({"rule": rule, "path": location, "line": number} for rule in sorted(rules))
    return findings


def public_location(relative, identities):
    return "<redacted-path>" if scan_text(relative, "path", identities) else relative


def scan_files(root, paths, identities):
    findings = []
    for relative in paths:
        location = public_location(relative, identities)
        if blocked_path(relative):
            findings.append({"rule": "excluded-file-type", "path": location, "line": 0})
            continue
        findings.extend(scan_text(relative, location, identities))
        path = root.joinpath(*PurePosixPath(relative).parts)
        if path.is_symlink() or root not in path.resolve().parents:
            raise PublicationError("Candidate path escapes its root or is a symlink.")
        if not path.is_file():
            raise PublicationError("A listed source file is missing; reconcile tracked deletions first.")
        if path.stat().st_size > MAX_FILE_BYTES:
            findings.append({"rule": "oversized-file", "path": location, "line": 0})
            continue
        try:
            contents = path.read_bytes().decode("utf-8-sig")
        except UnicodeDecodeError:
            findings.append({"rule": "non-utf8-or-binary", "path": location, "line": 0})
            continue
        if "\0" in contents:
            findings.append({"rule": "binary-content", "path": location, "line": 0})
        findings.extend(scan_text(contents, location, identities))
    return findings


def scan_git(root, identities):
    if not (root / ".git").is_dir():
        raise PublicationError("Publication candidate needs its own Git directory.")
    findings = []
    config_path = root / ".git" / "config"
    if config_path.is_file():
        findings.extend(scan_text(config_path.read_text(encoding="utf-8"), "git-config", identities))
    refs = git(root, "for-each-ref", "--format=%(refname)").decode("utf-8")
    findings.extend(scan_text(refs, "git-refs", identities))
    count = 0
    objects = git(root, "cat-file", "--batch-all-objects", "--batch-check=%(objectname) %(objecttype) %(objectsize)")
    for entry in objects.decode("ascii").splitlines():
        oid, kind, size = entry.split()
        if kind not in {"commit", "tag", "blob"}:
            continue
        location = "git-" + kind + ":" + oid[:12]
        if int(size) > MAX_FILE_BYTES:
            findings.append({"rule": "oversized-git-object", "path": location, "line": 0})
            continue
        raw = git(root, "cat-file", kind, oid)
        try:
            text = raw.decode("utf-8-sig")
        except UnicodeDecodeError:
            findings.append({"rule": "non-utf8-git-object", "path": location, "line": 0})
            continue
        findings.extend(scan_text(text, location, identities))
        if kind in {"commit", "tag"}:
            if kind == "commit":
                count += 1
            identity_lines = [line for line in text.split("\n\n", 1)[0].splitlines()
                              if line.startswith(("author ", "committer ", "tagger "))]
            for line in identity_lines:
                if not re.fullmatch(
                    r"(?:author|committer|tagger) " + re.escape(ANONYMOUS_NAME) +
                    r" <" + re.escape(ANONYMOUS_EMAIL) + r"> \d+ \+0000", line):
                    findings.append({"rule": "non-anonymous-git-identity", "path": location, "line": 0})
    logs = root / ".git" / "logs"
    if logs.exists():
        for path in logs.rglob("*"):
            if path.is_file():
                findings.extend(scan_text(path.read_text(encoding="utf-8"), "git-reflog", identities))
    return findings, count


def audit(root, working_tree=False, private_repository=None):
    root = root.resolve()
    if not root.is_dir():
        raise PublicationError("Candidate directory does not exist.")
    identities = private_identities(private_repository)
    paths = working_tree_paths(root) if working_tree else candidate_paths(root)
    findings = scan_files(root, paths, identities)
    commits = None
    if not working_tree:
        git_findings, commits = scan_git(root, identities)
        findings.extend(git_findings)
    return {"passed": not findings, "file_count": len(paths), "commit_count": commits,
            "scope": "publishable-working-tree" if working_tree else "publication-files-and-git-objects",
            "finding_count": len(findings), "findings": findings}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--working-tree", action="store_true", help="Check Git-selected publishable files, excluding old Git history.")
    parser.add_argument("--private-repository", type=Path, help="Read old Git identities as private deny terms; never copy them into the report.")
    args = parser.parse_args()
    try:
        result = audit(args.directory, args.working_tree, args.private_repository)
    except PublicationError as error:
        print(str(error), file=sys.stderr)
        return 2
    except (OSError, UnicodeError) as error:
        print("Publication check could not complete (" + type(error).__name__ + ").", file=sys.stderr)
        return 2
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
