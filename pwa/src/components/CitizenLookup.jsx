import { useState, useEffect } from 'react';
import { supabase } from '../lib/supabase';

function CitizenLookup({ onBack }) {
  const [cardUID, setCardUID] = useState('');
  const [citizen, setCitizen] = useState(null);
  const [rewardHistory, setRewardHistory] = useState([]);
  const [loading, setLoading] = useState(false);
  const [notFound, setNotFound] = useState(false);

  const lookupCitizen = async (uid) => {
    if (!uid.trim()) return;

    setLoading(true);
    setNotFound(false);

    try {
      // Get citizen info
      const { data: citizenData, error: citizenError } = await supabase
        .from('citizens')
        .select('*')
        .eq('card_uid', uid.toUpperCase())
        .single();

      if (citizenError || !citizenData) {
        setNotFound(true);
        setCitizen(null);
        setRewardHistory([]);
      } else {
        setCitizen(citizenData);

        // Get reward history
        const { data: rewards } = await supabase
          .from('reward_events')
          .select('*')
          .eq('card_uid', uid.toUpperCase())
          .order('timestamp', { ascending: false })
          .limit(20);

        setRewardHistory(rewards || []);
      }
    } catch (error) {
      console.error('Error looking up citizen:', error);
      alert('Lookup failed');
    } finally {
      setLoading(false);
    }
  };

  useEffect(() => {
    if (citizen) {
      // Subscribe to realtime point updates
      const channel = supabase
        .channel(`citizen-${citizen.card_uid}`)
        .on(
          'postgres_changes',
          {
            event: 'UPDATE',
            schema: 'public',
            table: 'citizens',
            filter: `card_uid=eq.${citizen.card_uid}`
          },
          (payload) => {
            setCitizen(payload.new);
          }
        )
        .on(
          'postgres_changes',
          {
            event: 'INSERT',
            schema: 'public',
            table: 'reward_events',
            filter: `card_uid=eq.${citizen.card_uid}`
          },
          () => {
            lookupCitizen(citizen.card_uid);
          }
        )
        .subscribe();

      return () => {
        supabase.removeChannel(channel);
      };
    }
  }, [citizen]);

  const getConfidenceBadge = (confidence) => {
    const colors = {
      confirmed: '#44ff44',
      no_disposal: '#ffaa00',
      rate_limited: '#ff4444'
    };
    return (
      <span
        className="confidence-badge"
        style={{ backgroundColor: colors[confidence] || '#888' }}
      >
        {confidence}
      </span>
    );
  };

  return (
    <div className="citizen-lookup">
      <button className="back-btn" onClick={onBack}>
        ← Back
      </button>

      <div className="lookup-header">
        <h2>Citizen Lookup</h2>
        <p>Enter a card UID to view points balance and reward history</p>
      </div>

      <div className="lookup-form">
        <input
          type="text"
          placeholder="Enter Card UID (e.g., ABCD1234)"
          value={cardUID}
          onChange={(e) => setCardUID(e.target.value.toUpperCase())}
          onKeyPress={(e) => e.key === 'Enter' && lookupCitizen(cardUID)}
        />
        <button onClick={() => lookupCitizen(cardUID)} disabled={loading}>
          {loading ? 'Looking up...' : 'Lookup'}
        </button>
      </div>

      {notFound && (
        <div className="not-found">
          <p>No citizen found with card UID: {cardUID}</p>
          <p className="help-text">
            This card has not been used at any bin yet. Points are automatically created on first tap.
          </p>
        </div>
      )}

      {citizen && (
        <div className="citizen-info">
          <div className="citizen-card">
            <h3>Citizen Information</h3>
            <div className="info-row">
              <span className="label">Card UID:</span>
              <span className="value">{citizen.card_uid}</span>
            </div>
            <div className="info-row">
              <span className="label">Name:</span>
              <span className="value">{citizen.name || 'Not set'}</span>
            </div>
            <div className="info-row">
              <span className="label">Points Balance:</span>
              <span className="value points-balance">{citizen.points_balance} pts</span>
            </div>
            <div className="info-row">
              <span className="label">Member Since:</span>
              <span className="value">
                {new Date(citizen.created_at).toLocaleDateString()}
              </span>
            </div>
          </div>

          <div className="reward-history">
            <h3>Reward History</h3>
            {rewardHistory.length > 0 ? (
              <div className="rewards-list">
                {rewardHistory.map((reward) => (
                  <div key={reward.id} className="reward-card">
                    <div className="reward-header">
                      {getConfidenceBadge(reward.confidence)}
                      <span className="reward-time">
                        {new Date(reward.timestamp).toLocaleString()}
                      </span>
                    </div>
                    <div className="reward-details">
                      <span>Bin: {reward.device_id}</span>
                      <span>
                        Fill: {Math.round(reward.fill_pct_before)}% → {Math.round(reward.fill_pct_after)}%
                      </span>
                      <span className="points-awarded">
                        {reward.points_awarded > 0 ? '+' : ''}
                        {reward.points_awarded} pts
                      </span>
                    </div>
                  </div>
                ))}
              </div>
            ) : (
              <p className="no-data">No reward events yet</p>
            )}
          </div>
        </div>
      )}
    </div>
  );
}

export default CitizenLookup;
