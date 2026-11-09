# Releases

Each version has its notes here, in `<version>.md` ("What is new"). `tools/release.sh` adds `install.md` (downloads, unsigned builds, tools to install) and the checksums to them to make the text of the GitHub release.

## Publishing a version

On a Mac, with Docker running and `gh` signed in, on `main`, with everything committed and pushed:

```sh
tools/release.sh 0.2.0 --dry-run   # optional: build and package only, in build-release/dist
tools/release.sh 0.2.0
```

1. If `docs/releases/0.2.0.md` does not exist, the script writes a draft from the commits since the last tag and stops. Rewrite it for people who use VCSIDE (what they can do now, not the commit list), commit it and run the script again.
2. It sets the version in `CMakeLists.txt` (the About window takes it from there).
3. It builds and tests:
   - macOS: a universal `VCSIDE.app` (arm64 + x86_64, macOS 11 or later) with SDL3 linked statically, signed ad hoc, in a zip. The script stops if the executable links a library from Homebrew.
   - Linux x86-64: in the `vcside-linux` Docker image for `linux/amd64` (emulated on Apple silicon, so it is slow), with SDL3 linked statically. Built on Debian 13, so it needs glibc 2.41 or later.
   - Windows x86-64: cross-compiled with MinGW-w64, static, `vcside.exe` + `data`.
4. It commits the version as "VCSIDE 0.2.0", tags it `v0.2.0` (annotated), pushes both, and creates the GitHub release with the three packages, `SHA256SUMS` and the notes.
5. It tells the website: `tools/release_web.py` writes `release.json` in `../vcside-web` (or `$VCSIDE_WEB`), the website's `scripts/build.py` puts the download buttons, sizes and checksums on its download page, in English and Spanish, and the change is committed and pushed there. If the website is not on a clean `main`, this step is skipped and the command to run is printed. Deploying the website is still a separate step.

There is no automation on GitHub: the script is the whole process, so it runs the same way every time and nothing builds on someone else's machine.

## Signing

The builds are not signed with a Developer ID (macOS) or a code-signing certificate (Windows), so both systems warn the first time; `install.md` says how to open them. Signing and notarising for macOS needs a paid Apple Developer account; once there is one, `codesign --sign "Developer ID Application: ..."` with `--options runtime` and `xcrun notarytool submit --wait` go where the ad hoc signature is now.

## While the repository is private

The download links on the website point to the GitHub release, so they only work for people with access to the repository until it is public.
