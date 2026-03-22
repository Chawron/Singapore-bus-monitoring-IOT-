# 🚌 BusMonitor (ESP32-C6 + Waveshare LCD 1.47")

✨ End-user and deployment guide for `busmon2`.

This firmware runs on `ESP32-C6-DevKitM-1` with Waveshare 1.47" LCD hardware and provides:
- ⏱️ Bus ETA monitor (LTA DataMall v3)
- 🔁 Multi-stop rotation (up to 4 stops)
- 🎯 Optional service filtering per stop (or all services)
- 📶 Setup AP + web setup page
- 🚨 On-screen error codes (`E_WIFI_*`, `E_STOP_*`, `E_BUS_*`, `E_WTH_*`, `E_MKT_*`)
- 🌦️ Weather + market side pages

## 1) 🧩 Hardware and Environment

- 🧠 Board: `Espressif ESP32-C6-DevKitM-1`
- 🛠️ Framework: Arduino (PlatformIO)
- 💾 Flash size: 8MB detected on board
- 🖥️ Display resolution: `172 x 320` (landscape UI)
- 📡 Setup AP:
  - SSID: `BusMonitor-Setup`
  - Password: none
  - Setup URL: `http://192.168.4.1`

## 2) 🔐 Register LTA DataMall and Get Account Key

This firmware needs an LTA DataMall Account Key for bus APIs.

1. Open DataMall home: `https://datamall.lta.gov.sg/content/datamall/en.html`
2. Click `Request for API Access`.
3. Complete the request form:
   - Part I: applicant info
   - Part II: data usage
   - Part III: terms acceptance
4. Submit the form.
5. Obtain your Account Key from LTA DataMall subscription flow (use that key in setup page).

Important:
- 🔒 Keep the Account Key private.
- ⏳ If access is not working yet, wait a while and retry, then verify with a direct API test.

Quick API test example (PowerShell):

```powershell
$key = "YOUR_ACCOUNT_KEY"
$url = "https://datamall2.mytransport.sg/ltaodataservice/BusArrivalv3?BusStopCode=83139"
Invoke-WebRequest -Uri $url -Headers @{ AccountKey = $key; accept = "application/json" } | Select-Object -Expand Content
```

✅ If this returns JSON, your key is ready.

## 3) ⚙️ Build and Flash Firmware

From project root:

```powershell
pio run -e esp32-c6-devkitm-1
pio run -e esp32-c6-devkitm-1 -t upload --upload-port COM8
```

If Windows console shows Unicode encoding errors during upload, use:

```powershell
chcp 65001 > $null
$env:PYTHONUTF8 = "1"
$env:PYTHONIOENCODING = "utf-8"
pio run -e esp32-c6-devkitm-1 -t upload --upload-port COM8
```

## 4) 📱 First-Time Setup (From Blank Device to Running)

1. 🔌 Power on device.
2. 🟢 Boot splash shows `Softinex.com`.
3. 🧭 Device enters Setup Mode if config is missing or Wi-Fi cannot connect.
4. 🧾 Setup screen has 2 pages:
   - Page 1: QR to join `BusMonitor-Setup` Wi-Fi
   - Page 2: QR to open `http://192.168.4.1`
5. 👆 Short press `BOOT` button to switch setup pages.
6. On phone/laptop:
   - 📶 Connect to Wi-Fi `BusMonitor-Setup`
   - 🌐 Open browser to `http://192.168.4.1` (or scan page-2 QR)
7. Fill setup form:
   - 📡 Home Wi-Fi SSID and password
   - 🔑 LTA DataMall Account Key
   - 🚏 Bus stops (1 to 4 stop codes)
   - 🧮 Bus service filter per stop (optional)
8. 💾 Click `Save & Restart Device`.
9. 🔄 Device reboots and runs monitor pages.

Notes:
- 🟡 Leave service filter empty to monitor all services at that stop.
- 🤖 Stop name entry is not required; firmware auto-fetches from API and falls back safely.

## 5) 🖥️ Normal Runtime Behavior

- 🚌 Bus page is primary screen.
- 🔁 If multiple stops are saved, active stop rotates every `30s`.
- 🌤️ Weather and market pages appear every `5 min` for `10s` each.
- Poll intervals:
  - Bus: `20s`
  - Weather: `10 min`
  - Market: `10 min`

Thermal optimizations already enabled:
- 🧊 CPU capped to 80MHz
- 🔋 Wi-Fi power save enabled after connect
- 💡 Backlight PWM dimming in run/setup/idle modes

## 6) ♻️ Factory Reset

To clear saved configuration:

1. ✋ Hold `BOOT` for about `5s`.
2. 📊 A factory reset progress screen appears.
3. 🧹 Device clears NVS config and restarts.

This works both at boot and during runtime.

## 7) 🚨 Error Codes on Screen

If something fails, an error code is shown on LCD.

Common codes:
- `E_CFG_00` Missing saved config
- `E_CFG_01` Wi-Fi SSID missing
- `E_WIFI_01` Cannot connect to Wi-Fi
- `E_WIFI_02` Wi-Fi disconnected after running
- `E_STOP_00` Missing stop/API key
- `E_STOP_01` Stop-name HTTP start failed
- `E_STOP_429` Stop-name API rate limited
- `E_BUS_00` Bus API HTTP start failed
- `E_BUS_01` Bus JSON parse failed
- `E_BUS_02` No services found for selected filter/stop
- `E_BUS_429` Bus API rate limited
- `E_WTH_*` Weather API error family
- `E_MKT_*` Market API error family

Tip:
- ℹ️ `E_STOP_02` (parse warning) is treated as non-fatal in current firmware; display should continue with fallback stop name.

## 8) 🌍 Data Sources

- LTA DataMall:
  - `BusArrivalv3`
  - `BusStops`
- Weather: Singapore government weather endpoint used by firmware
- Market:
  - BTC from CoinGecko
  - Gold from Gold API (with fallback source)

## 9) 📁 Project Files

- Firmware: `src/main.cpp`
- Setup web page (embedded in firmware): `setup.html`
- Build config: `platformio.ini`
