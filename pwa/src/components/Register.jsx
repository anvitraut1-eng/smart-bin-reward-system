import { useState } from 'react';
import { supabase } from '../lib/supabase';

function Register({ onSuccess, onSwitchToLogin }) {
  const [formData, setFormData] = useState({
    email: '',
    password: '',
    confirmPassword: '',
    fullName: '',
    accountType: 'civilian',
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

      // Success
      if (formData.accountType === 'civilian') {
        alert('Registration successful! Check your email to verify. After login, tap your RFID card at any bin to start earning points.');
      } else {
        alert('Admin registration successful! Please check your email to verify your account, then login.');
      }
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
          <div className="info-banner">
            <strong>📱 How to link your RFID card:</strong>
            <p>After registration and login, simply tap your RFID card at any smart bin. We'll automatically detect it and ask you to confirm linking it to your account. No need to type the card ID manually!</p>
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
