# PS4 HDD Tools

**English** | [Русский](docs/README_ru.md)

Check the health of your PS4 drive **without taking the console apart**. No need to pull the drive out, connect it to a PC and run a disk utility: send one payload to the console and open a web page from your phone or computer.

PS4 HDD Tools shows the drive's SMART status and scans its surface for read errors. The page is served by the console itself on your home network.

Version **1.2.0.11** · License **GPLv3** · Created by SergioPoverony and Mr.Claude

## Features
- Shows every drive: internal HDD/SSD, USB drives and extended storage. Each drive gets a tab with its name and a quick verdict: **Healthy**, **Caution** or **Bad**.
- Detailed SMART: model, firmware, serial number, temperature, power-on hours, power cycles and a full attribute table with thresholds. The verdict is based on critical attributes (reallocated and pending sectors, temperature, and so on).
- Surface scan: the whole drive or a range in GB. Before the scan starts, the speed is measured and the expected duration is shown; nothing starts without your confirmation.
- Pause, resume and stop. A block map colours every block by read time and marks read errors in red. Below it: statistics, a list of problem sectors and a log.
- The console does not go to sleep while a scan is running, even if you do not touch the controller.
- Russian and English interface, dark theme, mobile-friendly layout.

## Good to know
- Only **read** commands are sent to the drive. Nothing is ever written or erased.
- When idle, the payload does nothing. Drives are read when you open the page or press *Refresh disks*.
- A surface scan puts heavy load on the drive; games and menus may lag while it runs.
- If a USB enclosure does not pass SMART through, the drive is still listed and marked *SMART unavailable*; the surface scan still works for it.

## Usage
1. Send `ps4hddtools.elf` to the console with BinLoader (port 9090), or add it to the GoldHEN autoload.
2. Open `http://<console IP>:8686/` in a browser.
3. Pick a drive, optionally set a range, and press **Start**.

Running the payload again asks the old copy to shut down, so only one copy is ever active. The **Shut down** button is in the menu (the three-bar button in the top right corner).

## Tested on
Firmware 13.52, GoldHEN 2.4b18. Tested with an internal Seagate drive behind the USB-SATA bridge, an Apacer SSD connected directly over SATA, a Toshiba USB drive and a USB flash drive. Other firmware versions and drives are not guaranteed to work. If a drive is detected incorrectly, please open an issue and attach the log (the *Log* section of the page).

## Building
```
export PS4_PAYLOAD_SDK=/path/to/ps4-payload-sdk
make                          # build/ps4hddtools.elf
make send PS4_HOST=192.168.0.24
```
The SDK is [ps4-payload-dev/sdk](https://github.com/ps4-payload-dev/sdk). The version number lives in one place: `src/version.h`.

### Tests on a PC
```
python3 tools/embed.py build/assets_gen.h
g++ -std=c++17 -g -pthread -fsanitize=address,undefined -DHOST_TEST \
    -DDEV_DIR='"/tmp/hddtest/dev"' -DDATA_DIR='"/tmp/hddtest/data"' \
    -Isrc -Ibuild -o build/host_test src/*.cpp
python3 tests/test_api.py build/host_test
```

## Support
If the project is useful to you, there is a *Support* button in the menu of the web page.

## License
GPLv3. Created by SergioPoverony and Mr.Claude.
