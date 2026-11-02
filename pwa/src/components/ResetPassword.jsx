import { useState } from 'react';
import { supabase } from '../lib/supabase';

function ResetPassword({ onDone }) {
  const [password, setPassword] = useState('');
  const [confirmPassword, setConfirmPassword] = useState('');
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState('');
  const [success, setSuccess] = useState('');

  const handleSubmit = async (event) => {
    event.preventDefault();
    setError('');
    setSuccess('');

    if (password.length < 6) {
      setError('Password must be at least 6 characters long.');
      return;
    }
    if (password !== confirmPassword) {
      setError('The passwords do not match.');
      return;
    }

    setLoading(true);
    try {
      const { error } = await supabase.auth.updateUser({ password });
      if (error) throw error;
      setSuccess('Your password has been updated. You can now log in with your new password.');
      await supabase.auth.signOut();
      window.setTimeout(onDone, 1200);
    } catch (err) {
      setError(err.message || 'Could not update your password. Open the reset link again and retry.');
    } finally {
      setLoading(false);
    }
  };

  return (
    <div className="auth-form">
      <h2>Choose a new password</h2>
      {error && <div className="error-message">{error}</div>}
      {success && <div className="info-banner"><p>{success}</p></div>}
      <form onSubmit={handleSubmit}>
        <div className="form-group">
          <label>New password</label>
          <input type="password" value={password} onChange={(e) => setPassword(e.target.value)} autoComplete="new-password" required />
        </div>
        <div className="form-group">
          <label>Confirm new password</label>
          <input type="password" value={confirmPassword} onChange={(e) => setConfirmPassword(e.target.value)} autoComplete="new-password" required />
        </div>
        <button type="submit" className="primary-btn" disabled={loading || !!success}>
          {loading ? 'Updating...' : 'Update password'}
        </button>
      </form>
    </div>
  );
}

export default ResetPassword;
