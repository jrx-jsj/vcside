#!/bin/bash
# Signs VCSIDE's packages when the credentials are there, and leaves them unsigned when they are not.
# tools/release.sh uses it; it can also be run on its own on a build:
#
#   tools/sign.sh status                  # what would be signed, without signing anything
#   tools/sign.sh macos-app VCSIDE.app    # sign (and notarise, and staple) the app
#   tools/sign.sh macos-dmg VCSIDE.dmg    # sign, notarise and staple a disk image
#   tools/sign.sh windows file.exe...     # sign Windows executables and installers
#   tools/sign.sh windows-verify file.exe...
#
# No secret is kept in the repository or printed: everything comes from the keychain or from
# environment variables, set in your shell (or in a file you source, outside the repository).
#
# macOS
#   VCSIDE_MACOS_IDENTITY   Developer ID Application identity: its name or SHA-1. Default: the only
#                           "Developer ID Application" identity in the keychain. Without one, the
#                           app is signed ad hoc, as before.
#   VCSIDE_NOTARY_PROFILE   notarytool keychain profile (default vcside-notary), made once with
#                           xcrun notarytool store-credentials vcside-notary --apple-id ... --team-id ...
# Windows (signed with jsign: brew install jsign)
#   VCSIDE_WINDOWS_SIGNER   artifact-signing: Azure Artifact Signing (formerly Trusted Signing),
#                             with the Azure CLI signed in (az login) and
#                             VCSIDE_AZURE_SIGNING_ENDPOINT  e.g. https://weu.codesigning.azure.net
#                             VCSIDE_AZURE_SIGNING_ACCOUNT   the signing account
#                             VCSIDE_AZURE_SIGNING_PROFILE   the certificate profile
#                           jsign: any keystore jsign knows (a PKCS#11 token, a cloud HSM, a
#                             .p12 file...), with VCSIDE_JSIGN_STORETYPE, VCSIDE_JSIGN_KEYSTORE,
#                             VCSIDE_JSIGN_ALIAS, VCSIDE_JSIGN_STOREPASS and, if the keystore
#                             needs it, VCSIDE_JSIGN_CERTFILE (see https://ebourg.github.io/jsign/)
#                           unset: not signed.
#   VCSIDE_WINDOWS_TSA      timestamp server (default: Microsoft's for Artifact Signing, else
#                           DigiCert's)
# Both
#   VCSIDE_SIGN=0           sign nothing (ad hoc on macOS), whatever is set
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
NOTARY_PROFILE=${VCSIDE_NOTARY_PROFILE:-vcside-notary}
LOG_DIR=${VCSIDE_SIGN_LOG_DIR:-"$ROOT/build-release"}

die() { echo "sign: $*" >&2; exit 1; }
note() { echo "sign: $*" >&2; }

signing_off() { [ "${VCSIDE_SIGN:-1}" = 0 ]; }

# --- macOS ------------------------------------------------------------------------------------

# Prints the identity to sign with (its SHA-1, never the name), or nothing.
macos_identity() {
  signing_off && return 0
  if [ -n "${VCSIDE_MACOS_IDENTITY:-}" ]; then
    local found
    found=$(security find-identity -v -p codesigning | awk -v id="$VCSIDE_MACOS_IDENTITY" \
      'index($0, "\"" id "\"") || $2 == id { print $2; exit }')
    [ -n "$found" ] || die "VCSIDE_MACOS_IDENTITY is not a valid signing identity in the keychain"
    echo "$found"
    return 0
  fi
  local hashes count
  hashes=$(security find-identity -v -p codesigning | awk '/"Developer ID Application: / { print $2 }')
  count=$(printf '%s' "$hashes" | grep -c . || true)
  [ "$count" -le 1 ] || die "$count Developer ID Application identities in the keychain: set VCSIDE_MACOS_IDENTITY"
  printf '%s' "$hashes"
}

notary_ready() {
  xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" >/dev/null 2>&1
}

# Sends a file to Apple's notary service and waits; stops with the log saved if it is refused.
notarize() {
  local file=$1 result status id
  note "notarising $(basename "$file") (it takes a few minutes)"
  result=$(xcrun notarytool submit "$file" --keychain-profile "$NOTARY_PROFILE" --wait --output-format json) \
    || die "notarytool could not submit $(basename "$file")"
  status=$(printf '%s' "$result" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("status",""))')
  id=$(printf '%s' "$result" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("id",""))')
  if [ "$status" != Accepted ]; then
    mkdir -p "$LOG_DIR"
    xcrun notarytool log "$id" --keychain-profile "$NOTARY_PROFILE" "$LOG_DIR/notary-log.json" >/dev/null 2>&1 || true
    die "Apple did not accept $(basename "$file") ($status): see $LOG_DIR/notary-log.json"
  fi
  note "notarised"
}

is_macho() { file -b "$1" | grep -q 'Mach-O'; }

macos_app() {
  local app=${1:?usage: tools/sign.sh macos-app VCSIDE.app}
  [ -d "$app/Contents/MacOS" ] || die "$app is not an app bundle"
  local identity main nested
  identity=$(macos_identity)
  main="$app/Contents/MacOS/$(defaults read "$(cd "$app" && pwd)/Contents/Info" CFBundleExecutable)"
  if [ -z "$identity" ]; then
    note "no Developer ID: signing $(basename "$app") ad hoc (Gatekeeper will warn)"
    codesign --force --deep --sign - "$app"
    return 0
  fi
  # Inside out, without --deep: every other Mach-O in the bundle (the language-model helper),
  # then the app. Hardened runtime and a secure timestamp, as notarisation requires. VCSIDE needs
  # no entitlements: it does not JIT, load plug-ins, use the camera or the microphone, and the
  # programs it starts (assemblers, emulators, the helper) run under their own signatures.
  while IFS= read -r -d '' nested; do
    [ "$nested" = "$main" ] && continue
    is_macho "$nested" || continue
    codesign --force --options runtime --timestamp --sign "$identity" "$nested"
  done < <(find "$app/Contents" -type f -perm -u+x -print0)
  codesign --force --options runtime --timestamp --sign "$identity" "$app"
  codesign --verify --strict --deep "$app" || die "the signature of $(basename "$app") does not verify"
  note "$(basename "$app") signed with Developer ID"

  if ! notary_ready; then
    note "no notarytool profile \"$NOTARY_PROFILE\": $(basename "$app") is signed but not notarised"
    return 0
  fi
  local zip
  zip="$(mktemp -d)/$(basename "$app" .app).zip"
  ditto -c -k --sequesterRsrc --keepParent "$app" "$zip"
  notarize "$zip"
  rm -f "$zip"
  xcrun stapler staple -q "$app"
  xcrun stapler validate -q "$app" || die "the ticket stapled to $(basename "$app") does not validate"
  spctl --assess --type execute "$app" || die "Gatekeeper does not accept $(basename "$app")"
  note "$(basename "$app") notarised and stapled; Gatekeeper accepts it"
}

macos_dmg() {
  local dmg=${1:?usage: tools/sign.sh macos-dmg file.dmg}
  local identity
  identity=$(macos_identity)
  [ -n "$identity" ] || return 0   # an unsigned disk image is what there was before
  codesign --force --timestamp --sign "$identity" "$dmg"
  if ! notary_ready; then
    note "no notarytool profile \"$NOTARY_PROFILE\": $(basename "$dmg") is signed but not notarised"
    return 0
  fi
  notarize "$dmg"
  xcrun stapler staple -q "$dmg"
  xcrun stapler validate -q "$dmg" || die "the ticket stapled to $(basename "$dmg") does not validate"
  spctl --assess --type open --context context:primary-signature "$dmg" \
    || die "Gatekeeper does not accept $(basename "$dmg")"
  note "$(basename "$dmg") signed, notarised and stapled"
}

# notarised | signed | unsigned
macos_state() {
  if [ -z "$(macos_identity)" ]; then echo unsigned
  elif notary_ready; then echo notarised
  else echo signed
  fi
}

# --- Windows ----------------------------------------------------------------------------------

windows_state() {
  if signing_off || [ -z "${VCSIDE_WINDOWS_SIGNER:-}" ]; then echo unsigned; else echo signed; fi
}

need() { [ -n "${!1:-}" ] || die "$1 is not set (needed by VCSIDE_WINDOWS_SIGNER=$VCSIDE_WINDOWS_SIGNER)"; }

windows_sign() {
  [ $# -gt 0 ] || die "usage: tools/sign.sh windows file.exe..."
  if [ "$(windows_state)" = unsigned ]; then
    note "VCSIDE_WINDOWS_SIGNER is not set: the Windows packages are not signed"
    return 0
  fi
  command -v jsign >/dev/null || die "jsign is not installed (brew install jsign)"
  local storetype keystore alias tsa certfile=""
  case "$VCSIDE_WINDOWS_SIGNER" in
    artifact-signing)
      need VCSIDE_AZURE_SIGNING_ENDPOINT; need VCSIDE_AZURE_SIGNING_ACCOUNT; need VCSIDE_AZURE_SIGNING_PROFILE
      command -v az >/dev/null || die "the Azure CLI is not installed (brew install azure-cli), or set VCSIDE_AZURE_TOKEN"
      storetype=TRUSTEDSIGNING
      keystore=${VCSIDE_AZURE_SIGNING_ENDPOINT#https://}
      keystore=${keystore%/}
      alias="$VCSIDE_AZURE_SIGNING_ACCOUNT/$VCSIDE_AZURE_SIGNING_PROFILE"
      # An access token for the signing service, valid for about an hour, passed in the environment.
      if [ -z "${VCSIDE_AZURE_TOKEN:-}" ]; then
        VCSIDE_AZURE_TOKEN=$(az account get-access-token --resource https://codesigning.azure.net \
          --query accessToken -o tsv) || die "az could not get a token: run az login"
      fi
      export VCSIDE_JSIGN_STOREPASS=$VCSIDE_AZURE_TOKEN
      tsa=${VCSIDE_WINDOWS_TSA:-http://timestamp.acs.microsoft.com}
      ;;
    jsign)
      need VCSIDE_JSIGN_STORETYPE; need VCSIDE_JSIGN_KEYSTORE
      storetype=$VCSIDE_JSIGN_STORETYPE
      keystore=$VCSIDE_JSIGN_KEYSTORE
      alias=${VCSIDE_JSIGN_ALIAS:-}
      certfile=${VCSIDE_JSIGN_CERTFILE:-}
      export VCSIDE_JSIGN_STOREPASS=${VCSIDE_JSIGN_STOREPASS:-}
      tsa=${VCSIDE_WINDOWS_TSA:-http://timestamp.digicert.com}
      ;;
    *) die "VCSIDE_WINDOWS_SIGNER must be artifact-signing or jsign, not \"$VCSIDE_WINDOWS_SIGNER\"" ;;
  esac
  # The password or token goes as env:NAME, so it is not on the command line (ps shows those).
  local args=(--storetype "$storetype" --keystore "$keystore" --storepass env:VCSIDE_JSIGN_STOREPASS
              --alg SHA-256 --tsaurl "$tsa" --tsmode RFC3161 --name VCSIDE --url https://vcside.com)
  [ -n "$alias" ] && args+=(--alias "$alias")
  [ -n "$certfile" ] && args+=(--certfile "$certfile")
  local file
  for file in "$@"; do
    jsign "${args[@]}" "$file" >/dev/null || die "jsign could not sign $(basename "$file")"
    note "$(basename "$file") signed"
  done
}

# Checks the Authenticode signatures with osslsigncode, in the vcside-windows Docker image.
windows_verify() {
  [ $# -gt 0 ] || die "usage: tools/sign.sh windows-verify file.exe..."
  local file dir
  for file in "$@"; do
    dir=$(cd "$(dirname "$file")" && pwd)
    docker run --rm -v "$dir":/check:ro vcside-windows \
      osslsigncode verify -in "/check/$(basename "$file")" >/dev/null \
      || die "the signature of $(basename "$file") does not verify"
    note "$(basename "$file"): signature verified"
  done
}

case "${1:-}" in
  status)
    echo "macOS: $(macos_state)"
    echo "Windows: $(windows_state)"
    ;;
  macos-state) macos_state ;;
  windows-state) windows_state ;;
  macos-app) shift; macos_app "$@" ;;
  macos-dmg) shift; macos_dmg "$@" ;;
  windows) shift; windows_sign "$@" ;;
  windows-verify) shift; windows_verify "$@" ;;
  *) sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2 ;;
esac
