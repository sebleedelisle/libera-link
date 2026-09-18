# CI and Release Setup

This repo uses GitHub Actions for:

- signed, packaged builds on `push` to `main`
- unsigned CI builds on pull requests targeting `main`
- signed, packaged builds on tags matching `v*`

The workflow lives at [`.github/workflows/build.yml`](../.github/workflows/build.yml).

## What It Builds

Builds run on:

- macOS
- Linux
- Windows Server 2022

Non-PR builds package:

- macOS: signed and notarized `.dmg`
- Linux: `.AppImage`
- Windows: signed installer and portable `.zip`

Pull requests upload unsigned CI artifacts instead of packaged release files.
No controller plugins are bundled in the release artifacts.

## Required GitHub Secrets

Set these repository secrets before running signed non-PR builds:

- `APPLE_CERTIFICATE_P12`
  Base64-encoded Developer ID Application certificate export (`.p12`).
- `APPLE_CERTIFICATE_PASSWORD`
  Password for the `.p12` certificate.
- `APPLE_ID`
  Apple ID email used for notarization.
- `APPLE_APP_PASSWORD`
  App-specific password for notarization.
- `APPLE_TEAM_ID`
  Apple Developer Team ID.
- `AZURE_CLIENT_SECRET`
  Client secret for Azure Trusted Signing.

## Required GitHub Variables

Set these repository variables for Windows signing:

- `AZURE_TENANT_ID`
- `AZURE_CLIENT_ID`
- `AZURE_SIGNING_ENDPOINT`
- `AZURE_CODE_SIGNING_ACCOUNT_NAME`
- `AZURE_CERT_PROFILE_NAME`

## Trigger Behavior

- `push` to `main`
  Builds, signs/packages, and uploads artifacts for macOS, Linux, and Windows.
- `pull_request` to `main`
  Builds the app on macOS, Linux, and Windows and uploads unsigned CI artifacts.
- tag `vX.Y.Z`
  Builds, signs/packages on all three platforms, and creates a GitHub Release.

GitHub Release creation uses `softprops/action-gh-release` and the job grants
`contents: write` to the default `GITHUB_TOKEN`.

The GitHub Release name and body include a licence-server build identity in the
form `vX.Y.Z-<channel>.<build>`. For a plain `vX.Y.Z` tag, the workflow creates
`vX.Y.Z-public.<GITHUB_RUN_NUMBER>`. Tags may also specify a channel, for
example `v0.1.0-beta`, `v0.1.0-rc`, or `v0.1.0-private`. The build number uses
the GitHub Actions run number unless the tag already includes a full identity
such as `v0.1.0-beta.123`.

## Packaging Notes

- macOS imports `APPLE_CERTIFICATE_P12` into a temporary keychain, signs the
  app bundle and DMG, submits both to Apple notarization, staples the result,
  and validates with `spctl`. App signing applies the
  `com.apple.security.cs.disable-library-validation` entitlement so the
  hardened runtime can load explicitly user-installed unsigned plugins.
- Linux installs build dependencies from `apt`, creates a desktop file and icon,
  then uses `linuxdeploy-x86_64.AppImage`.
- Windows downloads libusb `1.0.30`, stages the VS2022 x64 DLL/import library,
  signs the application executables with Azure Trusted Signing, and packages
  the executable pair with `libusb-1.0.dll`. It also builds and signs an Inno
  Setup installer which installs the Visual C++ x64 runtime when required.

## Release Process

1. Ensure the repository secrets and variables above are configured.
2. Push changes to `main` and confirm the normal `Build` workflow passes.
3. Create and push a version tag:

```bash
git tag v0.1.0
git push origin v0.1.0
```

4. Wait for the tagged `Build` workflow to complete.
5. Verify the GitHub Release contains: `libera-link-macos.dmg`,
   `libera-link-linux.AppImage`, `libera-link-windows-setup.exe`, and
   `libera-link-windows.zip`.
6. Verify the GitHub Release title or body contains the server identity, for
   example `v0.1.0-public.123`.
7. After the licence server webhook imports the release, review `/admin/releases`
   and publish the `LNK01` release when the version, build, access level,
   platform artifacts (including the Windows installer), and sync error are
   correct.

## Notes

- App versioning comes from `git describe --tags --abbrev=0`. If no matching
  tag is available, the version falls back to `0.0.0`.
- The licence server imports `libera-link-macos.dmg`,
  `libera-link-windows-setup.exe`, and `libera-link-linux.AppImage` for app code
  `LNK01`. The portable `libera-link-windows.zip` remains available on GitHub
  Releases. The Windows asset pattern is configured in the licence server's
  `js/app-config.js`; after deploying a pattern change, redeliver the release
  webhook to update existing imported artifacts.
- The workflow uses `fetch-depth: 0` so tags are available during CI.
- Pull requests do not receive signing secrets, so signing, notarization, and
  release packaging only run for non-PR events.
- Local preset verification command:

```bash
cmake --preset release
cmake --build --preset release --parallel
```
