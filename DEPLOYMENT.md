# Smart Bin Reward System - Deployment Guide

## ✅ Completed Steps

### 1. GitHub Repository
✅ **Repository Created & Pushed**: https://github.com/anvitraut1-eng/smart-bin-reward-system

All code is committed and pushed to the `main` branch:
- `smart_bin_esp32.ino` - Complete ESP32 firmware
- `schema.sql` - Supabase schema with triggers and RLS
- `pwa/` - React PWA with all components

### 2. Project Structure
```
smart-bin-reward-system/
├── smart_bin_esp32.ino       # ESP32 firmware (Arduino IDE)
├── schema.sql                 # Supabase database schema
├── README.md                  # Full documentation
└── pwa/                       # Progressive Web App
    ├── src/
    │   ├── components/        # React components
    │   ├── lib/supabase.js    # Supabase client (configured)
    │   ├── App.jsx            # Main app
    │   └── App.css            # Styling
    ├── public/
    │   ├── manifest.json      # PWA manifest
    │   └── sw.js              # Service worker
    └── vercel.json            # Vercel config

```

### 3. Configuration (Pre-configured)
✅ WiFi credentials, Supabase URL, and anon key are already set in:
- `smart_bin_esp32.ino` (lines 20-26)
- `pwa/src/lib/supabase.js` (lines 3-4)

## 🚀 Next Steps: Vercel Deployment

### Option A: Deploy via Vercel Dashboard (Recommended)

1. **Go to Vercel Dashboard**: https://vercel.com/new
2. **Import Git Repository**:
   - Select "Import Git Repository"
   - Connect to GitHub (if not already)
   - Search for: `anvitraut1-eng/smart-bin-reward-system`
   - Click "Import"

3. **Configure Project**:
   - **Project Name**: `smart-bin-reward-system`
   - **Framework Preset**: `Vite`
   - **Root Directory**: `pwa` (⚠️ IMPORTANT - set this!)
   - **Build Command**: `npm run build` (default)
   - **Output Directory**: `dist` (default)

4. **Deploy**:
   - Click "Deploy"
   - Wait for build to complete (~2-3 minutes)
   - Your live URL will be: `https://smart-bin-reward-system.vercel.app` (or similar)

### Option B: Deploy via Vercel CLI

```bash
cd pwa
npm install -g vercel
vercel login
vercel --prod
```

Follow the prompts:
- Set up and deploy? **Y**
- Which scope? Select your account
- Link to existing project? **N**
- Project name? `smart-bin-reward-system`
- Directory? `./` (you're already in pwa/)
- Override settings? **N**

## 📋 Remaining Setup Steps

### 1. Deploy Supabase Schema

1. Go to your Supabase project: https://supabase.com/dashboard
2. Navigate to: **SQL Editor**
3. Create a new query
4. Copy/paste entire contents of `schema.sql`
5. Click "Run" (⚠️ This creates all tables, triggers, and RLS policies)

### 2. Upload Firmware to ESP32

#### Required Arduino Libraries:
- WiFi (built-in)
- HTTPClient (built-in)
- Preferences (built-in)
- SPI (built-in)
- **MFRC522** (install via Library Manager)

#### Upload Steps:
1. Open Arduino IDE
2. Install ESP32 board package (if not already):
   - File → Preferences → Additional Board URLs
   - Add: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   - Tools → Board → Boards Manager → Search "ESP32" → Install

3. Open `smart_bin_esp32.ino`
4. Update `DEVICE_ID` on line 23 (e.g., "BIN_ESP32_001", "BIN_ESP32_002", etc.)
5. Select:
   - **Board**: "ESP32 Dev Module"
   - **Port**: (your ESP32's COM port)
6. Click "Upload"

#### Hardware Wiring:
```
ESP32 Pin Assignments:
- GPIO 5  → HC-SR04 Trig
- GPIO 18 → HC-SR04 Echo
- GPIO 19 → SW-420 Vibration sensor (D0)
- GPIO 21 → Calibration button (to GND)

RC522 RFID (SPI):
- GPIO 15 → SDA/SS
- GPIO 18 → SCK
- GPIO 23 → MOSI
- GPIO 19 → MISO
- GPIO 22 → RST
- 3.3V    → VCC
- GND     → GND
```

### 3. First Boot Calibration

1. Power on ESP32
2. Open Serial Monitor (115200 baud)
3. Wait for WiFi connection
4. **Ensure bin is EMPTY**
5. **Press calibration button** (GPIO 21 to GND)
6. Baseline distance is saved to flash (persists across reboots)

## 🧪 Testing the System

### Test Bin Monitoring
1. Open the PWA in your browser (Vercel URL)
2. You should see your bin(s) appear in the Fleet View
3. Fill readings update every 30 seconds
4. Add some weight to trigger fill % change

### Test Empty Detection
1. Shake/move the bin vigorously (3+ seconds)
2. Wait 3 seconds for quiet period
3. System settles for 5 seconds
4. Check "Bin Detail" view for empty event log

### Test Reward Flow
1. Tap an RFID card on RC522 reader
2. Within 10 seconds, drop something in the bin
3. System detects fill rise + vibration
4. Go to "Citizen Lookup" → Enter card UID → See points balance
5. Points are auto-awarded (10 pts per disposal)

### Test Redemption
1. Go to "Redeem Points" view
2. Enter card UID → Check balance
3. Enter points to redeem
4. System generates 8-character code
5. Code displayed for manual shop validation

### Test Admin Audit
1. Go to "Admin Audit" view
2. See all reward events (confirmed / no_disposal / rate_limited)
3. Filter by confidence level
4. Monitor for fraud patterns

## ⚠️ Important Production Notes

### v1 Placeholder Rules (CONFIRM BEFORE PRODUCTION):
- **Points per disposal**: 10 points (line 37 in firmware)
- **Rate limit**: 5 minutes same card/bin (line 38)
- **Disposal window**: 10 seconds after tap (line 36)
- **Min fill rise**: 2% (line 39)

### Security Warnings:
This v1 uses **anon key access** for exhibition/demo purposes.

**Before municipal production rollout**:
1. Replace anon key with service role key for firmware
2. Implement citizen authentication (not just card UID)
3. Restrict redemption to authenticated admins
4. Add fraud detection algorithms
5. Geofence reward events to registered device locations
6. Encrypt PII (citizen names, contact info)
7. Add audit logging for all point deductions

### Scaling Considerations:
- For >100 bins: partition `bin_readings` by month
- For >1000 PWA clients: switch from Realtime to polling
- Add materialized views for dashboard aggregations
- Archive data >90 days to cold storage

## 📊 Expected Behavior

### Normal Operation:
- Bin posts reading every 30 seconds
- PWA updates in real-time via Supabase Realtime
- Push notification when bin crosses 80% fill
- Empty detection logs vibration + fill drop
- RFID tap → 10s window → disposal detection → points awarded

### Offline Behavior:
- Up to 20 events buffered in ESP32 RAM
- Auto-flush when WiFi reconnects
- No data loss during brief outages

### Rate Limiting:
- Same card at same bin within 5 min → no points
- Logged as `rate_limited` in admin audit
- Prevents point-farming attacks

## 🔗 URLs Summary

- **GitHub Repo**: https://github.com/anvitraut1-eng/smart-bin-reward-system
- **Vercel PWA**: `https://smart-bin-reward-system.vercel.app` (after deployment)
- **Supabase Project**: https://bpecehlmvzuirxmruvyt.supabase.co

## 📞 Support

For issues or questions, open an issue on GitHub:
https://github.com/anvitraut1-eng/smart-bin-reward-system/issues

---

**Build completed**: 2026-08-22
**Status**: ✅ Code ready, ⏳ Awaiting Vercel + Supabase deployment