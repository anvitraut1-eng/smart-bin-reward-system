import { useState, useEffect } from 'react';
import { supabase } from '../lib/supabase';
import CardLinkModal from './CardLinkModal';

function CivilianDashboard({ profile, onLogout }) {
  const [citizenData, setCitizenData] = useState(null);
  const [rewardHistory, setRewardHistory] = useState([]);
  const [redemptions, setRedemptions] = useState([]);
  const [loading, setLoading] = useState(true);
  const [showCardLinkModal, setShowCardLinkModal] = useState(false);
  const [pendingCardsCount, setPendingCardsCount] = useState(0);

  useEffect(() => {
    let channels = [];
    let cancelled = false;

    const load = async () => {
      if (profile.card_uid) {
        await Promise.all([fetchCitizenData(), fetchRewardHistory(), fetchRedemptions()]);

        const channel = supabase
          .channel(`civilian-${profile.id}`)
          .on('postgres_changes', {
            event: '*', schema: 'public', table: 'citizens',
            filter: `card_uid=eq.${profile.card_uid}`
          }, fetchCitizenData)
          .on('postgres_changes', {
            event: 'INSERT', schema: 'public', table: 'reward_events',
            filter: `card_uid=eq.${profile.card_uid}`
          }, () => {
            fetchCitizenData();
            fetchRewardHistory();
          })
          .subscribe();
        channels.push(channel);
      } else {
        await checkPendingCards(true);

        // An unlinked civilian stays subscribed. When an ESP32 creates a
        // pending card link, the confirmation modal appears automatically.
        const channel = supabase
          .channel(`pending-cards-${profile.id}`)
          .on('postgres_changes', {
            event: 'INSERT', schema: 'public', table: 'pending_card_links'
          }, () => checkPendingCards(true))
          .on('postgres_changes', {
            event: 'UPDATE', schema: 'public', table: 'pending_card_links'
          }, () => checkPendingCards(false))
          .subscribe();
        channels.push(channel);
      }

      if (!cancelled) setLoading(false);
    };

    load();

    return () => {
      cancelled = true;
      channels.forEach((channel) => supabase.removeChannel(channel));
    };
  }, [profile.id, profile.card_uid]);

  const checkPendingCards = async (openModal = false) => {
    try {
      const { count, error } = await supabase
        .from('pending_card_links')
        .select('id', { count: 'exact', head: true })
        .eq('claimed', false)
        .gt('expires_at', new Date().toISOString());

      if (error) throw error;
      const pendingCount = count || 0;
      setPendingCardsCount(pendingCount);
      if (openModal && !profile.card_uid && pendingCount > 0) setShowCardLinkModal(true);
    } catch (err) {
      console.error('Error checking pending cards:', err);
    }
  };

  const fetchCitizenData = async () => {
    try {
      const { data, error } = await supabase.from('citizens').select('*').eq('card_uid', profile.card_uid).single();
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
      const { data, error } = await supabase.from('reward_events').select('*')
        .eq('card_uid', profile.card_uid).order('timestamp', { ascending: false }).limit(20);
      if (error) throw error;
      setRewardHistory(data || []);
    } catch (error) {
      console.error('Error fetching reward history:', error);
    }
  };

  const fetchRedemptions = async () => {
    try {
      const { data, error } = await supabase.from('redemptions').select('*')
        .eq('card_uid', profile.card_uid).order('redeemed_at', { ascending: false }).limit(10);
      if (error) throw error;
      setRedemptions(data || []);
    } catch (error) {
      console.error('Error fetching redemptions:', error);
    }
  };

  const getConfidenceBadge = (confidence) => {
    const colors = { confirmed: '#44ff44', no_disposal: '#ffaa00', rate_limited: '#ff4444', pending_link: '#4488ff' };
    return <span className="confidence-badge" style={{ backgroundColor: colors[confidence] || '#888' }}>{confidence}</span>;
  };

  if (loading) return <div className="loading">Loading your data...</div>;

  return (
    <div className="civilian-dashboard">
      <header className="dashboard-header">
        <div className="header-content">
          <div><h1>Welcome, {profile.full_name}!</h1><p className="user-role">Civilian Account</p></div>
          <button className="logout-btn" onClick={onLogout}>Logout</button>
        </div>
      </header>

      <main className="dashboard-main">
        {!profile.card_uid && (
          <div className="card-link-banner">
            <div className="banner-icon">🎴</div>
            <div className="banner-content">
              <h3>Link Your RFID Card</h3>
              <p>{pendingCardsCount > 0 ? `We detected ${pendingCardsCount} card tap${pendingCardsCount > 1 ? 's' : ''} waiting for confirmation!` : 'Tap your RFID card at any smart bin to start earning points.'}</p>
            </div>
            <button className="primary-btn" onClick={() => setShowCardLinkModal(true)}>{pendingCardsCount > 0 ? 'Link Card Now' : 'Learn More'}</button>
          </div>
        )}

        <div className="points-card">
          <div className="points-header"><h2>Your Points Balance</h2><span className="rfid-badge">{profile.card_uid ? `RFID: ${profile.card_uid}` : 'No card linked'}</span></div>
          <div className="points-display"><div className="points-value">{citizenData?.points_balance || 0}</div><div className="points-label">points</div></div>
          <p className="points-hint">{profile.card_uid ? 'Tap your RFID card at any smart bin when disposing waste to earn 10 points per disposal!' : 'Link your RFID card above to start earning points!'}</p>
        </div>

        <div className="stats-grid">
          <div className="stat-card"><h3>Total Disposals</h3><p className="stat-value">{rewardHistory.filter(r => r.confidence === 'confirmed').length}</p></div>
          <div className="stat-card"><h3>Total Points Earned</h3><p className="stat-value">{rewardHistory.reduce((sum, r) => sum + (r.points_awarded || 0), 0)}</p></div>
          <div className="stat-card"><h3>Points Redeemed</h3><p className="stat-value">{redemptions.reduce((sum, r) => sum + (r.points_spent || 0), 0)}</p></div>
          <div className="stat-card"><h3>Success Rate</h3><p className="stat-value">{rewardHistory.length > 0 ? Math.round((rewardHistory.filter(r => r.confidence === 'confirmed').length / rewardHistory.length) * 100) : 0}%</p></div>
        </div>

        <div className="activity-section"><h3>Recent Activity</h3>
          {rewardHistory.length > 0 ? <div className="activity-list">{rewardHistory.map((reward) => (
            <div key={reward.id} className="activity-card"><div className="activity-header">{getConfidenceBadge(reward.confidence)}<span className="activity-time">{new Date(reward.timestamp).toLocaleString()}</span></div>
              <div className="activity-details"><span>Bin: {reward.device_id}</span><span>Fill: {Math.round(reward.fill_pct_before)}% → {Math.round(reward.fill_pct_after)}%</span><span className="points-earned">{reward.points_awarded > 0 ? '+' : ''}{reward.points_awarded} pts</span></div>
            </div>))}</div> : <p className="no-data">No activity yet. Start disposing waste to earn points!</p>}
        </div>

        {redemptions.length > 0 && <div className="redemption-section"><h3>Redemption History</h3><div className="redemption-list">{redemptions.map((redemption) => (
          <div key={redemption.id} className="redemption-card"><div className="redemption-header"><span className="redemption-code">{redemption.redemption_code}</span><span className="redemption-time">{new Date(redemption.redeemed_at).toLocaleString()}</span></div><div className="redemption-points">{redemption.points_spent} points redeemed</div></div>
        ))}</div></div>}
      </main>

      {showCardLinkModal && <CardLinkModal userId={profile.id} onClose={() => { setShowCardLinkModal(false); checkPendingCards(false); }} />}
    </div>
  );
}

export default CivilianDashboard;
