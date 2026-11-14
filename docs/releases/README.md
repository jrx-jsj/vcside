# Releases

Each version has its notes here, in `<version>.md` ("What is new"). `tools/release.sh` adds `install.md` (downloads, how to open the builds that are not signed, tools to install) and the checksums to them to make the text of the GitHub release.

## Publishing a version

On a Mac, with Docker running and `gh` signed in, on `main`, with everything committed and pushed:

```sh
tools/release.sh 0.2.0 --dry-run   # optional: build and package only, in build-release/dist
tools/release.sh 0.2.0
```

1. If `docs/releases/0.2.0.md` does not exist, the script writes a draft from the commits since the last tag and stops. Rewrite it for people who use VCSIDE (what they can do now, not the commit list), commit it and run the script again.
2. It sets the version in `CMakeLists.txt` (the About window takes it from there).
3. It builds and tests:
   - macOS: a universal `VCSIDE.app` (arm64 + x86_64, macOS 11 or later) with SDL3 linked statically, signed with Developer ID, notarised and stapled (ad hoc without the credentials, see [Signing](#signing)), in a disk image (`.dmg`, made with `hdiutil`, with a link to Applications) and in a zip. The script stops if the executable links a library from Homebrew.
   - Linux x86-64 and arm64: in the `vcside-linux-amd64` and `vcside-linux-arm64` Docker images (on Apple silicon arm64 runs natively and amd64 is emulated, so it is slow), with SDL3 linked statically. Built on Debian 13, so they need glibc 2.41 or later.
   - Windows x86-64: cross-compiled with MinGW-w64, static, `vcside.exe` + `data` in a zip, and an installer (`-setup.exe`) made with NSIS from `tools/windows/vcside.nsi` in the same Docker image. With the credentials, `vcside.exe` is signed before it is zipped and packed, then the installer, and osslsigncode checks both. It installs for the current user in `%LOCALAPPDATA%\Programs\VCSIDE`, so it needs no administrator rights. There is no Windows on Arm build: Windows 11 on Arm runs the x86-64 one.
4. It commits the version as "VCSIDE 0.2.0", tags it `v0.2.0` (annotated), pushes both, and creates the GitHub release with the six packages (`.dmg` and zip for macOS, installer and zip for Windows, x86-64 and arm64 for Linux), `SHA256SUMS` and the notes.
5. It tells the website: `tools/release_web.py` writes `release.json` in `../vcside-web` (or `$VCSIDE_WEB`), the website's `scripts/build.py` puts the download buttons, sizes and checksums on its download page, in English and Spanish, and the change is committed and pushed there. If the website is not on a clean `main`, this step is skipped and the command to run is printed. Deploying the website is still a separate step.

There is no automation on GitHub: the script is the whole process, so it runs the same way every time and nothing builds on someone else's machine.

## Signing

`tools/sign.sh` signs what it has credentials for and leaves the rest as it was, so the release works with all, some or none of them; `tools/sign.sh status` says what it would do, and `release.sh` says it at the start. `install.md` keeps the paragraph on opening unsigned builds only for the platforms that are not signed. Nothing secret is in the repository: identities, passwords and tokens come from the keychain or from environment variables (keep them in a file outside the repository and `source` it before releasing). `VCSIDE_SIGN=0` turns signing off for a run.

### macOS: Developer ID and notarisation

Needs a membership of the Apple Developer Program, as an individual or an organisation.

1. In Xcode, *Settings → Accounts → Manage Certificates → + → Developer ID Application* (or at developer.apple.com, *Certificates*, with a CSR from Keychain Access). The certificate and its private key go to the login keychain. Back up both (an encrypted `.p12`, kept outside the repository): Apple lets you make only a few Developer ID certificates.
2. Make an app-specific password at account.apple.com (*Sign-In and Security → App-Specific Passwords*) and store it for notarytool, once:
   ```sh
   xcrun notarytool store-credentials vcside-notary --apple-id <Apple ID> --team-id <team ID>
   ```
   It asks for the password and keeps everything in the keychain under the profile name `vcside-notary` (or set `VCSIDE_NOTARY_PROFILE` to another name).
3. `tools/sign.sh status` should now say `macOS: notarised`.

With exactly one *Developer ID Application* identity in the keychain it is found by itself; with more, set `VCSIDE_MACOS_IDENTITY` to its name or SHA-1. The script signs every other Mach-O in the bundle first (the language-model helper, `Resources/llm/bin/llama-server`), then the app, with the hardened runtime and a secure timestamp, and no entitlements: VCSIDE does not JIT or load plug-ins, and the tools it starts run under their own signatures. The hardened runtime refuses libraries signed by someone else, which is one more reason the release links SDL3 statically. The app is sent to Apple's notary service in a zip (`notarytool submit --wait`), the ticket is stapled to it, Gatekeeper is asked (`spctl --assess`), and then it is zipped and put in the disk image; the disk image is signed, notarised and stapled too. If Apple refuses something, its log is saved in `build-release/notary-log.json`. Without a notarytool profile the app is still signed with Developer ID, but Gatekeeper warns until it is notarised.

To check a build by hand:

```sh
codesign --verify --strict --deep -vv VCSIDE.app
spctl --assess -vv --type execute VCSIDE.app
xcrun stapler validate VCSIDE.app
spctl --assess -vv --type open --context context:primary-signature VCSIDE-X.Y.Z-macos.dmg
```

A Developer ID signature also keeps the app's identity the same from one version to the next, so the Keychain stops asking again for the assistant's keys after each update (see [design-assistant.md](../design-assistant.md)).

### Windows: Authenticode

Signed on the Mac with [jsign](https://ebourg.github.io/jsign/) (`brew install jsign`), which talks to cloud signing services and hardware tokens alike, with an RFC 3161 timestamp so the signature outlives the certificate. `VCSIDE_WINDOWS_SIGNER` chooses how:

- `artifact-signing`: Microsoft's Azure Artifact Signing (formerly Trusted Signing). Needs the Azure CLI (`brew install azure-cli`) signed in with `az login` to an account with the *Certificate Profile Signer* role on the signing account, and `VCSIDE_AZURE_SIGNING_ENDPOINT` (the region's endpoint, such as `https://weu.codesigning.azure.net`), `VCSIDE_AZURE_SIGNING_ACCOUNT` and `VCSIDE_AZURE_SIGNING_PROFILE`. The script asks `az` for a token that lasts about an hour.
- `jsign`: any other keystore jsign supports, such as a certificate on a USB token or smart card (PKCS#11), a CA's cloud signing service or, for tests, a `.p12` file: `VCSIDE_JSIGN_STORETYPE`, `VCSIDE_JSIGN_KEYSTORE`, `VCSIDE_JSIGN_ALIAS`, `VCSIDE_JSIGN_STOREPASS` and, if the keystore needs it, `VCSIDE_JSIGN_CERTFILE`, as in jsign's documentation.

The password or token reaches jsign through the environment (`--storepass env:...`), never on the command line. `VCSIDE_WINDOWS_TSA` changes the timestamp server. The uninstaller that the installer writes on the user's computer is not signed: NSIS makes it while compiling, inside Docker, where the signing credentials are not; SmartScreen does not check it because it is not downloaded.

SmartScreen judges a signature by its reputation, which grows as people download and run the signed files, so the first signed versions may still be asked about once.

## While the repository is private

The download links on the website point to the GitHub release, so they only work for people with access to the repository until it is public.
