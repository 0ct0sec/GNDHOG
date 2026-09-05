# Publishing GNDHOG ZERO

[Back to the user guide](../README.md). This page describes release work, not
steps an app user needs to perform.

## Store contract

The [CardputerZero Template](https://github.com/CardputerZero/Template) is a
starter, not a required rendering framework. GNDHOG's Make-based native
framebuffer build is submitted as a prebuilt ARM64 Debian package through
[AppBuilder](https://github.com/CardputerZero/AppBuilder).

The authoritative rules are the [manifest schema](https://github.com/CardputerZero/AppBuilder/blob/main/docs/APP_BUILDER_JSON.md)
and the [packages repository validators](https://github.com/CardputerZero/packages/tree/main/.github/scripts).
Check them again for each release; a local validator is a useful guardrail,
not a permanent copy of store policy.

| Item | GNDHOG ZERO |
|------|-------------|
| Package / executable | `bfcli` |
| Display name / author | `GNDHOG ZERO` / `0ct0` |
| Initial version / share code | `0.1.0` / `GNDH` |
| License | MIT |
| Categories | Radio & Comms; Hardware & IoT |
| Screenshots | Six 320×170 PNG software UI previews |
| Store icon | 200×200 PNG |
| Launcher | `/usr/share/APPLaunch/applications/bfcli.desktop` |
| Application | `/opt/bfcli/bin/bfcli`, launched by `/opt/bfcli/run-bfcli` |
| Runtime dependencies | libc6; ALSA and Pulse client libraries for HUD audio |
| Permissions | `imu` and `additional_hardware` |

`additional_hardware` covers the framebuffer, keyboard, backlight, audio
output, serial devices, and the optional EXT power control. `imu` covers the
compass sensors. The app does not request camera, microphone, network,
background-service, or external-display permissions. Meshtastic traffic goes
through the attached serial radio. There is no network client in the app.

The archive contains no system service, package maintainer script, device node,
or setuid/setgid payload. Its launcher runs in the existing user session.
README links, the mascot, the field/build/publishing guides, and the MIT text
are included together under `/usr/share/doc/bfcli`.

## Release checks

1. Review the source diff and tracked files. Keep machine paths, private radio
   transcripts, coordinates from actual use, credentials, and build scratch
   files out of the release.
2. Run `make test` and `make package`. Inspect the `.deb` and run the upstream
   install-path and store-metadata policies as well as `make store-check`.
3. Run `qemu-aarch64 -L /usr/aarch64-linux-gnu build-arm64/bfcli --selftest`.
   Check ELF dependencies against the supported device image. QEMU is not
   evidence of physical USB, radio, keyboard, compass, or display behavior.
4. Refresh the six store screenshots from `--preview` when the UI changes.
   These are software-rendered examples with fixture data. Do not describe
   them as photographs or live hardware captures.
5. Commit, rebuild from the clean commit, and confirm `--version` has no
   `-dirty` suffix. Revalidate that exact archive and record its SHA-256.
6. Push the source commit. Publish the `.deb` and checksum on the source
   repository's GitHub release so users can also install manually.
7. From this repository, use the current publisher:

   ```sh
   /path/to/AppBuilder/czdev login
   /path/to/AppBuilder/czdev publish --deb dist/bfcli_0.1.0_arm64.deb
   ```

The publisher uploads the binary to a release in the contributor's packages
fork, then opens a PR containing metadata and image assets. It checks the
package version and store metadata before upload. Store CI checks ownership,
name/share-code uniqueness, archive layout, and the downloadable artifact.
Do not commit `.deb` binaries to either repository.

## First-release review

The store requires a **short video of the app running on a real Cardputer Zero**
in the submission PR comments. The maintainer watches it and merges the first
release manually. A screenshot slideshow or simulator run is useful supporting
material, but does not satisfy the physical demonstration requirement.

Record launch, navigation, and the features the connected hardware can
actually demonstrate. Keep props off for FC work, use a test mesh conversation,
and keep private messages and real location details out of the footage.

A submitted PR is not an approved listing. Update the README's publication
status only after the approved app appears in the registry. Later releases
must increase the Debian version; do not overwrite an already released build.
