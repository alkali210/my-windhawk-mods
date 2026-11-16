"""Fetch pinned compatibility samples from Microsoft's symbol server.

These files are used only by the isolated experiment; they are never installed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile
import urllib.request

root = Path(__file__).resolve().parents[1]
samples = json.loads((root / "tests/samples.json").read_text(encoding="utf-8"))
parser = argparse.ArgumentParser()
parser.add_argument("--version", action="append", default=[],
                    help="Exact file version to fetch, e.g. 10.0.26100.1 (repeatable; default all)")
args = parser.parse_args()
known = {sample["version"] for sample in samples}
unknown = set(args.version) - known
if unknown:
    parser.error(f"unknown version(s): {', '.join(sorted(unknown))}")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fetch(url, expected, destination):
    if destination.exists():
        if sha256(destination) != expected:
            raise SystemExit(f"Existing file has a different hash; left untouched: {destination}")
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with urllib.request.urlopen(url, timeout=60) as response:
            with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as output:
                temporary = Path(output.name)
                digest = hashlib.sha256()
                while chunk := response.read(1024 * 1024):
                    output.write(chunk)
                    digest.update(chunk)
        if digest.hexdigest() != expected:
            raise SystemExit(f"Microsoft server returned unexpected bytes for {url}; no file installed")
        temporary.replace(destination)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


for sample in samples:
    if args.version and sample["version"] not in args.version:
        continue
    directory = root / "build/windows-samples" / sample["version"].removeprefix("10.0.")
    fetch(sample["binary_url"], sample["sha256"], directory / "sysdm.cpl")
    fetch(sample["pdb_url"], sample["pdb_sha256"], directory / "sysdm.pdb")
    print(f"Verified {sample['version']}: {directory}", flush=True)
