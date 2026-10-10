#!/usr/bin/env python3
"""Prepare an independent anonymous repository, source ZIP and hash manifest."""

import argparse
from datetime import date
import hashlib
import json
from pathlib import Path, PurePosixPath
import sys
import tempfile
import zipfile

from check_publication import (
    ANONYMOUS_EMAIL, ANONYMOUS_NAME, PublicationError, audit, git,
    safe_git_environment, working_tree_paths,
)


PROJECT_NAME = "NextGenerationGarbageCollector"


def digest(path):
    with path.open("rb") as stream:
        value = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def prepare(source, output, release_date):
    source = source.resolve()
    output = output.resolve()
    if output.exists():
        raise PublicationError("Output already exists; use a new output directory.")
    if source == output or (source in output.parents and output.relative_to(source).parts[0] != "out"):
        raise PublicationError("An in-repository output must be below the ignored out directory.")
    if output in source.parents:
        raise PublicationError("Output must not contain the source repository.")
    source_report = audit(source, working_tree=True, private_repository=source)
    if not source_report["passed"]:
        print(json.dumps(source_report, ensure_ascii=False, indent=2))
        raise PublicationError("Publishable source did not pass the privacy check.")
    paths = working_tree_paths(source)
    output.mkdir(parents=True, exist_ok=False)
    candidate = output / PROJECT_NAME
    candidate.mkdir()
    for relative in paths:
        target = candidate.joinpath(*PurePosixPath(relative).parts)
        target.parent.mkdir(parents=True, exist_ok=True)
        # No source timestamps, ownership, extended attributes or filesystem metadata.
        target.write_bytes(source.joinpath(*PurePosixPath(relative).parts).read_bytes())

    env = safe_git_environment()
    timestamp = release_date.isoformat() + "T00:00:00+0000"
    env.update({
        "GIT_AUTHOR_NAME": ANONYMOUS_NAME, "GIT_AUTHOR_EMAIL": ANONYMOUS_EMAIL,
        "GIT_COMMITTER_NAME": ANONYMOUS_NAME, "GIT_COMMITTER_EMAIL": ANONYMOUS_EMAIL,
        "GIT_AUTHOR_DATE": timestamp, "GIT_COMMITTER_DATE": timestamp,
    })
    with tempfile.TemporaryDirectory(prefix="nggc-empty-git-template-") as template:
        git(candidate, "init", "--initial-branch=main", "--template=" + template, env=env)
    for key, value in (
        ("user.name", ANONYMOUS_NAME), ("user.email", ANONYMOUS_EMAIL),
        ("user.useConfigOnly", "true"), ("commit.gpgSign", "false"),
        ("tag.gpgSign", "false"), ("core.logAllRefUpdates", "false"),
        ("core.autocrlf", "false"), ("core.quotePath", "false"),
    ):
        git(candidate, "config", "--local", key, value, env=env)
    git(candidate, "add", "--all", env=env)
    git(candidate, "commit", "--no-gpg-sign", "-m", "Initial public snapshot (ABI 0.88)", env=env)
    result = audit(candidate, private_repository=source)
    if not result["passed"] or result["commit_count"] != 1:
        print(json.dumps(result, ensure_ascii=False, indent=2))
        raise PublicationError("Candidate audit failed; do not publish this output.")
    if git(candidate, "status", "--porcelain", env=env).strip():
        raise PublicationError("Candidate changed during export; regenerate it.")
    if git(candidate, "remote", env=env).strip():
        raise PublicationError("Unexpected remote in candidate repository.")

    archive = output / (PROJECT_NAME + "-source.zip")
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED) as bundle:
        for relative in paths:
            entry = zipfile.ZipInfo(PROJECT_NAME + "/" + relative, date_time=(1980, 1, 1, 0, 0, 0))
            entry.create_system = 3
            entry.external_attr = 0o100644 << 16
            entry.compress_type = zipfile.ZIP_DEFLATED
            bundle.writestr(entry, candidate.joinpath(*PurePosixPath(relative).parts).read_bytes())
    checksums = [digest(archive) + "  " + archive.name]
    checksums.extend(digest(candidate.joinpath(*PurePosixPath(relative).parts)) +
                     "  " + PROJECT_NAME + "/" + relative for relative in paths)
    (output / "SHA256SUMS.txt").write_text("\n".join(checksums) + "\n", encoding="utf-8")
    result.update({
        "release_date": release_date.isoformat(),
        "abi": "0.88",
        "branch": "main",
        "commit": git(candidate, "rev-parse", "HEAD", env=env).decode("ascii").strip(),
        "source_archive": archive.name,
        "source_archive_sha256": digest(archive),
        "license": "not-selected",
        "source_history_copied": False,
        "remotes": [],
    })
    (output / "publication-report.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False, indent=2))


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=root)
    parser.add_argument("--output", type=Path, default=root / "out" / "publication")
    parser.add_argument("--release-date", type=date.fromisoformat, default=date.today())
    args = parser.parse_args()
    try:
        prepare(args.source, args.output, args.release_date)
    except PublicationError as error:
        print(str(error), file=sys.stderr)
        return 1
    except (OSError, UnicodeError) as error:
        print("Publication preparation stopped (" + type(error).__name__ + "). Inspect any partial output locally.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
