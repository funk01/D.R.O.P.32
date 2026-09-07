<img width="474" height="196" alt="logo" src="https://github.com/user-attachments/assets/225fb772-a6d8-429d-959c-cb191a89b383" />

# D.R.O.P.32

**Wi-Fi in. USB out.**
A Data Relay for Offline PCs — powered by ESP32.

D.R.O.P.32 transfers files from a modern phone, tablet or computer to an offline computer without connecting that destination machine to a LAN or the Internet. An ESP32-S3 receives files through a browser, stores them, and exposes the same files as a USB drive.

Perfect for older 95/XP/2000 or other retro PCs with no protection from modern threats, because they lack updates, but now file transfer from modern PC to retro PC is simple and secure.

For large files and complete folder collections, connect an SD card to the ESP32-S3 and select **SD CARD** on the Dashboard. Internal flash is convenient for smaller transfers; SD storage offers substantially more capacity and is the recommended setup for regular use.

```text
Modern device ── Wi-Fi ──> ESP32-S3 ── Internal FAT or SD ── USB ──> Offline computer
                              D.R.O.P.32
```

The offline computer receives a normal USB mass-storage device. D.R.O.P.32 does not bridge its Wi-Fi connection to USB.

## Screenshots
<img width="1275" height="772" alt="dashboard" src="https://github.com/user-attachments/assets/da35f8d4-ac93-4ef9-b3d4-c4108d73a665" />

<img width="1260" height="751" alt="transfer" src="https://github.com/user-attachments/assets/ea15fc6c-5f4b-4e8c-8ccd-3be7b754db3f" />

## Features

- Drag-and-drop uploads with progress, completion confirmation and visible errors
- Individual files or complete folder structures
- Browser download, rename, delete and folder creation
- Clickable folder navigation
- Internal FAT flash or optional SD-card storage
- Dashboard controls for **WEB TRANSFER** and **USB DRIVE**
- Writable USB Mass Storage for macOS, Windows and offline computers
- Storage usage and approximate write speed in KB/s
- Private setup access point that remains available as a recovery path
- Optional connection to an existing 2.4 GHz Wi-Fi network
- Wi-Fi scan and SSID selection in the web interface
- Browser-based `.bin` firmware updates
- No file-extension allowlist or blocklist

## Recommended hardware

- ESP32-S3 DevKitC-1 N16R8
- USB data cable
- Optional but recommended: SPI SD-card module and FAT-formatted SD card
- Stable USB power supply

### SD-card wiring

| SD module | ESP32-S3 |
|---|---:|
| CS | GPIO 10 |
| MOSI | GPIO 11 |
| SCK | GPIO 12 |
| MISO | GPIO 13 |
| 3V3 | 3.3 V |
| GND | GND |

Insert the SD card before starting D.R.O.P.32. Changing the selected storage does not copy or delete files.

## First installation

Install [PlatformIO Core](https://platformio.org/install/cli), connect the ESP32-S3 by USB and open this project directory:

```bash
pio run
pio run --target upload --upload-port /dev/cu.usbmodem1101
```

On macOS, locate the current port with:

```bash
ls /dev/cu.usbmodem*
```

The project is configured for 16 MB flash, two OTA application slots and an internal FAT transfer partition.

## First connection

Connect a modern device to:

| Setting | Value |
|---|---|
| Wi-Fi | `DROP32-SETUP` |
| Wi-Fi password | `drop32setup` |
| Web interface | `http://192.168.4.1` |
| Web password | `drop32` |

The setup access point remains available even when D.R.O.P.32 is connected to another Wi-Fi network.

> The default credentials are public because this is an open-source project. Use D.R.O.P.32 only on a trusted local network unless you change them in `src/main.cpp`. The web interface uses HTTP, not HTTPS, and must not be exposed to the Internet.

## Transferring files

1. Keep **WEB TRANSFER** selected on the Dashboard.
2. Select **INTERNAL FLASH** or the recommended **SD CARD** storage.
3. Open **TRANSFER** and drop files into the upload area.
4. Wait for `TRANSFER COMPLETE — READY FOR USB`.
5. On the Dashboard, select **USB DRIVE**.
6. Use the mounted D.R.O.P.32 drive on the destination computer.
7. Eject the drive before returning to **WEB TRANSFER**.

Only one side controls the FAT volume at a time. Never disconnect power or switch modes while data is being written.

## Joining an existing Wi-Fi network

Open **NETWORK**, select **SCAN NETWORKS**, choose a 2.4 GHz SSID and enter its password. Once connected, D.R.O.P.32 displays its local IP address and can usually be opened at:

```text
http://drop32.local
```

This lets phones and computers reach D.R.O.P.32 without leaving their normal Wi-Fi network. The USB-connected offline computer still receives no LAN or Internet connection. The ESP32-S3 does not support 5 GHz-only networks.

## Firmware updates

After V1.0.0 has been installed once through USB, later firmware can be installed under **SYSTEM → FIRMWARE UPDATE**.

Build an update with:

```bash
pio run
```

Upload only:

```text
.pio/build/esp32-s3-devkitc-1/firmware.bin
```

Do not upload `bootloader.bin`, `partitions.bin` or a merged factory image through the web updater. Keep the device powered until verification finishes and it restarts. Firmware updates are disabled while **USB DRIVE** owns the storage.

## Project structure

```text
DROP32-V1.0.0/
├── .github/             GitHub workflow and templates
├── assets/              Logo artwork
├── docs/images/         Screenshot placeholders
├── firmware/            Prebuilt OTA firmware.bin
├── include/             Embedded web logo
├── src/main.cpp         Firmware and web interface
├── partitions.csv       16 MB flash layout
└── platformio.ini       PlatformIO configuration
```

## Notes and limitations

- USB and browser access must never modify the selected storage simultaneously.
- USB write-speed figures include FAT metadata and are approximate.
- Empty folders are not included by browser folder selection.
- The hostname `drop32.local` depends on mDNS support in the client network.
- Test important workflows with disposable files before relying on any embedded storage device for unique data.

## License

This project is released under the [MIT License](LICENSE).
