-- Smart Bin Municipal Monitoring + Citizen Reward System
-- Supabase Schema
-- Deploy this SQL in your Supabase SQL Editor

-- ============================================================================
-- TABLES
-- ============================================================================

-- Devices table (bins)
CREATE TABLE IF NOT EXISTS devices (
  device_id TEXT PRIMARY KEY,
  location TEXT DEFAULT 'Not Set',
  created_at TIMESTAMPTZ DEFAULT NOW()
);

-- Bin readings (fill percentage)
CREATE TABLE IF NOT EXISTS bin_readings (
  id BIGSERIAL PRIMARY KEY,
  device_id TEXT NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
  fill_pct NUMERIC(5,2) NOT NULL CHECK (fill_pct >= 0 AND fill_pct <= 100),
  timestamp TIMESTAMPTZ NOT NULL,
  created_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX idx_bin_readings_device_time ON bin_readings(device_id, timestamp DESC);

-- Empty events (bin collection)
CREATE TABLE IF NOT EXISTS empty_events (
  id BIGSERIAL PRIMARY KEY,
  device_id TEXT NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
  event_type TEXT NOT NULL CHECK (event_type IN ('emptied', 'handling_no_empty', 'emptied_unconfirmed')),
  fill_pct_before NUMERIC(5,2) NOT NULL,
  fill_pct_after NUMERIC(5,2) NOT NULL,
  pulse_count INT NOT NULL,
  active_duration_ms INT NOT NULL,
  timestamp TIMESTAMPTZ NOT NULL,
  created_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX idx_empty_events_device_time ON empty_events(device_id, timestamp DESC);

-- Citizens table (RFID cardholders)
CREATE TABLE IF NOT EXISTS citizens (
  card_uid TEXT PRIMARY KEY,
  name TEXT,
  points_balance INT NOT NULL DEFAULT 0 CHECK (points_balance >= 0),
  created_at TIMESTAMPTZ DEFAULT NOW()
);

-- Reward events (disposal logging)
CREATE TABLE IF NOT EXISTS reward_events (
  id BIGSERIAL PRIMARY KEY,
  card_uid TEXT NOT NULL,
  device_id TEXT NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
  fill_pct_before NUMERIC(5,2) NOT NULL,
  fill_pct_after NUMERIC(5,2) NOT NULL,
  points_awarded INT NOT NULL DEFAULT 0,
  confidence TEXT NOT NULL CHECK (confidence IN ('confirmed', 'no_disposal', 'rate_limited')),
  timestamp TIMESTAMPTZ NOT NULL,
  created_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX idx_reward_events_card ON reward_events(card_uid, timestamp DESC);
CREATE INDEX idx_reward_events_device ON reward_events(device_id, timestamp DESC);
CREATE INDEX idx_reward_events_confidence ON reward_events(confidence, timestamp DESC);

-- Redemptions table (points spending)
CREATE TABLE IF NOT EXISTS redemptions (
  id BIGSERIAL PRIMARY KEY,
  card_uid TEXT NOT NULL REFERENCES citizens(card_uid) ON DELETE CASCADE,
  points_spent INT NOT NULL CHECK (points_spent > 0),
  redemption_code TEXT NOT NULL UNIQUE,
  redeemed_at TIMESTAMPTZ DEFAULT NOW(),
  created_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX idx_redemptions_card ON redemptions(card_uid, redeemed_at DESC);
CREATE INDEX idx_redemptions_code ON redemptions(redemption_code);

-- ============================================================================
-- TRIGGER: Auto-award points on confirmed reward events
-- ============================================================================

CREATE OR REPLACE FUNCTION award_points_on_reward()
RETURNS TRIGGER AS $$
BEGIN
  -- Only process confirmed disposals with points > 0
  IF NEW.confidence = 'confirmed' AND NEW.points_awarded > 0 THEN
    -- Insert or update citizen record
    INSERT INTO citizens (card_uid, points_balance)
    VALUES (NEW.card_uid, NEW.points_awarded)
    ON CONFLICT (card_uid)
    DO UPDATE SET points_balance = citizens.points_balance + NEW.points_awarded;
  END IF;

  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER trigger_award_points
AFTER INSERT ON reward_events
FOR EACH ROW
EXECUTE FUNCTION award_points_on_reward();

-- ============================================================================
-- FUNCTION: Redeem points (with validation)
-- ============================================================================

CREATE OR REPLACE FUNCTION redeem_points(
  p_card_uid TEXT,
  p_points_spent INT
)
RETURNS TABLE (
  success BOOLEAN,
  redemption_code TEXT,
  new_balance INT,
  error_message TEXT
) AS $$
DECLARE
  v_current_balance INT;
  v_redemption_code TEXT;
BEGIN
  -- Check citizen exists and has enough points
  SELECT points_balance INTO v_current_balance
  FROM citizens
  WHERE card_uid = p_card_uid
  FOR UPDATE;

  IF NOT FOUND THEN
    RETURN QUERY SELECT FALSE, NULL::TEXT, 0, 'Card UID not found';
    RETURN;
  END IF;

  IF v_current_balance < p_points_spent THEN
    RETURN QUERY SELECT FALSE, NULL::TEXT, v_current_balance, 'Insufficient points';
    RETURN;
  END IF;

  -- Generate 8-char alphanumeric code
  v_redemption_code := UPPER(SUBSTRING(MD5(RANDOM()::TEXT || CLOCK_TIMESTAMP()::TEXT) FROM 1 FOR 8));

  -- Deduct points
  UPDATE citizens
  SET points_balance = points_balance - p_points_spent
  WHERE card_uid = p_card_uid;

  -- Log redemption
  INSERT INTO redemptions (card_uid, points_spent, redemption_code)
  VALUES (p_card_uid, p_points_spent, v_redemption_code);

  -- Return success
  RETURN QUERY SELECT TRUE, v_redemption_code, v_current_balance - p_points_spent, NULL::TEXT;
END;
$$ LANGUAGE plpgsql;

-- ============================================================================
-- ROW LEVEL SECURITY (RLS)
-- ============================================================================

-- Enable RLS on all tables
ALTER TABLE devices ENABLE ROW LEVEL SECURITY;
ALTER TABLE bin_readings ENABLE ROW LEVEL SECURITY;
ALTER TABLE empty_events ENABLE ROW LEVEL SECURITY;
ALTER TABLE citizens ENABLE ROW LEVEL SECURITY;
ALTER TABLE reward_events ENABLE ROW LEVEL SECURITY;
ALTER TABLE redemptions ENABLE ROW LEVEL SECURITY;

-- IMPORTANT: For exhibition/demo purposes, allow anon key access
-- A production municipal deployment should use:
-- - Service role key or authenticated endpoints for firmware
-- - Proper citizen authentication (not just card UID lookup)
-- - Admin-only access for sensitive operations

-- Devices: allow upsert (insert + update) from anon
CREATE POLICY devices_anon_upsert ON devices
FOR ALL
TO anon
USING (true)
WITH CHECK (true);

-- Bin readings: allow insert from anon (firmware)
CREATE POLICY bin_readings_anon_insert ON bin_readings
FOR INSERT
TO anon
WITH CHECK (true);

-- Bin readings: allow select for PWA
CREATE POLICY bin_readings_anon_select ON bin_readings
FOR SELECT
TO anon
USING (true);

-- Empty events: allow insert from anon (firmware)
CREATE POLICY empty_events_anon_insert ON empty_events
FOR INSERT
TO anon
WITH CHECK (true);

-- Empty events: allow select for PWA
CREATE POLICY empty_events_anon_select ON empty_events
FOR SELECT
TO anon
USING (true);

-- Citizens: allow select and insert/update (for point awards)
CREATE POLICY citizens_anon_select ON citizens
FOR SELECT
TO anon
USING (true);

CREATE POLICY citizens_anon_upsert ON citizens
FOR ALL
TO anon
USING (true)
WITH CHECK (true);

-- Reward events: allow insert from anon (firmware)
CREATE POLICY reward_events_anon_insert ON reward_events
FOR INSERT
TO anon
WITH CHECK (true);

-- Reward events: allow select for PWA
CREATE POLICY reward_events_anon_select ON reward_events
FOR SELECT
TO anon
USING (true);

-- Redemptions: allow select and insert (via redeem_points function)
CREATE POLICY redemptions_anon_select ON redemptions
FOR SELECT
TO anon
USING (true);

CREATE POLICY redemptions_anon_insert ON redemptions
FOR INSERT
TO anon
WITH CHECK (true);

-- ============================================================================
-- ENABLE REALTIME
-- ============================================================================

-- Enable Realtime on all tables for live PWA updates
ALTER PUBLICATION supabase_realtime ADD TABLE devices;
ALTER PUBLICATION supabase_realtime ADD TABLE bin_readings;
ALTER PUBLICATION supabase_realtime ADD TABLE empty_events;
ALTER PUBLICATION supabase_realtime ADD TABLE citizens;
ALTER PUBLICATION supabase_realtime ADD TABLE reward_events;
ALTER PUBLICATION supabase_realtime ADD TABLE redemptions;

-- ============================================================================
-- SAMPLE DATA (optional - for testing)
-- ============================================================================

-- Insert a test device
-- INSERT INTO devices (device_id, location) VALUES ('BIN_ESP32_001', 'Main Street North');

-- Insert a test citizen
-- INSERT INTO citizens (card_uid, name, points_balance) VALUES ('ABCD1234', 'Test User', 0);

-- ============================================================================
-- NOTES
-- ============================================================================

-- Security considerations for production:
-- 1. Replace anon key access with service role key for firmware POST operations
-- 2. Implement proper authentication for citizen lookup (OAuth, JWT, etc.)
-- 3. Restrict redemptions to authenticated admin users only
-- 4. Add audit logging for all point deductions
-- 5. Implement fraud detection (e.g., multiple disposals from same user in short time)
-- 6. Add geofencing to prevent reward events from unauthorized device_ids
-- 7. Encrypt sensitive PII (citizen names, contact info if added later)

-- Performance optimization for large deployments:
-- 1. Partition bin_readings and reward_events by month for time-series queries
-- 2. Add materialized views for dashboard aggregations (e.g., daily bin fill stats)
-- 3. Archive old data to cold storage after 90 days
-- 4. Add database connection pooling (Supabase handles this automatically)

-- Realtime considerations:
-- 1. Realtime is enabled on all tables - monitor connection count as fleet scales
-- 2. For >1000 concurrent PWA clients, consider switching to polling for non-critical updates
-- 3. Use Supabase Realtime filters to subscribe only to relevant device_ids per client
