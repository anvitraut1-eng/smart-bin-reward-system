import { useState } from 'react';
import { supabase } from '../lib/supabase';

function RedeemFlow({ onBack }) {
  const [cardUID, setCardUID] = useState('');
  const [pointsToRedeem, setPointsToRedeem] = useState('');
  const [currentBalance, setCurrentBalance] = useState(null);
  const [redemptionCode, setRedemptionCode] = useState(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState(null);

  const checkBalance = async () => {
    if (!cardUID.trim()) return;

    setLoading(true);
    setError(null);

    try {
      const { data, error: fetchError } = await supabase
        .from('citizens')
        .select('points_balance')
        .eq('card_uid', cardUID.toUpperCase())
        .single();

      if (fetchError || !data) {
        setError('Card UID not found');
        setCurrentBalance(null);
      } else {
        setCurrentBalance(data.points_balance);
      }
    } catch (err) {
      console.error('Error checking balance:', err);
      setError('Failed to check balance');
    } finally {
      setLoading(false);
    }
  };

  const redeemPoints = async () => {
    if (!cardUID.trim() || !pointsToRedeem || pointsToRedeem <= 0) {
      setError('Please enter valid card UID and points amount');
      return;
    }

    setLoading(true);
    setError(null);

    try {
      // Call the redeem_points function
      const { data, error: redeemError } = await supabase.rpc('redeem_points', {
        p_card_uid: cardUID.toUpperCase(),
        p_points_spent: parseInt(pointsToRedeem)
      });

      if (redeemError) throw redeemError;

      const result = data[0];

      if (result.success) {
        setRedemptionCode(result.redemption_code);
        setCurrentBalance(result.new_balance);
        setPointsToRedeem('');
      } else {
        setError(result.error_message);
      }
    } catch (err) {
      console.error('Error redeeming points:', err);
      setError('Redemption failed: ' + (err.message || 'Unknown error'));
    } finally {
      setLoading(false);
    }
  };

  const resetForm = () => {
    setCardUID('');
    setPointsToRedeem('');
    setCurrentBalance(null);
    setRedemptionCode(null);
    setError(null);
  };

  if (redemptionCode) {
    return (
      <div className="redeem-flow">
        <button className="back-btn" onClick={onBack}>
          ← Back
        </button>

        <div className="redemption-success">
          <div className="success-icon">✓</div>
          <h2>Redemption Successful!</h2>

          <div className="redemption-code-display">
            <p className="code-label">Show this code at the shop counter:</p>
            <div className="redemption-code">{redemptionCode}</div>
          </div>

          <div className="redemption-details">
            <p>
              <strong>Card UID:</strong> {cardUID}
            </p>
            <p>
              <strong>Points Redeemed:</strong> {pointsToRedeem}
            </p>
            <p>
              <strong>New Balance:</strong> {currentBalance} pts
            </p>
          </div>

          <button className="primary-btn" onClick={resetForm}>
            Redeem Another
          </button>
        </div>
      </div>
    );
  }

  return (
    <div className="redeem-flow">
      <button className="back-btn" onClick={onBack}>
        ← Back
      </button>

      <div className="redeem-header">
        <h2>Redeem Points</h2>
        <p className="warning-text">
          ⚠️ v1 Placeholder: This generates a code for manual validation at a shop counter.
          No real payment integration in this version.
        </p>
      </div>

      <div className="redeem-form">
        <div className="form-section">
          <label>Card UID</label>
          <div className="input-group">
            <input
              type="text"
              placeholder="Enter Card UID"
              value={cardUID}
              onChange={(e) => {
                setCardUID(e.target.value.toUpperCase());
                setCurrentBalance(null);
                setError(null);
              }}
            />
            <button onClick={checkBalance} disabled={loading}>
              Check Balance
            </button>
          </div>
        </div>

        {currentBalance !== null && (
          <div className="balance-display">
            <p className="balance-label">Current Balance:</p>
            <p className="balance-value">{currentBalance} points</p>
          </div>
        )}

        {error && <div className="error-message">{error}</div>}

        <div className="form-section">
          <label>Points to Redeem</label>
          <input
            type="number"
            placeholder="Enter amount"
            value={pointsToRedeem}
            onChange={(e) => setPointsToRedeem(e.target.value)}
            min="1"
            max={currentBalance || undefined}
          />
        </div>

        <div className="quick-amounts">
          <p>Quick select:</p>
          <div className="quick-btns">
            {[10, 25, 50, 100].map((amount) => (
              <button
                key={amount}
                onClick={() => setPointsToRedeem(amount.toString())}
                disabled={!currentBalance || currentBalance < amount}
                className="quick-btn"
              >
                {amount} pts
              </button>
            ))}
          </div>
        </div>

        <button
          className="redeem-btn"
          onClick={redeemPoints}
          disabled={
            loading ||
            !cardUID ||
            !pointsToRedeem ||
            pointsToRedeem <= 0 ||
            currentBalance === null ||
            parseInt(pointsToRedeem) > currentBalance
          }
        >
          {loading ? 'Processing...' : 'Redeem Points'}
        </button>

        <div className="redemption-info">
          <h4>How Redemption Works:</h4>
          <ul>
            <li>Enter the citizen's card UID and check their balance</li>
            <li>Choose the amount of points to redeem</li>
            <li>An 8-character redemption code will be generated</li>
            <li>Show the code to the shop counter for validation</li>
            <li>Points are deducted immediately and cannot be reversed</li>
          </ul>
        </div>
      </div>
    </div>
  );
}

export default RedeemFlow;
