import { createClient } from '@supabase/supabase-js';

const supabaseUrl = 'https://bpecehlmvzuirxmruvyt.supabase.co';
const supabaseAnonKey = 'eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImJwZWNlaGxtdnp1aXJ4bXJ1dnl0Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODY4OTEzMzMsImV4cCI6MjEwMjQ2NzMzM30.LyOps1xtjd1mPVc7OL1e6xLVW6Uu7J7RSPiTEZbZJyU';

export const supabase = createClient(supabaseUrl, supabaseAnonKey);
