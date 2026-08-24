import { useState } from 'react';
import { supabase } from '../lib/supabase';

function Register({ onSuccess, onSwitchToLogin }) {
  const [formData, setFormData] = useState({
    email: '',
    password: '',
    confirmPassword: '',
    fullName: '',
    accountType: 'civilian',
    cardUID: '',
  });
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState(null);

  const handleChange = (e) => {
    setFormData({
      ...formData,
      [e.target.name]: e.target.value,
    });
  };

  const handleRegister = async (e) => {
    e.preventDefault();
    setError(null);

    // Validation
    if (formData.password !== formData.confirmPassword) {
      setError('Passwords do not match');
      return;
    }

    if (formData.password.length < 6) {
      setError('Password must be at least 6 characters');
      return;
    }

    if (formData.accountType === 'civilian' && !formData.cardUID.trim()) {
      setError('RFID Card UID is required for civilian accounts');
      return;
    }

    setLoading(true);

    try {
      // Sign up user
      const { data: authData, error: signUpError } = await supabase.auth.signUp({
        email: formData.email,
        password: formData.password,
        options: {
          data: {
            full_name: formData.fullName,
            account_type: formData.accountType,
          },
        },
      });

      if (signUpError) throw signUpError;

      // If civilian account, link RFID card
      if (formData.accountType === 'civilian' && authData.user) {
        const { error: linkError } = await supabase.rpc('link_rfid_to_user', {
          p_user_id: authData.user.id,
          p_card_uid: formData.cardUID.toUpperCase(),
        });

        if (linkError) {
          // If linking fails, still allow registration but show warning
          console.error('RFID linking error:', linkError);
          setError(`Account created but RFID linking failed: ${linkError.message}. Contact admin.`);
        }
      }

      // Success
      alert('Registration successful! Please check your email to verify your account, then login.');
      onSuccess();
    } catch (error) {
      console.error('Registration error:', error);
      setError(error.message);
    } finally {
      setLoading(false);
    }
  };

  return (
    <div className="auth-form">
      <h2>Register</h2>

      {error && <div className="error-message">{error}</div>}

      <form onSubmit={handleRegister}>
        <div className="form-group">
          <label>Account Type</label>
          <select name="accountType" value={formData.accountType} onChange={handleChange}>
            <option value="civilian">Civilian Account</option>
            <option value="admin">Admin Account</option>
          </select>
          <p className="help-text">
            {formData.accountType === 'civilian'
              ? 'For citizens earning points by disposing waste'
              : 'For municipal staff monitoring bins'}
          </p>
        </div>

        <div className="form-group">
          <label>Full Name</label>
          <input
            type="text"
            name="fullName"
            value={formData.fullName}
            onChange={handleChange}
            placeholder="John Doe"
            required
          />
        </div>

        <div className="form-group">
          <label>Email</label>
          <input
            type="email"
            name="email"
            value={formData.email}
            onChange={handleChange}
            placeholder="your@email.com"
            required
          />
        </div>

        {formData.accountType === 'civilian' && (
          <div className="form-group">
            <label>RFID Card UID</label>
            <input
              type="text"
              name="cardUID"
              value={formData.cardUID}
              onChange={handleChange}
              placeholder="ABCD1234 (from your RFID card)"
              required
            />
            <p className="help-text">
              Enter the unique ID from your RFID card. You'll use this card at bins to earn points.
            </p>
          </div>
        )}

        <div className="form-group">
          <label>Password</label>
          <input
            type="password"
            name="password"
            value={formData.password}
            onChange={handleChange}
            placeholder="••••••••"
            required
          />
        </div>

        <div className="form-group">
          <label>Confirm Password</label>
          <input
            type="password"
            name="confirmPassword"
            value={formData.confirmPassword}
            onChange={handleChange}
            placeholder="••••••••"
            required
          />
        </div>

        <button type="submit" className="primary-btn" disabled={loading}>
          {loading ? 'Registering...' : 'Register'}
        </button>
      </form>

      <p className="auth-switch">
        Already have an account?{' '}
        <button onClick={onSwitchToLogin} className="link-btn">
          Login here
        </button>
      </p>
    </div>
  );
}

export default Register;
