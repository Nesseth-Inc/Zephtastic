# ZephTastic

ZephTastic is Zephyr firmware for LoRa mesh companions and routers. Project release **0.1-beta** targets the Meshtastic 2.8.2 phone and radio protocols at upstream tag `v2.8.2.d5f1619`. This independent beta offers USB-installable builds for Heltec T114 V2, Heltec T096 and RAK4631. The source also supports the XIAO ESP32-S3 with Wio-SX1262 and experimental Heltec V4 receive-only profiles. The 2.8.1 tag remains the regression baseline. [Beta test results and known issues](mt_only/docs/XIAO-BETA.md)

The ordinary XIAO companion has 16 trusted private contacts. The separately signed expanded companion supports 198 after the owner explicitly enables it over USB. Both support USB, Bluetooth LE and Wi-Fi. Browser messaging uses the device's existing access controls. Device and file administration stay local. ZT suppresses relaying Position packets that exceed the channel's precision setting. Hardware support for a handset, relay topology or accessory requires tests on that combination.

## 0.1-beta board downloads

Download the board ZIP and `SHA256SUMS` from the [0.1-beta release](https://github.com/Nesseth-Inc/zephtastic/releases/tag/0.1-beta). Extract the ZIP for your board and read its installation instructions before flashing.

| Board and download | Installation and hardware evidence |
| --- | --- |
| **Heltec T114 V2** · `zephtastic-0.1-beta-heltec-t114-v2-test.zip` | Application-only UF2 for the V2. An earlier image booted and completed three USB configuration exchanges and a BLE exchange on retry. The downloadable build selects the corrected model **69**, verified offline. |
| **Heltec T096** · `zephtastic-0.1-beta-heltec-t096-test.zip` | Start with the **680 KiB fresh-install** UF2. The extended 728 KiB image leaves 1,960 bytes of application flash and 3,300 bytes of static RAM. Run the matching layout check on a verified 1 MiB readback before installing either. An earlier image completed three USB configuration exchanges. BLE advertised, then disconnected before configuration completed. |
| **Heltec WiFi LoRa 32 V4** · `zephtastic-0.1-beta-heltec-v4-build-sources.zip` (build kit) | The tested build configurations cover V4.2 R2, V4.3 R2 and V4.3 R8. LoRa is **receive-only** pending transmit-power calibration for each amplifier. Build and image checks passed offline. Hardware testing is open. |
| **RAK4631 WisBlock** · `zephtastic-0.1-beta-rak4631-rak19003-mini-firmware.zip` | Application-only UF2 for the **RAK19003 Mini Base** with S140 6.1.1 and no optional RAK12002 RTC. Build and image checks passed offline. Hardware testing is open. |

All four Nordic images in the three public UF2 packages were verified offline. Prebuilt ESP32-S3 images for the XIAO and Heltec V4 are held pending resolution of the corresponding-source issue for their linked Espressif radio libraries. The earlier T114 and T096 bench images kept settings in RAM. The exact downloads still need tests for boot, settings retention after a power cycle, and over-the-air reception. When reporting results, include your board revision or WisBlock base, image filename and SHA-256, and the checks that passed or failed. [Report a test result or issue](https://github.com/Nesseth-Inc/zephtastic/issues)

The release includes the project source, the Nordic dependency and notices archive, and the protocol source archive with the pinned Meshtastic schemas and generation instructions. Use them alongside the recipe in each board ZIP. The Heltec V4 build kit supplies its board definitions and USB receive-only configuration. The XIAO build instructions are below.

## Companion and router roles

Use the image for your board. The TX-capable builds share one routing implementation across roles. A fresh configuration starts as CLIENT_MUTE, which suppresses forwarding. Use the phone to select CLIENT for ordinary rebroadcast, or ROUTER or ROUTER_LATE for a dedicated routing node. The T114 V2 and RAK4631 builds also start with transmission disabled. Both Meshtastic 2.8.2 and this firmware map the retired REPEATER setting to CLIENT. Forwarding also requires an appropriate region, enabled transmission and a rebroadcast setting that permits the packet. The receive-only Heltec V4 profiles cannot forward packets. Physical three-node relay testing is still open.

On a fresh T096, complete the [USB identity enrollment](mt_only/docs/T096-FRESH-DOMAIN.md#first-enrollment) before testing the radio. The two T096 downloads are feature and storage profiles. The 680 KiB companion leaves 43,960 bytes of flash and 14,304 bytes of static RAM. The 728 KiB extended build leaves 1,960 and 3,300 bytes respectively. Changing the role does not free either resource. A dedicated router build could omit phone or display features at compile time. This release ships the two companion-capable T096 profiles.

## Board identity for source builds

ZT selects the Meshtastic hardware model from the Zephyr board at build time. For example, a Heltec T114 V2 reports 69, a T096 reports 127, and the two Heltec V4 revisions share 110. The V4 R8 reports 132. The phone API, radio announcements and MQTT map reports use the same selection. [Automatic board mappings](mt_only/src/BoardIdentity.h)

For a carrier or board revision that Zephyr cannot distinguish, set `CONFIG_MT_HARDWARE_MODEL` in the board's application configuration. For example, `CONFIG_MT_HARDWARE_MODEL=10` selects a verified Heltec LoRa 32 V2.1. Zero selects automatic detection, and an unidentified board reports `UNSET`. Private ports can use 255. Unassigned numbers and the vendor placeholders 28 and 104 fail the build. Drivers and radio settings still follow the physical board. The device declines client-initiated firmware updates when the selected model differs from the physical board or the build has no matching updater target. [Meshtastic 2.8.2 model numbers](https://github.com/meshtastic/protobufs/blob/8d0ed0c135f91ad1d6f9c60bc341277d64946bce/meshtastic/mesh.proto#L221)

## XIAO build and update compatibility

The privately qualified signed XIAO firmware reports **2.8.2+136**. The factory image is for a backed-up XIAO ESP32-S3 with Wio-SX1262 and 8 MiB flash. It replaces the bootloader, application and settings. A device already enrolled in this ordinary release signing series can install a newer signed application over BLE, Wi-Fi or USB. Stock firmware and development previews use other signing authorities and require the factory migration procedure. [XIAO installation and recovery](mt_only/docs/XIAO-UPDATES.md) · [Stock identity migration](mt_only/docs/STOCK-IDENTITY-IMPORT.md)

The ordinary series uses a P-256 verification key with SHA-256 SPKI fingerprint `0cc69ef819b9dcd74319724a023c244b0063e004938c0e45889cfd48e42dc074`. Use this public key and the recorded build timestamp to reproduce the release's unsigned application and bootloader. [Ordinary release public key](mt_only/boards/keys/xiao-ordinary-release-v1.pem)

## Build the XIAO from source

Start from a clean Git checkout. Use a directory outside it for downloads, keys and build output. Install Git, CMake, Ninja, devicetree compiler, OpenSSL, Python 3.12 or newer with venv, and the Zephyr SDK **1.0.1** with the Xtensa ESP32-S3 toolchain. Point `ZEPHYR_SDK_INSTALL_DIR` at that SDK and `ZT_WORK` at your external work directory.

```sh
: "${ZT_WORK:?Set ZT_WORK to a new directory outside the checkout}"
: "${ZEPHYR_SDK_INSTALL_DIR:?Set ZEPHYR_SDK_INSTALL_DIR to the installed SDK}"
mkdir -p "$ZT_WORK"
python3.12 -m venv "$ZT_WORK/venv"
. "$ZT_WORK/venv/bin/activate"
python mt_only/scripts/prepare-deps.py "$ZT_WORK/deps"
export ZEPHYR_BASE="$ZT_WORK/deps/zephyr"
export ZT_MODULES="$ZT_WORK/deps/modules"
export MCUBOOT="$ZT_WORK/mcuboot"
git init -q "$MCUBOOT"
git -C "$MCUBOOT" fetch --depth 1 https://github.com/mcu-tools/mcuboot aa32eaaad41cc345e6f6e6633368a4766fe23c25
git -C "$MCUBOOT" checkout --detach FETCH_HEAD
python -m pip install -r "$ZEPHYR_BASE/scripts/requirements-base.txt" \
    -r "$ZT_MODULES/hal/espressif/zephyr/requirements.txt" "$MCUBOOT/scripts"
export SOURCE_DATE_EPOCH=1795713761  # Fixed for the 0.1-beta release series
```

The dependency lock specifies each source revision, overlay and downloaded vendor blob. The MCUboot revision above matches the pinned Zephyr manifest. The release builder embeds a P-256 **public** verification key. Use a disposable local key for a development build. Keep private signing material outside the checkout and package.

```sh
(umask 077 && test ! -e "$ZT_WORK/dev-signer.pem" && openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$ZT_WORK/dev-signer.pem")
openssl pkey -in "$ZT_WORK/dev-signer.pem" -pubout -out "$ZT_WORK/dev-public.pem"
python mt_only/scripts/xiao_release.py build \
    --zephyr "$ZEPHYR_BASE" --modules "$ZT_MODULES" --mcuboot "$MCUBOOT" \
    --sdk "$ZEPHYR_SDK_INSTALL_DIR" --source-date-epoch "$SOURCE_DATE_EPOCH" \
    --public-key "$ZT_WORK/dev-public.pem" --build "$ZT_WORK/build-ordinary"
```

For a byte comparison with an earlier build, supply the same SDK, public key and recorded source date epoch. Compare the unsigned application and bootloader before packaging the signed image.

The builder checks the resolved board configuration, flash layout, and public key in the application and bootloader. For the expanded build, use its own P-256 key, a new output directory, and add `--xiao-expanded-contacts --ordinary-public-key "$ZT_WORK/dev-public.pem"`. Use a new work directory for a new signer. The factory image replaces all 8 MiB of flash. Save and verify the device's recovery image before an installation. [Migrating a stock node identity](mt_only/docs/STOCK-IDENTITY-IMPORT.md) · [Signing, update files and recovery](mt_only/docs/XIAO-UPDATES.md) · [198-key activation and recovery limits](mt_only/docs/XIAO-EXPANDED-CONTACTS.md)

## Compatibility checks

The private-runner firmware checks exercise pinned upstream tests, the stock Python phone client and the supported board builds. Each completed group records the commit, test results or board memory measurements. [Tests, coverage and module priorities](mt_only/docs/UPSTREAM-QUALIFICATION.md)

For the module inventory check, use an upstream Meshtastic firmware checkout containing the two pinned commits. Run from the preview root with `STOCK` pointing to that checkout.

```sh
python mt_only/scripts/check-upstream-module-inventory.py --stock "$STOCK" --version 2.8.1
python mt_only/scripts/check-upstream-module-inventory.py --stock "$STOCK" --version 2.8.2
```

The firmware comparison uses stock commit `d5f16191ca28b01b5024f590f04a67f25d66f1b1` and protobufs `8d0ed0c135f91ad1d6f9c60bc341277d64946bce`. The earlier regression fixtures use firmware `8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c`. The XIAO update paths have bench coverage. Physical handset updates, sustained mixed-version radio traffic, and a real three-node relay with the direct path blocked remain qualification work. [Upstream firmware changes](https://github.com/meshtastic/firmware/compare/v2.8.1.8e6a88d...v2.8.2.d5f1619) · [Module coverage](mt_only/docs/UPSTREAM-MODULE-INVENTORY.md)

## Source and licenses

The application, board profiles, pinned dependency overlays, host tests and release tools are under `mt_only/`. Platform adapters, board definitions, cryptography and stock comparison sources are under `zephcore/`. Component notices accompany their sources. MIT covers original project-owned ZephTastic material without another applicable license. File and component licenses take precedence, including GPL-3.0-only on the application code marked with that identifier. Include the copyright and license notice when redistributing copies or substantial portions. ZephCore and other upstream authors retain their copyrights and license terms. The incorporated Meshtastic firmware code is GPLv3. Distributing a combined firmware image that includes it requires meeting the GPLv3 terms for the combined work, including providing its corresponding source. Each board package identifies the additional sources and notices for that build. [Project license](LICENSE) · [Meshtastic donor license](zephcore/bench/upstream-differential/source/LICENSE) · [ZephCore notice](zephcore/helpers/LICENSE.ZephCore) · [Sensor library notices](mt_only/docs/licenses)
