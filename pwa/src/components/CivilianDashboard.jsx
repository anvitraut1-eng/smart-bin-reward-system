import { useState, useEffect } from 'react';
import { supabase } from '../lib/supabase';

function CivilianDashboard({ profile, onLogout }) {
  const [citizenData, setCitizenData] = useState(null);
  const [rewardHistory, setRewardHistory] = useState([]);
  const [redemptions, setRedemptions] = useState([]);
  const [loading, setLoading] = useState(true);

  useEffect(() => {
    if (profile.card_uid) {
      fetchCitizenData();
      fetchRewardHistory();
      fetchRedemptions();

      // Subscribe to realtime updates
      const channel = supabase
        .channel(`civilian-${profile.card_uid}`)
        .on(
          'postgres_changes',
          {
            event: '*',
            schema: 'public',
            table: 'citizens',
            filter: `card_uid=eq.${profile.card_uid}`
          },
          () => fetchCitizenData()
        )
        .on(
          'postgres_changes',
          {
            event: 'INSERT',
            schema: 'public',
            table: 'reward_events',
            filter: `card_uid=eq.${profile.card_uid}`
          },
          () => {
            fetchCitizenData();
            fetchRewardHistory();
          }
        )
        .subscribe();

      return () => {
        supabase.removeChannel(channel);
      };
    }
  }, [profile.card_uid]);

  const fetchCitizenData = async () => {
    try {
      const { data, error } = await supabase
        .from('citizens')
        .select('*')
        .eq('card_uid', profile.card_uid)
        .single();

      if (error && error.code !== 'PGRST116') throw error;
      setCitizenData(data || { card_uid: profile.card_uid, points_balance: 0 });
    } catch (error) {
      console.error('Error fetching citizen data:', error);
    } finally {
      setLoading(false);
    }
  };

  const fetchRewardHistory = async () => {
    try {
      const { data, error } = await supabase
        .from('reward_events')
        .select('*')
        .eq('card_uid', profile.card_uid)
        .order('timestamp', { ascending: false })
        .limit(20);

      if (error) throw error;
      setRewardHistory(data || []);
    } catch (error) {
      console.error('Error fetching reward history:', error);
    }
  };

  const fetchRedemptions = async () => {
    try {
      const { data, error } = await supabase
        .from('redemptions')
        .select('*')
        .eq('card_uid', profile.card_uid)
        .order('redeemed_at', { ascending: false })
        .limit(10);

      if (error) throw error;
      setRedemptions(data || []);
    } catch (error) {
      console.error('Error fetching redemptions:', error);
    }
  };

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

  if (loading) {
    return <div className="loading">Loading your data...</div>;
  }

  return (
    <div className="civilian-dashboard">
      <header className="dashboard-header">
        <div className="header-content">
          <div>
            <h1>Welcome, {profile.full_name}!</h1>
            <p className="user-role">Civilian Account</p>
          </div>
          <button className="logout-btn" onClick={onLogout}>
            Logout
          </button>
        </div>
      </header>

      <main className="dashboard-main">
        {/* Points Balance Card */}
        <div className="points-card">
          <div className="points-header">
            <h2>Your Points Balance</h2>
            <span className="rfid-badge">RFID: {profile.card_uid}</span>
          </div>
          <div className="points-display">
            <div className="points-value">{citizenData?.points_balance || 0}</div>
            <div className="points-label">points</div>
          </div>
          <p className="points-hint">
            Tap your RFID card at any smart bin when disposing waste to earn 10 points per disposal!
          </p>
        </div>

        {/* Stats Grid */}
        <div className="stats-grid">
          <div className="stat-card">
            <h3>Total Disposals</h3>
            <p className="stat-value">
              {rewardHistory.filter(r => r.confidence === 'confirmed').length}
            </p>
          </div>
          <div className="stat-card">
            <h3>Total Points Earned</h3>
            <p className="stat-value">
              {rewardHistory.reduce((sum, r) => sum + r.points_awarded, 0)}
            </p>
          </div>
          <div className="stat-card">
            <h3>Points Redeemed</h3>
            <p className="stat-value">
              {redemptions.reduce((sum, r) => sum + r.points_spent, 0)}
            </p>
          </div>
          <div className="stat-card">
            <h3>Success Rate</h3>
            <p className="stat-value">
              {rewardHistory.length > 0
                ? Math.round((rewardHistory.filter(r => r.confidence === 'confirmed').length / rewardHistory.length) * 100)
                : 0}%
            </p>
          </div>
        </div>

        {/* Recent Reward Activity */}
        <div className="activity-section">
          <h3>Recent Activity</h3>
          {rewardHistory.length > 0 ? (
            <div className="activity-list">
              {rewardHistory.map((reward) => (
                <div key={reward.id} className="activity-card">
                  <div className="activity-header">
                    {getConfidenceBadge(reward.confidence)}
                    <span className="activity-time">
                      {new Date(reward.timestamp).toLocaleString()}
                    </span>
                  </div>
                  <div className="activity-details">
                    <span>Bin: {reward.device_id}</span>
                    <span>
                      Fill: {Math.round(reward.fill_pct_before)}% → {Math.round(reward.fill_pct_after)}%
                    </span>
                    <span className="points-earned">
                      {reward.points_awarded > 0 ? '+' : ''}
                      {reward.points_awarded} pts
                    </span>
                  </div>
                </div>
              ))}
            </div>
          ) : (
            <p className="no-data">No activity yet. Start disposing waste to earn points!</p>
          )}
        </div>

        {/* Redemption History */}
        {redemptions.length > 0 && (
          <div className="redemption-section">
            <h3>Redemption History</h3>
            <div className="redemption-list">
              {redemptions.map((redemption) => (
                <div key={redemption.id} className="redemption-card">
                  <div className="redemption-header">
                    <span className="redemption-code">{redemption.redemption_code}</span>
                    <span className="redemption-time">
                      {new Date(redemption.redeemed_at).toLocaleString()}
                    </span>
                  </div>
                  <div className="redemption-points">
                    {redemption.points_spent} points redeemed
                  </div>
                </div>
              ))}
            </div>
          </div>
        )}
      </main>
    </div>
  );
}

export default CivilianDashboard;
