# Smart Bin Reward System — Deployment Guide

## Current deployment

- PWA root directory: `pwa`
- Supabase project ref: `bpecehlmvzuirxmruvyt`
- ESP32 device IDs: `BIN_ESP32_001`, `BIN_ESP32_002`
- Reward RPC: `public.record_reward()`
- Admin reset RPC: `public.reset_project_data()`

## Supabase setup

Run these in order:

1. `schema.sql`
2. `supabase/account_aware_rewards_and_reset.sql`

The second file is required because the ESP32 calls `record_reward()` and the admin panel calls `reset_project_data()`.

The reset RPC uses explicit `WHERE TRUE` clauses because the project database requires safe DELETE statements.

## ESP32 firmware

Use the sketch at `smart_bin_esp32/smart_bin_esp32.ino` and keep the existing wiring unchanged.

### Actual GPIO mapping used by the firmware

```text
HC-SR04: TRIG GPIO 5, ECHO GPIO 4
SW-420:  GPIO 27
Button:  GPIO 21 → GND
RC522:   SS 15, RST 22, SCK 18, MISO 19, MOSI 23
LED:     GPIO 2
```

Do not use the older GPIO 18/19 ultrasonic/vibration mapping from previous versions of this document.

Wi-Fi and Supabase credentials belong in `smart_bin_esp32/arduino_secrets.h`.

## PWA / Vercel

- Framework: Vite
- Root directory: `pwa`
- Build command: `npm run build`
- Output directory: `dist`

The PWA registers `public/sw.js` from `index.html` and uses a network-first navigation strategy to avoid stale deployment chunks.

## RFID reward flow

1. Citizen taps an RFID card.
2. ESP32 detects a qualifying fill rise and calls `record_reward()`.
3. Unknown cards become `pending_link` and create a pending card link.
4. The citizen claims the card through the PWA.
5. Pending reward events are promoted to `confirmed` and 10 points are credited per disposal.
6. Future linked-card rewards are confirmed immediately.

Pending card links expire after 24 hours.

## Admin panel

The admin panel can:
- View fleet status and latest readings.
- Edit bin locations.
- View reward audit events, including pending RFID links.
- Wipe operational project data while preserving user accounts.

## Important testing rules

- Reward window: 10 seconds after RFID tap.
- Minimum fill rise: 2 percentage points.
- Rate limit: 5 minutes per card/bin.
- Points per qualifying disposal: 10.
- Offline ESP32 events are buffered in RAM and retried after Wi-Fi reconnects.

## Security

The ESP32 currently uses the Supabase anon key because this is a school/demo deployment. Do not use the anon-key firmware architecture for a municipal production rollout without adding device authentication and server-side anti-spoofing controls.
