-- Run after schema.sql.
-- Makes reward processing, pending-card expiry, permissions and admin reset
-- consistent with the current ESP32/PWA flow.

-- schema.sql has an older AFTER INSERT card-registration trigger. Replace it
-- with a BEFORE trigger so known cards are confirmed before the points trigger.
CREATE OR REPLACE FUNCTION public.check_card_registration_atomic()
RETURNS trigger AS $$
DECLARE
  card_exists BOOLEAN;
  pending_exists BOOLEAN;
BEGIN
  IF NEW.confidence NOT IN ('confirmed', 'no_disposal', 'rate_limited', 'pending_link') THEN
    NEW.confidence := 'pending_link';
    NEW.points_awarded := 0;
  END IF;

  IF NEW.confidence = 'pending_link' THEN
    SELECT EXISTS (
      SELECT 1 FROM public.citizens WHERE card_uid = NEW.card_uid AND user_id IS NOT NULL
    ) INTO card_exists;

    IF card_exists THEN
      NEW.confidence := 'confirmed';
      NEW.points_awarded := 10;
    ELSE
      NEW.points_awarded := 0;
      SELECT EXISTS (
        SELECT 1 FROM public.pending_card_links
        WHERE card_uid = NEW.card_uid AND claimed = false AND expires_at > NOW()
      ) INTO pending_exists;

      IF NOT pending_exists THEN
        INSERT INTO public.pending_card_links (card_uid, device_id, timestamp)
        VALUES (NEW.card_uid, NEW.device_id, NEW.timestamp);
      END IF;
    END IF;
  ELSIF NEW.confidence IN ('no_disposal', 'rate_limited') THEN
    NEW.points_awarded := 0;
  END IF;

  RETURN NEW;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER SET search_path = public, pg_temp;

DROP TRIGGER IF EXISTS trigger_check_card_registration ON public.reward_events;
DROP TRIGGER IF EXISTS trigger_check_card_registration_atomic ON public.reward_events;
CREATE TRIGGER trigger_check_card_registration_atomic
  BEFORE INSERT ON public.reward_events
  FOR EACH ROW EXECUTE FUNCTION public.check_card_registration_atomic();

-- Pending links are shown in the PWA for up to 24 hours, matching the UI.
CREATE OR REPLACE FUNCTION public.ensure_pending_card_expires_soon()
RETURNS trigger AS $$
BEGIN
  IF TG_OP = 'INSERT' OR (TG_OP = 'UPDATE' AND NEW.expires_at IS DISTINCT FROM OLD.expires_at) THEN
    NEW.expires_at := LEAST(NOW() + INTERVAL '24 hours', NEW.expires_at);
  END IF;
  RETURN NEW;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER SET search_path = public, pg_temp;

-- The firmware uses preconfigured devices; anonymous clients do not need to
-- create or modify device rows.
DROP POLICY IF EXISTS devices_anon_insert ON public.devices;
DROP POLICY IF EXISTS devices_anon_update ON public.devices;
REVOKE INSERT, UPDATE ON public.devices FROM anon;

CREATE OR REPLACE FUNCTION public.record_reward(
  p_card_uid TEXT,
  p_device_id TEXT,
  p_fill_before NUMERIC,
  p_fill_after NUMERIC,
  p_points INTEGER DEFAULT 10
)
RETURNS TABLE(success BOOLEAN, confidence TEXT, points_awarded INTEGER, card_linked BOOLEAN, error_message TEXT)
AS $$
DECLARE
  v_confidence TEXT;
  v_points INTEGER;
BEGIN
  IF p_card_uid IS NULL OR length(trim(p_card_uid)) = 0 THEN
    RETURN QUERY SELECT FALSE, NULL::TEXT, 0, FALSE, 'Missing card UID'; RETURN;
  END IF;

  IF NOT EXISTS (SELECT 1 FROM public.devices WHERE device_id = p_device_id) THEN
    RETURN QUERY SELECT FALSE, NULL::TEXT, 0, FALSE, 'Unknown device'; RETURN;
  END IF;

  IF p_fill_after < p_fill_before OR p_fill_after - p_fill_before < 2 THEN
    RETURN QUERY SELECT FALSE, NULL::TEXT, 0, FALSE, 'Disposal threshold not met'; RETURN;
  END IF;

  INSERT INTO public.reward_events
    (card_uid, device_id, fill_pct_before, fill_pct_after, weight_estimate_kg, points_awarded, confidence, timestamp)
  VALUES
    (p_card_uid, p_device_id, p_fill_before, p_fill_after, 0, 0, 'pending_link', NOW())
  RETURNING public.reward_events.confidence, public.reward_events.points_awarded
  INTO v_confidence, v_points;

  RETURN QUERY SELECT TRUE, v_confidence, v_points, (v_confidence = 'confirmed'), NULL::TEXT;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER SET search_path = public, pg_temp;

REVOKE ALL ON FUNCTION public.record_reward(TEXT, TEXT, NUMERIC, NUMERIC, INTEGER) FROM PUBLIC, authenticated;
GRANT EXECUTE ON FUNCTION public.record_reward(TEXT, TEXT, NUMERIC, NUMERIC, INTEGER) TO anon;
REVOKE INSERT ON public.reward_events FROM anon;

-- Remove stale overloads left by earlier versions. The PWA uses the 2-arg RPC.
DROP FUNCTION IF EXISTS public.claim_pending_card(BIGINT);
DROP FUNCTION IF EXISTS public.claim_pending_card(UUID, TEXT, TEXT);

CREATE OR REPLACE FUNCTION public.reset_project_data()
RETURNS TABLE(success BOOLEAN, error_message TEXT)
AS $$
BEGIN
  IF auth.uid() IS NULL OR NOT public.is_admin() THEN
    RETURN QUERY SELECT FALSE, 'Not authorized'; RETURN;
  END IF;

  DELETE FROM public.redemptions WHERE TRUE;
  DELETE FROM public.reward_events WHERE TRUE;
  DELETE FROM public.pending_card_links WHERE TRUE;
  DELETE FROM public.empty_events WHERE TRUE;
  DELETE FROM public.bin_readings WHERE TRUE;
  DELETE FROM public.citizens WHERE TRUE;
  DELETE FROM public.devices WHERE TRUE;

  INSERT INTO public.devices (device_id, location) VALUES
    ('BIN_ESP32_001', 'Not Set'),
    ('BIN_ESP32_002', 'Not Set')
  ON CONFLICT (device_id) DO UPDATE SET location = EXCLUDED.location;

  UPDATE public.profiles SET card_uid = NULL WHERE card_uid IS NOT NULL;

  RETURN QUERY SELECT TRUE, NULL::TEXT;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER SET search_path = public, pg_temp;

REVOKE ALL ON FUNCTION public.reset_project_data() FROM PUBLIC, anon;
GRANT EXECUTE ON FUNCTION public.reset_project_data() TO authenticated;
