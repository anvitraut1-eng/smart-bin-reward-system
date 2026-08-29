-- Run this once in the Supabase SQL Editor.
-- Adds secure account-aware reward processing and an admin-only project reset.

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
  v_user_id UUID;
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

  SELECT c.user_id INTO v_user_id
  FROM public.citizens c
  WHERE c.card_uid = p_card_uid
  LIMIT 1;

  IF v_user_id IS NOT NULL THEN
    v_confidence := 'confirmed';
    v_points := LEAST(GREATEST(COALESCE(p_points, 10), 0), 10);
  ELSE
    v_confidence := 'pending_link';
    v_points := 0;
  END IF;

  INSERT INTO public.reward_events
    (card_uid, device_id, fill_pct_before, fill_pct_after, weight_estimate_kg, points_awarded, confidence, timestamp)
  VALUES
    (p_card_uid, p_device_id, p_fill_before, p_fill_after, 0, v_points, v_confidence, NOW());

  RETURN QUERY SELECT TRUE, v_confidence, v_points, (v_user_id IS NOT NULL), NULL::TEXT;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER SET search_path = public, pg_temp;

REVOKE ALL ON FUNCTION public.record_reward(TEXT, TEXT, NUMERIC, NUMERIC, INTEGER) FROM PUBLIC, authenticated;
GRANT EXECUTE ON FUNCTION public.record_reward(TEXT, TEXT, NUMERIC, NUMERIC, INTEGER) TO anon;

-- Firmware uses record_reward(), so anonymous clients no longer need direct
-- reward-event INSERT access.
REVOKE INSERT ON public.reward_events FROM anon;

CREATE OR REPLACE FUNCTION public.reset_project_data()
RETURNS TABLE(success BOOLEAN, error_message TEXT)
AS $$
BEGIN
  IF auth.uid() IS NULL OR NOT public.is_admin() THEN
    RETURN QUERY SELECT FALSE, 'Not authorized'; RETURN;
  END IF;

  -- Keep auth.users and profiles (the actual user accounts).
  DELETE FROM public.redemptions;
  DELETE FROM public.reward_events;
  DELETE FROM public.pending_card_links;
  DELETE FROM public.empty_events;
  DELETE FROM public.bin_readings;
  DELETE FROM public.citizens;
  DELETE FROM public.devices;

  -- Keep the two configured school-project bins so ESP32 FK inserts continue
  -- working immediately after a reset.
  INSERT INTO public.devices (device_id, location) VALUES
    ('BIN_ESP32_001', 'Not Set'),
    ('BIN_ESP32_002', 'Not Set')
  ON CONFLICT (device_id) DO UPDATE SET location = EXCLUDED.location;

  -- Keep the accounts, but clear their RFID associations so cards can be
  -- freshly claimed during the next test run.
  UPDATE public.profiles SET card_uid = NULL WHERE card_uid IS NOT NULL;

  RETURN QUERY SELECT TRUE, NULL::TEXT;
END;
$$ LANGUAGE plpgsql SECURITY DEFINER SET search_path = public, pg_temp;

REVOKE ALL ON FUNCTION public.reset_project_data() FROM PUBLIC, anon;
GRANT EXECUTE ON FUNCTION public.reset_project_data() TO authenticated;
