import { useState } from 'react';
import { supabase } from '../lib/supabase';

function Login({ onSuccess, onSwitchToRegister, onForgotPassword }) {
  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState(null);

  const handleLogin = async (e) => {
    e.preventDefault();
    setError(null);
    setLoading(true);

    try {
      const { data, error } = await supabase.auth.signInWithPassword({
        email: email.trim(),
        password,
      });
      if (error) throw error;
      if (data.user) onSuccess(data.user.id);
    } catch (error) {
      console.error('Login error:', error);
      setError(error.message);
    } finally {
      setLoading(false);
    }
  };

  return (
    <div className="auth-form">
      <h2>Login</h2>
      {error && <div className="error-message">{error}</div>}

      <form onSubmit={handleLogin}>
        <div className="form-group">
          <label>Email</label>
          <input type="email" value={email} onChange={(e) => setEmail(e.target.value)} placeholder="your@email.com" autoComplete="email" required />
        </div>

        <div className="form-group">
          <label>Password</label>
          <input type="password" value={password} onChange={(e) => setPassword(e.target.value)} placeholder="••••••••" autoComplete="current-password" required />
        </div>

        <button type="submit" className="primary-btn" disabled={loading}>
          {loading ? 'Logging in...' : 'Login'}
        </button>
      </form>

      <p className="auth-switch">
        <button type="button" onClick={onForgotPassword} className="link-btn">Forgot password?</button>
      </p>

      <p className="auth-switch">
        Don't have an account?{' '}
        <button type="button" onClick={onSwitchToRegister} className="link-btn">Register here</button>
      </p>
    </div>
  );
}

export default Login;
