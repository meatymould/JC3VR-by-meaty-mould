"""Audit public files and every reachable Git blob without printing secret values."""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SECRET = re.compile(
    rb"(?:\bgh[pousr]_[A-Za-z0-9]{25,}|\bgithub_pat_[A-Za-z0-9_]{35,}|"
    rb"\bsk-(?:proj-|ant-)[A-Za-z0-9_-]{24,}|\bAKIA[A-Z0-9]{16}|"
    rb"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----|\bxox[baprs]-[A-Za-z0-9-]{20,})"
)
PERSONAL_PATH = re.compile(rb"(?i)[a-z]:[\\/]+Users[\\/]+(?!PLAYER\b|Public\b|<)[^\\/\s\"<>]+")
ASSIGNMENT = re.compile(
    rb"""(?im)^\s*(?:\$|export\s+)?(?:[A-Z_]*(?:API_KEY|ACCESS_TOKEN|PASSWORD|CLIENT_SECRET))\s*[=:]\s*["']?([A-Za-z0-9_+/.=-]{12,})"""
)
ASSETS = {
    ".dll", ".exe", ".pdb", ".rbm", ".ee", ".arc", ".tab", ".dds", ".ddsc",
    ".hmddsc", ".rgba", ".png", ".jpg", ".jpeg", ".mp4", ".mov", ".zip",
    ".ban", ".pfxc", ".gfx", ".sarc", ".mesh",
}


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args])


def issues(data, path):
    found = []
    if SECRET.search(data):
        found.append("credential pattern")
    if PERSONAL_PATH.search(data):
        found.append("personal home path")
    if ASSIGNMENT.search(data):
        found.append("credential assignment requiring review")
    if Path(path).suffix.lower() in ASSETS or b"\0" in data[:8192]:
        found.append("binary or artwork requires exclusion")
    if Path(path).name == ".env" or Path(path).suffix.lower() in {".pem", ".key", ".p12", ".pfx"}:
        found.append("private configuration or key")
    return found


def main():
    failures = []
    paths = git("ls-files", "--cached", "--others", "--exclude-standard", "-z").decode().split("\0")
    for name in filter(None, paths):
        path = ROOT / name
        if path.is_file():
            failures.extend(f"{name}: {reason}" for reason in issues(path.read_bytes(), name))
    objects = {}
    for line in git("rev-list", "--objects", "--all").decode().splitlines():
        oid, _, name = line.partition(" ")
        objects[oid] = name
    count = 0
    with subprocess.Popen(["git", "-C", str(ROOT), "cat-file", "--batch"],
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE) as process:
        for oid, name in objects.items():
            process.stdin.write((oid + "\n").encode())
            process.stdin.flush()
            header = process.stdout.readline().split()
            size = int(header[2])
            data = process.stdout.read(size)
            process.stdout.read(1)
            if header[1] == b"blob":
                count += 1
                failures.extend(f"history {oid[:12]} {name}: {reason}" for reason in issues(data, name))
        process.stdin.close()
        process.wait()
    metadata = git("log", "--all", "--format=%an <%ae>%n%cn <%ce>%n%B")
    if SECRET.search(metadata) or PERSONAL_PATH.search(metadata):
        failures.append("Git metadata contains sensitive information")
    for failure in failures:
        print(failure)
    print(f"Checked {len([p for p in paths if p])} current files and {count} historical blobs")
    print("PASS" if not failures else "FAIL")
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
