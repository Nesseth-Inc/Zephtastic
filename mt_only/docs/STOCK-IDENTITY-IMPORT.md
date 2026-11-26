# Moving a stock Meshtastic identity to the ordinary XIAO companion

This opt-in migration is for the XIAO ESP32-S3 16-contact companion. It keeps the stock node number and public key so existing peers can recognize the node after a whole-chip flash. The previous installation must stop transmitting under that key. A copy of a key or a flash snapshot on a second active radio can defeat the protections described below.

## Before flashing

1. Connect stock Meshtastic Android to the local stock radio using USB or an authenticated Bluetooth link with a private PIN. In **Security**, use **Backup Keys**. Record the node number and public key separately so you can check which identity the app offers after flashing. Use the dedicated key backup even if you also export a configuration file. Keep the phone and its app data available through the migration.
2. For names and channels, use **Settings → Backup & Restore → Export configuration**. Treat the exported file as sensitive. Android may include channel credentials and other values held in its cache. Its app-data backup holds the phone’s database and serves a different purpose. [Stock Android 2.8.4 key backup and restore](https://github.com/meshtastic/Meshtastic-Android/blob/927b38549110091fd7051daa24cc77150fd45849/feature/settings/src/commonMain/kotlin/org/meshtastic/feature/settings/radio/RadioConfigViewModel.kt#L767-L829)
3. Verify a recovery image of the original board. A factory flash overwrites all 8 MiB, including identity, settings and files. [XIAO installation and recovery](XIAO-UPDATES.md)

## Restoring the key

1. Install the ordinary ZT factory image on the XIAO and leave its private identity unenrolled. Attach its USB port to your computer. Configure a unique six-digit fixed Bluetooth PIN on the XIAO through the local USB settings. Remove any old bond and pair Android with that PIN. The firmware requires an authenticated BLE link and rejects the generic default PIN and Just Works pairing. Leave radio transmission disarmed during enrollment.
2. On a Linux or macOS computer, install the Python `meshtastic` package. The USB helper preserves the XIAO’s DTR and RTS modem-control lines so opening the port keeps the Android connection active. Run the USB confirmation helper with the **original public key** copied from stock Android. The helper accepts base64 or hex. It addresses the local XIAO directly and checks the profile before opening the restore window.

   ```sh
   python3 mt_only/scripts/xiao_identity_import.py --port /dev/ttyACM0 --public-key 'PUBLIC_KEY_FROM_STOCK'
   ```

3. When the helper opens the window, choose **Security → Restore Keys** in the stock Android app and select the saved source node. The radio stages that keypair in RAM. The USB helper confirms only when the staged public key and its node number agree with the expected identity. The window expires after 90 seconds. The helper keeps the one-time challenge in memory.
4. Leave USB attached through the reboot. The helper checks that the same physical XIAO restarted with the expected node number. It reads the full public key from both local NodeInfo and Security configuration before reporting success. Reconnect Android. Use the local owner interface to trust the intended peer’s public key before sending a private DM, then check delivery at the recipient.

A USB reply can be lost during reboot. The helper sends confirmation once, then uses read-only handshakes to determine the outcome. If it cannot verify the new boot, check the radio’s local public key before another attempt.

If power fails during confirmation, first reconnect normally and check whether the identity already committed. If startup holds an interrupted import, run the same USB helper with `--resume` and the original public key. It checks the saved intent and completes the previously authorized writes. Android does not need to send the private key again. The restricted recovery handshake omits the original boot counter and silicon identifier. After resume, verify the reported local public key independently, including if its acknowledgement is lost. An incompatible or damaged storage record needs recovery from your verified original installation.

## Tested migration

Stock Android 2.8.4 also restored a stock `2.8.2.d5f1619` key to the ordinary XIAO preview. Updating that installation over BLE preserved the node number, public key and restored secondary channel. Earlier preview builds could finish Android's profile import while silently retaining the old owner names, Unmessageable setting, LoRa region and hop limit. The beta candidate handles the stock owner's unchanged identity fields and the Wio-SX1262's “external amplifier not present” setting. After a profile import, reboot and reread the values on the device. [Candidate qualification and known issues](XIAO-BETA.md)

Stock Android 2.8.4 backed up a key on an XIAO ESP32-S3 running stock firmware `2.8.1.8e6a88d` and restored it over authenticated BLE after a whole-chip flash to the ordinary ZT `2.8.2+123` preview. The receiving Nordic node decrypted a private LoRa message from that identity before and after the migration. The restored XIAO received the outgoing test message over USB. A second full installation with TX disabled verified the helper’s commit receipt and the full public key after reboot.

On the ordinary `2.8.2+127` preview, stock Android 2.8.4 automatically reconnected over BLE after **Restore Keys** and after three further XIAO restarts. Each time the app reused its existing bond and downloaded both configuration and NodeInfo. If Android was already retrying a connection when you installed the fix, close and reopen the app once with the XIAO still selected. That recovered the previously failing bond in the bench test.

After trusting the recipient on the XIAO, open the peer from **Nodes**, choose **Message**, and check for **Public Key Encryption** before sending. In the test, reopening Android once let it refresh the newly added contact. The app then sent a private message over BLE using the restored stock identity. The receiving radio decrypted the LoRa packet, and its packet ID matched the one recorded by Android.

## What changes for encrypted traffic

A stock Android key backup contains the keypair. It carries no record of previously used encryption nonces or packets received by the old radio. A nonce is a value that must be unique each time a given encryption key is used. Importing a key starts a new local record of usage. ZT cannot prove that a new nonce was never used before the migration, or use the backup to establish the freshness of an old inbound message. Re-enroll only trusted peer keys. [Stock Android key import](https://github.com/meshtastic/Meshtastic-Android/blob/927b38549110091fd7051daa24cc77150fd45849/core/domain/src/commonMain/kotlin/org/meshtastic/core/domain/usecase/settings/ImportSecurityConfigUseCase.kt#L34-L45)

For private DMs, stock puts the sender, packet ID and a 32-bit extra value into the AES-CCM nonce. The shared encryption key depends on the sending and receiving keypairs. On an imported identity, ZT reserves counters in persistent storage and permutes them using AES-256 FF1 with a fresh per-installation key. A reboot skips any unused reservations. The permutation gives each counter a different extra value until exhaustion, including when the phone repeats a packet ID. A new local enrollment continues to use its existing counter policy. [Stock encryption](https://github.com/meshtastic/firmware/blob/d5f16191ca28b01b5024f590f04a67f25d66f1b1/src/mesh/CryptoEngine.cpp#L297-L323) · [NIST FF1 specification](https://doi.org/10.6028/NIST.SP.800-38G)

| Overlap with traffic before the flash | Chance of reusing a historical private nonce |
| --- | --- |
| One old send and one new send to the same peer with the same packet ID | About 1 in 4.29 billion, assuming fresh import randomness |
| 10,000 old and 10,000 new sends to the same peer, all with one packet ID | At most about 2.33% using a conservative bound |
| Private sends confined to the new installation and its intact persistent state | Each new reservation produces a distinct extra value, including across ordinary reboots |

For fixed histories, sum the product of old and new send counts for each matching peer and packet ID, then divide by the roughly 2³² available extra values. This upper bound can overcount collisions. Under adaptive choices, condition on the remaining unused values of the permutation. The phone supplies no counts for the old installation. Each further import introduces another boundary. Rolling back active state or running a clone falls outside these probability estimates. A whole-chip erase also removes ZT’s record that an identity was retired. Restoring an older full-chip image can roll back both the retirement record and used counters. Retire the old installation before using the restored key.

Shared-channel encryption has a different limitation. Its nonce has no private-DM extra value. With the same sender, channel key and packet ID, an old encryption stream can repeat with certainty. The imported installation blocks new transmissions under confidential shared keys, including automatic ones. It can still receive or relay eligible packets without re-encrypting them. After local signing is ready and the owner enables radio transmission, the published default channel and licensed plaintext can originate messages even with an empty trust list. If a private DM would use a confidential channel to return its delivery acknowledgment, that acknowledgment is also suppressed. Verify receipt at the peer. Use a fresh local identity for normal confidential shared-channel transmission.
