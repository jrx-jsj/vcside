#!/usr/bin/env python3
"""Writes the website's release.json for a version that tools/release.sh has packaged.

    python3 tools/release_web.py VERSION DIST_FOLDER OWNER/REPO WEB_FOLDER

The website (vcside-web) builds its download page from that file: version, date, the
release page and, for each system, the package's name, address, size and SHA-256.
"""
import datetime
import hashlib
import json
import os
import sys


def main():
    version, dist, repo, web = sys.argv[1:5]
    tag = "v" + version
    base = f"https://github.com/{repo}/releases"
    names = {
        "macos": f"VCSIDE-{version}-macos.zip",
        "windows": f"vcside-{version}-windows-x86_64.zip",
        "linux": f"vcside-{version}-linux-x86_64.tar.gz",
    }
    files = {}
    for system, name in names.items():
        path = os.path.join(dist, name)
        with open(path, "rb") as package:
            digest = hashlib.sha256(package.read()).hexdigest()
        files[system] = {"name": name, "url": f"{base}/download/{tag}/{name}",
                         "size": os.path.getsize(path), "sha256": digest}
    release = {"version": version, "date": datetime.date.today().isoformat(),
               "notes": f"{base}/tag/{tag}", "files": files}
    with open(os.path.join(web, "release.json"), "w", encoding="utf-8") as output:
        json.dump(release, output, indent=2)
        output.write("\n")
    print("release.json:", version)


if __name__ == "__main__":
    main()
