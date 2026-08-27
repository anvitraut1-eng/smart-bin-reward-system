-- Smart Bin Municipal Monitoring + Citizen Reward System
-- Supabase schema. Deploy in the Supabase SQL Editor.
--
-- This file mirrors what is deployed on project bpecehlmvzuirxmruvyt and is
-- safe to re-run: every object is created with IF NOT EXISTS, or dropped and
-- recreated. Note that PostgreSQL has no IF NOT EXISTS for CREATE TRIGGER,
-- CREATE POLICY, or ALTER PUBLICATION ... ADD TABLE, hence the DROP-then-CREATE
-- and DO-block guards below.

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

CREATE INDEX IF NOT EXISTS idx_profiles_card_uid ON profiles(card_uid);
CREATE INDEX IF NOT EXISTS idx_profiles_account_type ON profiles(account_type);

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

CREATE INDEX IF NOT EXISTS idx_bin_readings_device_time ON bin_readings(device_id, timestamp DESC);

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

CREATE INDEX IF NOT EXISTS idx_empty_events_device_time ON empty_events(device_id, timestamp DESC);

-- Citizens table (RFID cardholders with points)
CREATE TABLE IF NOT EXISTS citizens (
  card_uid TEXT PRIMARY KEY,
  user_id UUID REFERENCES auth.users(id) ON DELETE SET NULL, -- Link to registered user
  name TEXT,
  points_balance INT NOT NULL DEFAULT 0 CHECK (points_balance >= 0),
  created_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_citizens_user_id ON citizens(user_id);

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

CREATE INDEX IF NOT EXISTS idx_pending_card_links_uid ON pending_card_links(card_uid);
CREATE INDEX IF NOT EXISTS idx_pending_card_links_device ON pending_card_links(device_id);
CREATE INDEX IF NOT EXISTS idx_pending_card_links_expires ON pending_card_links(expires_at);

-- Reward events (disposal logging)
CREATE TABLE IF NOT EXISTS reward_events (
  id BIGSERIAL PRIMARY KEY,
  card_uid TEXT NOT NULL,
  device_id TEXT NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
  fill_pct_before NUMERIC(5,2) NOT NULL,
  fill_pct_after NUMERIC(5,2) NOT NULL,
  -- Estimated from the fill rise; no load cell is fitted. See estimateWeight()
  -- in smart_bin_esp32/smart_bin_esp32.ino.
  weight_estimate_kg NUMERIC(6,2) DEFAULT 0,
  points_awarded INT NOT NULL DEFAULT 0,
  confidence TEXT NOT NULL CHECK (confidence IN ('confirmed', 'no_disposal', 'rate_limited', 'pending_link')),
  timestamp TIMESTAMPTZ NOT NULL,
  created_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_reward_events_card ON reward_events(card_uid, timestamp DESC);
CREATE INDEX IF NOT EXISTS idx_reward_events_device ON reward_events(device_id, timestamp DESC);
CREATE INDEX IF NOT EXISTS idx_reward_events_confidence ON reward_events(confidence, timestamp DESC);

-- Redemptions table (points spending)
CREATE TABLE IF NOT EXISTS redemptions (
  id BIGSERIAL PRIMARY KEY,
  card_uid TEXT NOT NULL REFERENCES citizens(card_uid) ON DELETE CASCADE,
  points_spent INT NOT NULL CHECK (points_spent > 0),
  redemption_code TEXT NOT NULL UNIQUE,
  redeemed_at TIMESTAMPTZ DEFAULT NOW(),
  created_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_redemptions_card ON redemptions(card_uid, redeemed_at DESC);
CREATE INDEX IF NOT EXISTS idx_redemptions_code ON redemptions(redemption_code);

-- ============================================================================
-- RLS HELPER FUNCTIONS
-- ============================================================================
-- A policy on profiles must not itself SELECT FROM profiles: the subquery
-- re-enters the same policy and PostgreSQL raises "infinite recursion detected
-- in policy for relation profiles". These SECURITY DEFINER helpers run as the
-- owner and bypass RLS, breaking the cycle.
--
-- search_path is pinned to satisfy the Supabase linter (0011
-- function_search_path_mutable). It must NOT be set to '' -- that makes
-- unqualified table names unresolvable and every function fails at runtime.

CREATE OR REPLACE FUNCTION is_admin()
RETURNS BOOLEAN AS $$
  SELECT EXISTS (
    SELECT 1 FROM profiles
    WHERE id = auth.uid() AND account_type = 'admin'
  );
$$ LANGUAGE sql SECURITY DEFINER STABLE SET search_path = public;

CREATE OR REPLACE FUNCTION my_card_uid()
RETURNS TEXT AS $$
  SELECT card_uid FROM profiles WHERE id = auth.uid();
$$ LANGUAGE sql SECURITY DEFINER STABLE SET search_path = public;

-- ============================================================================
-- TRIGGER FUNCTIONS
-- ============================================================================

-- Auto-update updated_at timestamp
CREATE OR REPLACE FUNCTION update_updated_at_column()
RETURNS TRIGGER AS $$
BEGIN
  NEW.updated_at = NOW();
  RETURN NEW;
END;
$$ LANGUAGE plpgsql SET search_path = public, pg_temp;

-- Create a profile row whenever a user signs up
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
$$ LANGUAGE plpgsql SECURITY DEFINER SET search_path = public, pg_temp;

-- Credit points when a confirmed disposal is recorded. SECURITY DEFINER so the
-- firmware (anon role) never needs write access to citizens.
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
$$ LANGUAGE plpgsql SECURITY DEFINER SET search_path = public, pg_temp;

-- First-tap registration: an unrecognised card raises a pending link for a
-- citizen to claim in the PWA.
CREATE OR REPLACE FUNCTION check_card_registration()
RETURNS TRIGGER AS $$
DECLARE
  card_exists INT;
  pending_exists INT;
BEGIN
  IF NEW.confidence = 'pending_link' THEN
    SELECT COUNT(*) INTO card_exists
    FROM citizens WHERE card_uid = NEW.card_uid;

    SELECT COUNT(*) INTO pending_exists
    FROM pending_card_links WHERE card_uid = NEW.card_uid AND claimed = false;

    IF card_exists = 0 AND pending_exists = 0 THEN
      INSERT INTO pending_card_links (card_uid, device_id, timestamp)
      VALUES (NEW.card_uid, NEW.device_id, NEW.timestamp);
    END IF;
  END IF;

  RETURN NEW;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER SET search_path = public, pg_temp;

-- ============================================================================
-- TRIGGERS
-- ============================================================================
-- CREATE TRIGGER has no IF NOT EXISTS, so drop first.

DROP TRIGGER IF EXISTS update_profiles_updated_at ON profiles;
CREATE TRIGGER update_profiles_updated_at
  BEFORE UPDATE ON profiles
  FOR EACH ROW EXECUTE FUNCTION update_updated_at_column();

DROP TRIGGER IF EXISTS on_auth_user_created ON auth.users;
CREATE TRIGGER on_auth_user_created
  AFTER INSERT ON auth.users
  FOR EACH ROW EXECUTE FUNCTION create_profile_for_user();

DROP TRIGGER IF EXISTS trigger_award_points ON reward_events;
CREATE TRIGGER trigger_award_points
  AFTER INSERT ON reward_events
  FOR EACH ROW EXECUTE FUNCTION award_points_on_reward();

DROP TRIGGER IF EXISTS trigger_check_card_registration ON reward_events;
CREATE TRIGGER trigger_check_card_registration
  AFTER INSERT ON reward_events
  FOR EACH ROW EXECUTE FUNCTION check_card_registration();

-- ============================================================================
-- RPC FUNCTIONS
-- ============================================================================
-- SECURITY DEFINER bypasses RLS, so each of these must perform its own
-- auth.uid() authorization check. Without that, any signed-in caller could
-- pass someone else's identifier and act on their account.

-- Claim a pending card link and retroactively confirm its taps
CREATE OR REPLACE FUNCTION claim_pending_card(p_user_id UUID, p_card_uid TEXT)
RETURNS TABLE(success BOOLEAN, error_message TEXT) AS $$
DECLARE
  v_card_exists BOOLEAN;
  v_already_linked BOOLEAN;
  v_pending_id BIGINT;
  v_points INT;
BEGIN
  -- Only the signed-in user may claim for themselves. Without this an
  -- authenticated attacker could pass any UUID and hijack a pending card.
  IF auth.uid() IS NULL OR auth.uid() != p_user_id THEN
    RETURN QUERY SELECT FALSE, 'Not authorized';
    RETURN;
  END IF;

  SELECT EXISTS (
    SELECT 1 FROM profiles WHERE card_uid = p_card_uid AND id != p_user_id
  ) INTO v_already_linked;

  IF v_already_linked THEN
    RETURN QUERY SELECT FALSE, 'This card is already linked to another account';
    RETURN;
  END IF;

  SELECT EXISTS (
    SELECT 1 FROM profiles WHERE card_uid = p_card_uid AND id = p_user_id
  ) INTO v_card_exists;

  IF v_card_exists THEN
    RETURN QUERY SELECT FALSE, 'This card is already linked to your account';
    RETURN;
  END IF;

  SELECT id INTO v_pending_id
  FROM pending_card_links
  WHERE card_uid = p_card_uid AND claimed = false AND expires_at > NOW()
  FOR UPDATE;

  IF NOT FOUND THEN
    RETURN QUERY SELECT FALSE, 'No valid pending card link found. Card may have expired.';
    RETURN;
  END IF;

  UPDATE profiles SET card_uid = p_card_uid WHERE id = p_user_id;

  INSERT INTO citizens (card_uid, user_id, points_balance)
  VALUES (p_card_uid, p_user_id, 0)
  ON CONFLICT (card_uid)
  DO UPDATE SET user_id = p_user_id;

  UPDATE pending_card_links
  SET claimed = true, claimed_by = p_user_id, claimed_at = NOW()
  WHERE id = v_pending_id;

  -- Retroactively confirm the pending taps and award their points.
  -- Award exactly what was converted rather than a hardcoded 10, so a
  -- citizen with several pending taps is paid correctly.
  WITH promoted AS (
    UPDATE reward_events
    SET confidence = 'confirmed', points_awarded = 10
    WHERE card_uid = p_card_uid AND confidence = 'pending_link'
    RETURNING 10 AS pts
  )
  SELECT COALESCE(SUM(pts), 0) INTO v_points FROM promoted;

  -- trigger_award_points only fires on INSERT, not UPDATE, so credit here.
  UPDATE citizens
  SET points_balance = points_balance + v_points
  WHERE card_uid = p_card_uid;

  RETURN QUERY SELECT TRUE, NULL::TEXT;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER SET search_path = public, pg_temp;

-- Spend points and issue a redemption code
CREATE OR REPLACE FUNCTION redeem_points(p_card_uid TEXT, p_points_spent INTEGER)
RETURNS TABLE(success BOOLEAN, redemption_code TEXT, new_balance INTEGER, error_message TEXT) AS $$
DECLARE
  v_current_balance INT;
  v_redemption_code TEXT;
  v_owner UUID;
BEGIN
  IF p_points_spent IS NULL OR p_points_spent <= 0 THEN
    RETURN QUERY SELECT FALSE, NULL::TEXT, 0, 'Points must be greater than zero';
    RETURN;
  END IF;

  SELECT points_balance, user_id INTO v_current_balance, v_owner
  FROM citizens
  WHERE card_uid = p_card_uid
  FOR UPDATE;

  IF NOT FOUND THEN
    RETURN QUERY SELECT FALSE, NULL::TEXT, 0, 'Card UID not found';
    RETURN;
  END IF;

  -- Without this check any signed-in user could redeem another citizen's
  -- points by passing their card_uid, since SECURITY DEFINER bypasses RLS.
  IF auth.uid() IS NULL OR v_owner IS NULL OR v_owner != auth.uid() THEN
    RETURN QUERY SELECT FALSE, NULL::TEXT, 0, 'Not authorized for this card';
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
$$ LANGUAGE plpgsql SECURITY DEFINER SET search_path = public, pg_temp;

-- ============================================================================
-- ROW LEVEL SECURITY
-- ============================================================================

ALTER TABLE profiles            ENABLE ROW LEVEL SECURITY;
ALTER TABLE devices             ENABLE ROW LEVEL SECURITY;
ALTER TABLE bin_readings        ENABLE ROW LEVEL SECURITY;
ALTER TABLE empty_events        ENABLE ROW LEVEL SECURITY;
ALTER TABLE citizens            ENABLE ROW LEVEL SECURITY;
ALTER TABLE pending_card_links  ENABLE ROW LEVEL SECURITY;
ALTER TABLE reward_events       ENABLE ROW LEVEL SECURITY;
ALTER TABLE redemptions         ENABLE ROW LEVEL SECURITY;

-- CREATE POLICY has no IF NOT EXISTS, so drop first.

-- profiles: own row only. is_admin() is a function call rather than an inline
-- "OR account_type = 'admin'" -- the inline form is evaluated per row and would
-- expose every admin's name and email to any signed-in user.
DROP POLICY IF EXISTS profiles_select_own ON profiles;
CREATE POLICY profiles_select_own ON profiles
  FOR SELECT TO authenticated
  USING (id = auth.uid() OR is_admin());

DROP POLICY IF EXISTS profiles_update_own ON profiles;
CREATE POLICY profiles_update_own ON profiles
  FOR UPDATE TO authenticated
  USING (id = auth.uid())
  WITH CHECK (id = auth.uid());

-- devices: firmware self-registers; admins manage
DROP POLICY IF EXISTS devices_anon_insert ON devices;
CREATE POLICY devices_anon_insert ON devices
  FOR INSERT TO anon WITH CHECK (true);

DROP POLICY IF EXISTS devices_admin_select ON devices;
CREATE POLICY devices_admin_select ON devices
  FOR SELECT TO authenticated USING (is_admin());

DROP POLICY IF EXISTS devices_admin_update ON devices;
CREATE POLICY devices_admin_update ON devices
  FOR UPDATE TO authenticated USING (is_admin()) WITH CHECK (is_admin());

-- bin_readings / empty_events: firmware writes, admins read
DROP POLICY IF EXISTS bin_readings_anon_insert ON bin_readings;
CREATE POLICY bin_readings_anon_insert ON bin_readings
  FOR INSERT TO anon WITH CHECK (true);

DROP POLICY IF EXISTS bin_readings_admin_select ON bin_readings;
CREATE POLICY bin_readings_admin_select ON bin_readings
  FOR SELECT TO authenticated USING (is_admin());

DROP POLICY IF EXISTS empty_events_anon_insert ON empty_events;
CREATE POLICY empty_events_anon_insert ON empty_events
  FOR INSERT TO anon WITH CHECK (true);

DROP POLICY IF EXISTS empty_events_admin_select ON empty_events;
CREATE POLICY empty_events_admin_select ON empty_events
  FOR SELECT TO authenticated USING (is_admin());

-- citizens: no anon access at all. The anon key ships in the public PWA
-- bundle, so a permissive anon policy here would let anyone read every
-- balance and set their own. award_points_on_reward() is SECURITY DEFINER and
-- maintains this table on the firmware's behalf.
DROP POLICY IF EXISTS citizens_anon_upsert ON citizens;
DROP POLICY IF EXISTS citizens_select_own ON citizens;
CREATE POLICY citizens_select_own ON citizens
  FOR SELECT TO authenticated
  USING (user_id = auth.uid() OR is_admin());

-- pending_card_links: an unclaimed link must be visible to a user who does not
-- own the card yet -- that is what makes first-tap registration work.
-- SECURITY NOTE: this means any signed-in user can see any unclaimed link for
-- as long as it lives (24h by default), and could claim a card that is not
-- theirs. Shorten expires_at, or scope claiming to a device pairing code, if
-- that race matters for your deployment.
DROP POLICY IF EXISTS pending_card_links_select_own ON pending_card_links;
CREATE POLICY pending_card_links_select_own ON pending_card_links
  FOR SELECT TO authenticated
  USING ((claimed = false AND expires_at > NOW()) OR claimed_by = auth.uid() OR is_admin());

DROP POLICY IF EXISTS pending_card_links_update_own ON pending_card_links;
CREATE POLICY pending_card_links_update_own ON pending_card_links
  FOR UPDATE TO authenticated
  USING (claimed = false AND expires_at > NOW())
  WITH CHECK (claimed_by = auth.uid());

-- reward_events: firmware writes, citizen reads own via card, admin reads all
DROP POLICY IF EXISTS reward_events_anon_insert ON reward_events;
CREATE POLICY reward_events_anon_insert ON reward_events
  FOR INSERT TO anon WITH CHECK (true);

DROP POLICY IF EXISTS reward_events_select_own ON reward_events;
CREATE POLICY reward_events_select_own ON reward_events
  FOR SELECT TO authenticated
  USING (card_uid = my_card_uid() OR is_admin());

-- redemptions: written only by redeem_points(); readable by owner and admins
DROP POLICY IF EXISTS redemptions_anon_insert ON redemptions;
DROP POLICY IF EXISTS redemptions_select_own ON redemptions;
CREATE POLICY redemptions_select_own ON redemptions
  FOR SELECT TO authenticated
  USING (card_uid = my_card_uid() OR is_admin());

-- ============================================================================
-- FUNCTION EXECUTE GRANTS
-- ============================================================================
-- PostgreSQL grants EXECUTE on new functions to PUBLIC by default, and
-- anon/authenticated inherit it. Revoking from those two roles alone leaves
-- the PUBLIC grant in place and every function stays reachable at
-- /rest/v1/rpc/<name>, so PUBLIC must be revoked explicitly.

-- Trigger functions are never called directly. EXECUTE is checked when a
-- trigger is created, not when it fires, so revoking does not break them.
REVOKE ALL ON FUNCTION update_updated_at_column() FROM PUBLIC, anon, authenticated;
REVOKE ALL ON FUNCTION create_profile_for_user()  FROM PUBLIC, anon, authenticated;
REVOKE ALL ON FUNCTION award_points_on_reward()   FROM PUBLIC, anon, authenticated;
REVOKE ALL ON FUNCTION check_card_registration()  FROM PUBLIC, anon, authenticated;

-- RLS helpers must stay executable by authenticated: policy expressions are
-- evaluated with the privileges of the querying role. Both return only facts
-- about the caller, so RPC exposure discloses nothing extra.
REVOKE ALL ON FUNCTION is_admin()    FROM PUBLIC, anon;
REVOKE ALL ON FUNCTION my_card_uid() FROM PUBLIC, anon;
GRANT EXECUTE ON FUNCTION is_admin()    TO authenticated;
GRANT EXECUTE ON FUNCTION my_card_uid() TO authenticated;

-- Citizen-facing RPCs: signed-in callers only.
REVOKE ALL ON FUNCTION claim_pending_card(UUID, TEXT) FROM PUBLIC, anon;
REVOKE ALL ON FUNCTION redeem_points(TEXT, INTEGER)   FROM PUBLIC, anon;
GRANT EXECUTE ON FUNCTION claim_pending_card(UUID, TEXT) TO authenticated;
GRANT EXECUTE ON FUNCTION redeem_points(TEXT, INTEGER)   TO authenticated;

-- ============================================================================
-- REALTIME
-- ============================================================================
-- ALTER PUBLICATION ... ADD TABLE has no IF NOT EXISTS and errors if the table
-- is already a member, so guard each addition.

DO $$
DECLARE
  t TEXT;
BEGIN
  FOREACH t IN ARRAY ARRAY[
    'profiles', 'devices', 'bin_readings', 'empty_events',
    'citizens', 'pending_card_links', 'reward_events', 'redemptions'
  ] LOOP
    IF NOT EXISTS (
      SELECT 1 FROM pg_publication_tables
      WHERE pubname = 'supabase_realtime'
        AND schemaname = 'public'
        AND tablename = t
    ) THEN
      EXECUTE format('ALTER PUBLICATION supabase_realtime ADD TABLE public.%I', t);
    END IF;
  END LOOP;
END;
$$;
