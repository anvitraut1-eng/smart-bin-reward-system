# Smart Bin Municipal Monitoring + Citizen Reward System

A complete IoT solution for municipal smart bin management with RFID-based citizen rewards.

## ⚠️ Important Notes

**v1 Placeholder Rules (confirm before production):**
- **Points per disposal**: 10 points (configurable constant in firmware)
- **Rate limit**: 5 minutes (same card, same bin)
- **Disposal window**: 10 seconds after tap
- **Minimum fill rise**: 2% (configurable threshold)
- **Redemption flow**: v1 generates an 8-char code for manual shop validation (no real payment integration)

## Project Structure

```
smart-bin-reward-system/
├── smart_bin_esp32.ino       # Arduino IDE firmware (ESP32 only)
├── schema.sql                 # Supabase schema with RLS + triggers
└── pwa/                       # React + Vite PWA
    ├── src/
    │   ├── components/
    │   │   ├── FleetView.jsx      # All bins dashboard
    │   │   ├── BinDetail.jsx      # Bin chart + history
    │   │   ├── CitizenLookup.jsx  # RFID card balance/history
    │   │   ├── RedeemFlow.jsx     # Generate redemption codes
    │   │   └── AdminAudit.jsx     # Fraud detection view
    │   ├── lib/
    │   │   └── supabase.js        # Supabase client
    │   ├── App.jsx                # Main app with navigation
    │   └── App.css                # Dark mode styling
    ├── public/
    │   ├── manifest.json          # PWA manifest
    │   └── sw.js                  # Service worker
    └── vercel.json                # Vercel deployment config
```

## Hardware

- **ESP32 DevKit** (no ESP8266 — RFID SPI pins conflict)
- **HC-SR04 or JSN-SR04T** ultrasonic sensor (Trig: GPIO 5, Echo: GPIO 18)
- **SW-420** vibration sensor (GPIO 19, interrupt-capable)
- **Push button** for calibration (GPIO 21, internal pull-up)
- **RC522 RFID/NFC** reader (SPI: SDA=15, SCK=18, MOSI=23, MISO=19, RST=22)

## Firmware Features

### Bin Monitoring
- WiFi connect + auto-reconnect
- NTP time sync via `configTime()`
- Calibration button (median-of-5 ultrasonic baseline, stored in Preferences flash)
- Fill % reading every 30s (median of 5 samples)
- POST to Supabase `bin_readings` table

### Empty Detection
- Vibration pattern detection (15s motion window, 3s quiet period)
- Classify "bump" vs "handling" (3+ pulses, 1s+ duration)
- Settle delay (5s), measure fill before/after
- Log to `empty_events`: `emptied` / `handling_no_empty` / `emptied_unconfirmed`

### Reward Flow (New)
1. RFID tap → read UID via RC522
2. Start 10s "disposal window" timer
3. Watch for fill_pct rise + vibration (handling)
4. If confirmed rise → log `reward_events` with `confidence: "confirmed"`, points=10
5. If no rise → log with `confidence: "no_disposal"`, points=0
6. Rate limit: same card + bin within 5min → `confidence: "rate_limited"`, points=0

### Offline Support
- Up to 20 events buffered in RAM
- Auto-flush on WiFi reconnect

## Supabase Schema

Tables:
- `devices` — device_id (PK), location, created_at
- `bin_readings` — fill % over time
- `empty_events` — bin collections
- `citizens` — card_uid (PK), name, points_balance
- `reward_events` — disposal attempts
- `redemptions` — point redemptions

Triggers:
- `award_points_on_reward()` — increments `citizens.points_balance` on confirmed rewards

Functions:
- `redeem_points(card_uid, points)` — generates 8-char redemption code, deducts points

RLS:
- All tables enabled for anon key (exhibition/demo only)
- **Production note**: Replace with service role key + proper auth

Realtime:
- Enabled on all tables for live PWA updates

## PWA Features

### Views
1. **Fleet View** — All bins, sorted by fill %, status badges (ok/needs pickup/offline), rename location
2. **Bin Detail** — Fill gauge, 24h chart, empty event history
3. **Citizen Lookup** — Enter card UID, show balance + reward history
4. **Redeem Flow** — Generate redemption codes, deduct points
5. **Admin Audit** — Recent reward_events, filter by confidence, fraud detection

### Features
- Realtime updates (Supabase subscriptions)
- Dark theme UI
- Push notifications when bin crosses 80% fill
- Installable PWA (manifest.json + service worker)
- Responsive design (mobile + desktop)

## Setup & Deployment

### 1. Supabase Setup

1. Create a new Supabase project at https://supabase.com
2. Go to SQL Editor and run `schema.sql`
3. Copy your project URL and anon key

### 2. Firmware Setup

1. Open Arduino IDE (NOT PlatformIO)
2. Install ESP32 board package
3. Install libraries:
   - `WiFi` (built-in)
   - `HTTPClient` (built-in)
   - `Preferences` (built-in)
   - `SPI` (built-in)
   - `MFRC522` (install from Arduino Library Manager)
4. Open `smart_bin_esp32.ino`
5. Update WiFi credentials, Supabase URL/anon key, DEVICE_ID
6. Select board: "ESP32 Dev Module"
7. Upload to your ESP32

### 3. PWA Deployment

```bash
cd pwa
npm install
npm run build
```

Deploy to Vercel:
```bash
vercel --prod
```

## Configuration Values (already set)

- **WiFi SSID**: `VIPUl1`
- **WiFi Password**: `vipul@india`
- **Supabase URL**: `https://bpecehlmvzuirxmruvyt.supabase.co`
- **Supabase Anon Key**: (set in firmware + PWA)
- **Default Bin Height**: 25 cm

## Security Considerations for Production

⚠️ **This v1 uses anon key access for exhibition/demo purposes.**

A real municipal rollout should:
1. Replace anon key with service role key for firmware POST operations
2. Implement proper citizen authentication (OAuth, JWT, etc.)
3. Restrict redemption endpoint to authenticated admin users only
4. Add audit logging for all point deductions
5. Implement fraud detection (multiple disposals in short time)
6. Add geofencing to prevent reward events from unauthorized devices
7. Encrypt sensitive PII (citizen names, contact info)

## License

MIT

## Contributing

Issues and pull requests welcome.