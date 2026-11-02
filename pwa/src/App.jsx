import { useState, useEffect } from 'react';
import { supabase } from './lib/supabase';
import Login from './components/Login';
import Register from './components/Register';
import ResetPassword from './components/ResetPassword';
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

    const { data: { subscription } } = supabase.auth.onAuthStateChange((event, session) => {
      setUser(session?.user ?? null);
      if (event === 'PASSWORD_RECOVERY') {
        setView('reset-password');
        setLoading(false);
        return;
      }
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

  if (view === 'reset-password') {
    return (
      <div className="auth-container">
        <InstallButton />
        <div className="auth-card">
          <div className="auth-header">
            <h1>🗑️ Smart Bin System</h1>
            <p>Municipal Waste Management & Rewards</p>
          </div>
          <ResetPassword onDone={() => setView('login')} />
        </div>
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
            <Login
              onSuccess={fetchProfile}
              onSwitchToRegister={() => setView('register')}
              onForgotPassword={() => setView('forgot-password')}
            />
          ) : view === 'forgot-password' ? (
            <div className="auth-form">
              <h2>Reset password</h2>
              <p className="help-text">Enter your account email and we'll send you a password reset link.</p>
              <ForgotPassword onBack={() => setView('login')} />
            </div>
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

function ForgotPassword({ onBack }) {
  const [email, setEmail] = useState('');
  const [loading, setLoading] = useState(false);
  const [message, setMessage] = useState('');
  const [error, setError] = useState('');

  const handleSubmit = async (event) => {
    event.preventDefault();
    setLoading(true);
    setMessage('');
    setError('');
    try {
      const { error } = await supabase.auth.resetPasswordForEmail(email.trim(), {
        redirectTo: window.location.origin,
      });
      if (error) throw error;
      setMessage('If an account exists for that email, a password reset link has been sent. Check your inbox and spam folder.');
    } catch (err) {
      setError(err.message || 'Could not send the reset email. Please try again.');
    } finally {
      setLoading(false);
    }
  };

  return (
    <>
      {error && <div className="error-message">{error}</div>}
      {message && <div className="info-banner"><p>{message}</p></div>}
      <form onSubmit={handleSubmit}>
        <div className="form-group">
          <label>Email</label>
          <input type="email" value={email} onChange={(e) => setEmail(e.target.value)} placeholder="your@email.com" autoComplete="email" required />
        </div>
        <button type="submit" className="primary-btn" disabled={loading}>
          {loading ? 'Sending...' : 'Send reset link'}
        </button>
      </form>
      <p className="auth-switch">
        <button type="button" onClick={onBack} className="link-btn">Back to login</button>
      </p>
    </>
  );
}

export default App;
