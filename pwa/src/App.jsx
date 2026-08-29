import { useState, useEffect } from 'react';
import { supabase } from './lib/supabase';
import Login from './components/Login';
import Register from './components/Register';
import CivilianDashboard from './components/CivilianDashboard';
import AdminDashboard from './components/AdminDashboard';
import InstallButton from './components/InstallButton';
import './App.css';

function App() {
  const [user, setUser] = useState(null);
  const [profile, setProfile] = useState(null);
  const [loading, setLoading] = useState(true);
  const [view, setView] = useState('login');

  useEffect(() => {
    supabase.auth.getSession().then(({ data: { session } }) => {
      setUser(session?.user ?? null);
      if (session?.user) fetchProfile(session.user.id);
      else setLoading(false);
    });

    const { data: { subscription } } = supabase.auth.onAuthStateChange((_event, session) => {
      setUser(session?.user ?? null);
      if (session?.user) fetchProfile(session.user.id);
      else {
        setProfile(null);
        setLoading(false);
      }
    });

    return () => subscription.unsubscribe();
  }, []);

  const fetchProfile = async (userId) => {
    if (!userId) {
      setLoading(false);
      return;
    }

    try {
      const { data, error } = await supabase
        .from('profiles')
        .select('*')
        .eq('id', userId)
        .single();

      if (error) throw error;
      setProfile(data);
    } catch (error) {
      console.error('Error fetching profile:', error);
      setProfile(null);
    } finally {
      setLoading(false);
    }
  };

  const handleLogout = async () => {
    await supabase.auth.signOut();
    setUser(null);
    setProfile(null);
    setView('login');
  };

  if (loading) {
    return (
      <div className="loading-screen">
        <div className="spinner"></div>
        <p>Loading...</p>
      </div>
    );
  }

  if (!user || !profile) {
    return (
      <div className="auth-container">
        <InstallButton />
        <div className="auth-card">
          <div className="auth-header">
            <h1>🗑️ Smart Bin System</h1>
            <p>Municipal Waste Management & Rewards</p>
          </div>
          {view === 'login' ? (
            <Login onSuccess={fetchProfile} onSwitchToRegister={() => setView('register')} />
          ) : (
            <Register onSuccess={() => setView('login')} onSwitchToLogin={() => setView('login')} />
          )}
        </div>
      </div>
    );
  }

  return (
    <div className="app">
      <InstallButton />
      {profile.account_type === 'admin' ? (
        <AdminDashboard profile={profile} onLogout={handleLogout} />
      ) : (
        <CivilianDashboard profile={profile} onLogout={handleLogout} />
      )}
    </div>
  );
}

export default App;
