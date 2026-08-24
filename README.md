# Smart Bin Municipal Monitoring + Citizen Reward System

A complete IoT solution for municipal smart bin management with RFID-based citizen rewards and account authentication.

## ⚠️ Important Notes

**v1 Placeholder Rules (confirm before production):**
- **Points per disposal**: 10 points (configurable constant in firmware)
- **Rate limit**: 5 minutes (same card, same bin)
- **Disposal window**: 10 seconds after tap
- **Minimum fill rise**: 2% (configurable threshold)
- **Redemption flow**: v1 generates an 8-char code for manual shop validation (no real payment integration)

## 🆕 Authentication System

### Account Types

**1. Civilian Account**
- For citizens who earn points by disposing waste
- Links to RFID card during registration
- Views: Points balance, reward history, redemptions
- Can only see their own data

**2. Admin Account**
- For municipal staff managing bins
- Full access to all bins and data
- Views: Fleet overview, bin details, reward audit
- Can edit bin locations
- No RFID card required

### Registration Flow
1. Choose account type (Civilian or Admin)
2. Enter email, password, full name
3. **Civilian only**: Enter RFID card UID (links card to account)
4. Email verification (Supabase built-in)
5. Login with credentials

### Login Flow
1. Enter email + password
2. Authenticated via Supabase Auth
3. Auto-redirected to appropriate dashboard based on account type

### Install Button
- Appears in top-right corner when PWA is installable
- Click to install as a home screen app
- **Disappears after install** (detects standalone mode)
- Works on Chrome, Edge, Safari iOS/Android

## Project Structure

```
smart-bin-reward-system/
├── smart_bin_esp32.ino       # Arduino IDE firmware (ESP32 only)
├── schema.sql                 # Supabase schema with auth + RLS + triggers
└── pwa/                       # React + Vite PWA
    ├── src/
    │   ├── components/
    │   │   ├── Login.jsx              # Login form
    │   │   ├── Register.jsx           # Registration with RFID linking
    │   │   ├── CivilianDashboard.jsx  # Civilian view (points, history)
    │   │   ├── AdminDashboard.jsx     # Admin view (fleet, audit)
    │   │   ├── FleetView.jsx          # All bins dashboard
    │   │   ├── BinDetail.jsx          # Bin chart + history
    │   │   ├── AdminAudit.jsx         # Reward audit
    │   │   └── InstallButton.jsx      # PWA install button
    │   ├── lib/
    │   │   └── supabase.js            # Supabase client
    │   ├── App.jsx                    # Main app with auth routing
    │   └── App.css                    # Dark mode styling
    ├── public/
    │   ├── manifest.json              # PWA manifest
    │   └── sw.js                      # Service worker
    └── vercel.json                    # Vercel deployment config
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

### Reward Flow
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

### Authentication Tables
- `profiles` — User profiles (extends auth.users)
  - `id` (UUID, FK to auth.users)
  - `email`, `full_name`, `account_type` (civilian/admin)
  - `card_uid` (unique, for civilians only)

### Data Tables
- `devices` — Bin registry
- `bin_readings` — Fill % time series
- `empty_events` — Bin collections
- `citizens` — Points balance (linked to profiles via user_id)
- `reward_events` — Disposal attempts
- `redemptions` — Point redemptions

### Triggers
- `create_profile_for_user()` — Auto-creates profile on signup
- `award_points_on_reward()` — Increments points on confirmed rewards

### Functions
- `link_rfid_to_user()` — Links RFID card to user account
- `redeem_points()` — Generates redemption code, deducts points

### RLS Policies
- **Civilians**: Read only their own profile, citizens record, reward_events, redemptions
- **Admins**: Read/update all devices, bin_readings, empty_events
- **Anon (firmware)**: Insert to devices, bin_readings, empty_events, reward_events

### Realtime
- Enabled on all tables for live PWA updates

## PWA Features

### Authentication
- Login/Register with email + password
- Auto-create profile on signup
- RFID card linking during registration
- Session persistence (stay logged in)
- Logout functionality

### Civilian Dashboard
- Large points balance display
- RFID card badge
- Stats: Total disposals, points earned, points redeemed, success rate
- Recent activity feed
- Redemption history

### Admin Dashboard
- Fleet view (all bins, sorted by fill %)
- Bin detail (chart, events)
- Reward audit (filter by confidence)
- Edit bin locations
- Real-time updates

### Universal Features
- Dark theme UI
- Realtime Supabase subscriptions
- Push notifications at 80% fill
- PWA install button (disappears after install)
- Responsive design (mobile + desktop)

## Setup & Deployment

### 1. Supabase Setup

1. Create a new Supabase project at https://supabase.com
2. Go to SQL Editor and run `schema.sql`
3. Configure authentication:
   - Go to Authentication → Providers
   - Enable Email provider (built-in)
4. Copy your project URL and anon key

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

**Important**: Set Root Directory to `pwa` in Vercel settings!

## Configuration Values (already set)

- **WiFi SSID**: `VIPUl1`
- **WiFi Password**: `vipul@india`
- **Supabase URL**: `https://bpecehlmvzuirxmruvyt.supabase.co`
- **Supabase Anon Key**: (set in firmware + PWA)
- **Default Bin Height**: 25 cm

## Testing the System

### 1. Test Authentication
1. Open PWA → Click "Register here"
2. Choose account type
3. Fill in details (include RFID UID for civilian)
4. Check email for verification
5. Login with credentials

### 2. Test Civilian Flow
1. Login as civilian
2. See points balance (starts at 0)
3. Tap RFID card at bin → drop trash
4. Points auto-update in real-time
5. View activity history

### 3. Test Admin Flow
1. Register as admin (no RFID needed)
2. Login → See fleet view
3. View all bins, edit locations
4. Check reward audit for fraud patterns

### 4. Test PWA Install
1. Open PWA in Chrome/Edge/Safari
2. Look for "Install App" button (top-right)
3. Click to install
4. Button disappears after install
5. Launch from home screen

## Security Considerations for Production

⚠️ **This v1 uses anon key for firmware + authenticated users for PWA.**

**Before municipal production rollout**:
1. Replace anon key with service role key for firmware POST operations
2. Implement admin email verification (currently any email can register as admin)
3. Add admin invitation system (only existing admins can create new admins)
4. Encrypt sensitive PII (citizen names, contact info)
5. Add audit logging for all admin actions
6. Implement rate limiting on registration endpoint
7. Add CAPTCHA to prevent automated registrations
8. Require email verification before account activation
9. Implement password strength requirements
10. Add 2FA for admin accounts

### Recommended Admin Setup Flow (Production)
1. First admin: Manually insert via SQL with verified email
2. Subsequent admins: Existing admin invites via email + temporary password
3. Civilians: Self-registration with email verification + admin approval for RFID linking

## Scaling Considerations

- For >100 bins: partition `bin_readings` by month
- For >1000 PWA clients: switch from Realtime to polling
- Add materialized views for dashboard aggregations
- Archive data >90 days to cold storage
- Use Supabase connection pooling (automatic)

## 📞 Support

For issues or questions, open an issue on GitHub:
https://github.com/anvitraut1-eng/smart-bin-reward-system/issues

---

**Build completed**: 2026-08-24
**Version**: 2.0 (Authentication added)
**Status**: ✅ Code ready, ⏳ Awaiting Supabase schema deployment + Vercel deployment