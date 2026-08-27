-- Smart Bin Municipal Monitoring + Citizen Reward System
-- Supabase Schema with Authentication
-- Deploy this SQL in your Supabase SQL Editor

-- ============================================================================
-- TABLES
-- ============================================================================

-- User profiles (extends Supabase auth.users)
CREATE TABLE IF NOT EXISTS profiles (
  id UUID PRIMARY KEY REFERENCES auth.users(id) ON DELETE CASCADE,
  email TEXT UNIQUE NOT NULL,
  full_name TEXT NOT NULL,
  account_type TEXT NOT NULL CHECK (account_type IN ('civilian', 'admin')),
  card_uid TEXT UNIQUE, -- RFID card linked to this account (civilians only)
  created_at TIMESTAMPTZ DEFAULT NOW(),
  updated_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX idx_profiles_card_uid ON profiles(card_uid);
CREATE INDEX idx_profiles_account_type ON profiles(account_type);

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

-- Citizens table (RFID cardholders with points)
CREATE TABLE IF NOT EXISTS citizens (
  card_uid TEXT PRIMARY KEY,
  user_id UUID REFERENCES auth.users(id) ON DELETE SET NULL, -- Link to registered user
  name TEXT,
  points_balance INT NOT NULL DEFAULT 0 CHECK (points_balance >= 0),
  created_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX idx_citizens_user_id ON citizens(user_id);

-- Pending card links (for first-tap registration)
CREATE TABLE IF NOT EXISTS pending_card_links (
  id BIGSERIAL PRIMARY KEY,
  card_uid TEXT NOT NULL UNIQUE,
  device_id TEXT NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
  timestamp TIMESTAMPTZ NOT NULL,
  claimed BOOLEAN DEFAULT false,
  claimed_by UUID REFERENCES auth.users(id),
  claimed_at TIMESTAMPTZ,
  expires_at TIMESTAMPTZ DEFAULT (NOW() + INTERVAL '24 hours'),
  created_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX idx_pending_card_links_uid ON pending_card_links(card_uid);
CREATE INDEX idx_pending_card_links_device ON pending_card_links(device_id);
CREATE INDEX idx_pending_card_links_expires ON pending_card_links(expires_at);

-- Reward events (disposal logging)
CREATE TABLE IF NOT EXISTS reward_events (
  id BIGSERIAL PRIMARY KEY,
  card_uid TEXT NOT NULL,
  device_id TEXT NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
  fill_pct_before NUMERIC(5,2) NOT NULL,
  fill_pct_after NUMERIC(5,2) NOT NULL,
  weight_estimate_kg NUMERIC(6,2) DEFAULT 0,  -- Estimated weight in kg (no load cell, calculated from fill rise)
  points_awarded INT NOT NULL DEFAULT 0,
  confidence TEXT NOT NULL CHECK (confidence IN ('confirmed', 'no_disposal', 'rate_limited', 'pending_link')),
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
-- TRIGGERS
-- ============================================================================

-- Auto-update updated_at timestamp
CREATE OR REPLACE FUNCTION update_updated_at_column()
RETURNS TRIGGER AS $$
BEGIN
  NEW.updated_at = NOW();
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER update_profiles_updated_at
BEFORE UPDATE ON profiles
FOR EACH ROW
EXECUTE FUNCTION update_updated_at_column();

-- Check card registration and create pending link if needed
CREATE OR REPLACE FUNCTION check_card_registration()
RETURNS TRIGGER AS $$
DECLARE
  card_exists INT;
  pending_exists INT;
BEGIN
  -- Only process pending_link events
  IF NEW.confidence = 'pending_link' THEN
    -- Check if card is already registered
    SELECT COUNT(*) INTO card_exists
    FROM citizens WHERE card_uid = NEW.card_uid;

    -- Check if pending link already exists
    SELECT COUNT(*) INTO pending_exists
    FROM pending_card_links WHERE card_uid = NEW.card_uid AND claimed = false;

    -- If card not registered and no pending link, create one
    IF card_exists = 0 AND pending_exists = 0 THEN
      INSERT INTO pending_card_links (card_uid, device_id, timestamp)
      VALUES (NEW.card_uid, NEW.device_id, NEW.timestamp);
    END IF;
  END IF;

  RETURN NEW;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;

CREATE TRIGGER trigger_check_card_registration
AFTER INSERT ON reward_events
FOR EACH ROW
EXECUTE FUNCTION check_card_registration();

-- Auto-award points on confirmed reward events
CREATE OR REPLACE FUNCTION award_points_on_reward()
RETURNS TRIGGER AS $$
BEGIN
  IF NEW.confidence = 'confirmed' AND NEW.points_awarded > 0 THEN
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

-- Create profile on user signup
CREATE OR REPLACE FUNCTION create_profile_for_user()
RETURNS TRIGGER AS $$
BEGIN
  INSERT INTO profiles (id, email, full_name, account_type)
  VALUES (
    NEW.id,
    NEW.email,
    COALESCE(NEW.raw_user_meta_data->>'full_name', 'User'),
    COALESCE(NEW.raw_user_meta_data->>'account_type', 'civilian')
  );
  RETURN NEW;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;

CREATE TRIGGER on_auth_user_created
AFTER INSERT ON auth.users
FOR EACH ROW
EXECUTE FUNCTION create_profile_for_user();

-- ============================================================================
-- FUNCTIONS
-- ============================================================================

-- Link RFID card to user account during registration
CREATE OR REPLACE FUNCTION link_rfid_to_user(
  p_user_id UUID,
  p_card_uid TEXT
)
RETURNS TABLE (
  success BOOLEAN,
  error_message TEXT
) AS $$
DECLARE
  v_card_exists BOOLEAN;
  v_card_linked BOOLEAN;
BEGIN
  -- Check if card is already linked to another user
  SELECT EXISTS (
    SELECT 1 FROM profiles WHERE card_uid = p_card_uid AND id != p_user_id
  ) INTO v_card_linked;

  IF v_card_linked THEN
    RETURN QUERY SELECT FALSE, 'This RFID card is already linked to another account';
    RETURN;
  END IF;

  -- Update profile with card_uid
  UPDATE profiles
  SET card_uid = p_card_uid
  WHERE id = p_user_id;

  -- Link existing citizen record to user if it exists
  UPDATE citizens
  SET user_id = p_user_id
  WHERE card_uid = p_card_uid;

  RETURN QUERY SELECT TRUE, NULL::TEXT;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;

-- Claim a pending card link and link it to user account
CREATE OR REPLACE FUNCTION claim_pending_card(
  p_user_id UUID,
  p_card_uid TEXT
)
RETURNS TABLE (
  success BOOLEAN,
  error_message TEXT
) AS $$
DECLARE
  v_card_exists BOOLEAN;
  v_already_linked BOOLEAN;
  v_pending_id BIGINT;
BEGIN
  -- Check if card is already linked to another user
  SELECT EXISTS (
    SELECT 1 FROM profiles WHERE card_uid = p_card_uid AND id != p_user_id
  ) INTO v_already_linked;

  IF v_already_linked THEN
    RETURN QUERY SELECT FALSE, 'This card is already linked to another account';
    RETURN;
  END IF;

  -- Check if card is already linked to this user
  SELECT EXISTS (
    SELECT 1 FROM profiles WHERE card_uid = p_card_uid AND id = p_user_id
  ) INTO v_card_exists;

  IF v_card_exists THEN
    RETURN QUERY SELECT FALSE, 'This card is already linked to your account';
    RETURN;
  END IF;

  -- Find the pending link
  SELECT id INTO v_pending_id
  FROM pending_card_links
  WHERE card_uid = p_card_uid AND claimed = false AND expires_at > NOW();

  IF NOT FOUND THEN
    RETURN QUERY SELECT FALSE, 'No valid pending card link found. Card may have expired.';
    RETURN;
  END IF;

  -- Link card to user profile
  UPDATE profiles
  SET card_uid = p_card_uid
  WHERE id = p_user_id;

  -- Create citizen record
  INSERT INTO citizens (card_uid, user_id, points_balance)
  VALUES (p_card_uid, p_user_id, 0)
  ON CONFLICT (card_uid)
  DO UPDATE SET user_id = p_user_id;

  -- Mark pending link as claimed
  UPDATE pending_card_links
  SET claimed = true, claimed_by = p_user_id, claimed_at = NOW()
  WHERE id = v_pending_id;

  -- Award points retroactively for the tap that triggered this
  UPDATE reward_events
  SET confidence = 'confirmed', points_awarded = 10
  WHERE card_uid = p_card_uid AND confidence = 'pending_link';

  -- Award the points
  UPDATE citizens
  SET points_balance = points_balance + 10
  WHERE card_uid = p_card_uid;

  RETURN QUERY SELECT TRUE, NULL::TEXT;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;

-- Redeem points (with validation)
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

  v_redemption_code := UPPER(SUBSTRING(MD5(RANDOM()::TEXT || CLOCK_TIMESTAMP()::TEXT) FROM 1 FOR 8));

  UPDATE citizens
  SET points_balance = points_balance - p_points_spent
  WHERE card_uid = p_card_uid;

  INSERT INTO redemptions (card_uid, points_spent, redemption_code)
  VALUES (p_card_uid, p_points_spent, v_redemption_code);

  RETURN QUERY SELECT TRUE, v_redemption_code, v_current_balance - p_points_spent, NULL::TEXT;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;

-- ============================================================================
-- ROW LEVEL SECURITY (RLS)
-- ============================================================================

ALTER TABLE profiles ENABLE ROW LEVEL SECURITY;
ALTER TABLE devices ENABLE ROW LEVEL SECURITY;
ALTER TABLE bin_readings ENABLE ROW LEVEL SECURITY;
ALTER TABLE empty_events ENABLE ROW LEVEL SECURITY;
ALTER TABLE citizens ENABLE ROW LEVEL SECURITY;
ALTER TABLE reward_events ENABLE ROW LEVEL SECURITY;
ALTER TABLE redemptions ENABLE ROW LEVEL SECURITY;
ALTER TABLE pending_card_links ENABLE ROW LEVEL SECURITY;

-- Profiles: users can read their own profile, admins can read all
CREATE POLICY profiles_select_own ON profiles
FOR SELECT
TO authenticated
USING (auth.uid() = id OR EXISTS (
  SELECT 1 FROM profiles WHERE id = auth.uid() AND account_type = 'admin'
));

CREATE POLICY profiles_update_own ON profiles
FOR UPDATE
TO authenticated
USING (auth.uid() = id)
WITH CHECK (auth.uid() = id);

-- Devices: allow firmware inserts (anon), admins can read/update
CREATE POLICY devices_anon_insert ON devices
FOR INSERT
TO anon
WITH CHECK (true);

CREATE POLICY devices_admin_select ON devices
FOR SELECT
TO authenticated
USING (EXISTS (
  SELECT 1 FROM profiles WHERE id = auth.uid() AND account_type = 'admin'
));

CREATE POLICY devices_admin_update ON devices
FOR UPDATE
TO authenticated
USING (EXISTS (
  SELECT 1 FROM profiles WHERE id = auth.uid() AND account_type = 'admin'
))
WITH CHECK (EXISTS (
  SELECT 1 FROM profiles WHERE id = auth.uid() AND account_type = 'admin'
));

-- Bin readings: firmware can insert (anon), admins can read
CREATE POLICY bin_readings_anon_insert ON bin_readings
FOR INSERT
TO anon
WITH CHECK (true);

CREATE POLICY bin_readings_admin_select ON bin_readings
FOR SELECT
TO authenticated
USING (EXISTS (
  SELECT 1 FROM profiles WHERE id = auth.uid() AND account_type = 'admin'
));

-- Empty events: firmware can insert (anon), admins can read
CREATE POLICY empty_events_anon_insert ON empty_events
FOR INSERT
TO anon
WITH CHECK (true);

CREATE POLICY empty_events_admin_select ON empty_events
FOR SELECT
TO authenticated
USING (EXISTS (
  SELECT 1 FROM profiles WHERE id = auth.uid() AND account_type = 'admin'
));

-- Citizens: users can read their own linked card, admins can read all
CREATE POLICY citizens_select_own ON citizens
FOR SELECT
TO authenticated
USING (
  user_id = auth.uid() OR
  EXISTS (SELECT 1 FROM profiles WHERE id = auth.uid() AND account_type = 'admin')
);

CREATE POLICY citizens_anon_upsert ON citizens
FOR ALL
TO anon
USING (true)
WITH CHECK (true);

-- Reward events: firmware can insert (anon), users can read their own, admins can read all
CREATE POLICY reward_events_anon_insert ON reward_events
FOR INSERT
TO anon
WITH CHECK (true);

CREATE POLICY reward_events_select_own ON reward_events
FOR SELECT
TO authenticated
USING (
  card_uid IN (SELECT card_uid FROM profiles WHERE id = auth.uid()) OR
  EXISTS (SELECT 1 FROM profiles WHERE id = auth.uid() AND account_type = 'admin')
);

-- Redemptions: users can read their own, admins can read all
CREATE POLICY redemptions_select_own ON redemptions
FOR SELECT
TO authenticated
USING (
  card_uid IN (SELECT card_uid FROM profiles WHERE id = auth.uid()) OR
  EXISTS (SELECT 1 FROM profiles WHERE id = auth.uid() AND account_type = 'admin')
);

CREATE POLICY redemptions_anon_insert ON redemptions
FOR INSERT
TO anon
WITH CHECK (true);

-- Pending card links: users can read unclaimed links, admins can read all
CREATE POLICY pending_card_links_select_own ON pending_card_links
FOR SELECT
TO authenticated
USING (
  (claimed = false AND expires_at > NOW()) OR
  claimed_by = auth.uid() OR
  EXISTS (SELECT 1 FROM profiles WHERE id = auth.uid() AND account_type = 'admin')
);

-- Users can update their own pending links (claim them)
CREATE POLICY pending_card_links_update_own ON pending_card_links
FOR UPDATE
TO authenticated
USING (
  claimed = false AND expires_at > NOW()
)
WITH CHECK (
  claimed_by = auth.uid()
);

-- ============================================================================
-- ENABLE REALTIME
-- ============================================================================

ALTER PUBLICATION supabase_realtime ADD TABLE profiles;
ALTER PUBLICATION supabase_realtime ADD TABLE devices;
ALTER PUBLICATION supabase_realtime ADD TABLE bin_readings;
ALTER PUBLICATION supabase_realtime ADD TABLE empty_events;
ALTER PUBLICATION supabase_realtime ADD TABLE citizens;
ALTER PUBLICATION supabase_realtime ADD TABLE reward_events;
ALTER PUBLICATION supabase_realtime ADD TABLE redemptions;
ALTER PUBLICATION supabase_realtime ADD TABLE pending_card_links;