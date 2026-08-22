import { useState, useEffect } from 'react';
import { supabase } from '../lib/supabase';

function AdminAudit({ onBack }) {
  const [rewardEvents, setRewardEvents] = useState([]);
  const [filteredEvents, setFilteredEvents] = useState([]);
  const [loading, setLoading] = useState(true);
  const [filterConfidence, setFilterConfidence] = useState('all');
  const [stats, setStats] = useState({
    total: 0,
    confirmed: 0,
    no_disposal: 0,
    rate_limited: 0,
    total_points: 0
  });

  useEffect(() => {
    fetchRewardEvents();

    // Subscribe to realtime updates
    const channel = supabase
      .channel('admin-audit')
      .on(
        'postgres_changes',
        { event: 'INSERT', schema: 'public', table: 'reward_events' },
        () => fetchRewardEvents()
      )
      .subscribe();

    return () => {
      supabase.removeChannel(channel);
    };
  }, []);

  useEffect(() => {
    if (filterConfidence === 'all') {
      setFilteredEvents(rewardEvents);
    } else {
      setFilteredEvents(rewardEvents.filter((e) => e.confidence === filterConfidence));
    }
  }, [filterConfidence, rewardEvents]);

  const fetchRewardEvents = async () => {
    try {
      const { data, error } = await supabase
        .from('reward_events')
        .select('*')
        .order('timestamp', { ascending: false })
        .limit(100);

      if (error) throw error;

      setRewardEvents(data || []);

      // Calculate stats
      const total = data?.length || 0;
      const confirmed = data?.filter((e) => e.confidence === 'confirmed').length || 0;
      const no_disposal = data?.filter((e) => e.confidence === 'no_disposal').length || 0;
      const rate_limited = data?.filter((e) => e.confidence === 'rate_limited').length || 0;
      const total_points = data?.reduce((sum, e) => sum + e.points_awarded, 0) || 0;

      setStats({ total, confirmed, no_disposal, rate_limited, total_points });
    } catch (error) {
      console.error('Error fetching reward events:', error);
    } finally {
      setLoading(false);
    }
  };

  const getConfidenceColor = (confidence) => {
    switch (confidence) {
      case 'confirmed':
        return '#44ff44';
      case 'no_disposal':
        return '#ffaa00';
      case 'rate_limited':
        return '#ff4444';
      default:
        return '#888888';
    }
  };

  if (loading) {
    return <div className="loading">Loading audit data...</div>;
  }

  return (
    <div className="admin-audit">
      <button className="back-btn" onClick={onBack}>
        ← Back
      </button>

      <div className="audit-header">
        <h2>Admin Audit - Reward Events</h2>
        <p>Monitor all reward taps across the fleet for fraud detection and system health</p>
      </div>

      <div className="stats-grid">
        <div className="stat-card">
          <h4>Total Events</h4>
          <p className="stat-value">{stats.total}</p>
        </div>
        <div className="stat-card success">
          <h4>Confirmed</h4>
          <p className="stat-value">{stats.confirmed}</p>
          <p className="stat-pct">
            {stats.total > 0 ? Math.round((stats.confirmed / stats.total) * 100) : 0}%
          </p>
        </div>
        <div className="stat-card warning">
          <h4>No Disposal</h4>
          <p className="stat-value">{stats.no_disposal}</p>
          <p className="stat-pct">
            {stats.total > 0 ? Math.round((stats.no_disposal / stats.total) * 100) : 0}%
          </p>
        </div>
        <div className="stat-card error">
          <h4>Rate Limited</h4>
          <p className="stat-value">{stats.rate_limited}</p>
          <p className="stat-pct">
            {stats.total > 0 ? Math.round((stats.rate_limited / stats.total) * 100) : 0}%
          </p>
        </div>
        <div className="stat-card points">
          <h4>Total Points Awarded</h4>
          <p className="stat-value">{stats.total_points}</p>
        </div>
      </div>

      <div className="filter-section">
        <label>Filter by Confidence:</label>
        <select value={filterConfidence} onChange={(e) => setFilterConfidence(e.target.value)}>
          <option value="all">All Events</option>
          <option value="confirmed">Confirmed Only</option>
          <option value="no_disposal">No Disposal</option>
          <option value="rate_limited">Rate Limited</option>
        </select>
        <span className="result-count">
          Showing {filteredEvents.length} of {rewardEvents.length} events
        </span>
      </div>

      <div className="events-table-container">
        {filteredEvents.length > 0 ? (
          <table className="events-table">
            <thead>
              <tr>
                <th>Timestamp</th>
                <th>Card UID</th>
                <th>Bin</th>
                <th>Fill Change</th>
                <th>Points</th>
                <th>Confidence</th>
              </tr>
            </thead>
            <tbody>
              {filteredEvents.map((event) => (
                <tr key={event.id}>
                  <td className="timestamp">
                    {new Date(event.timestamp).toLocaleString()}
                  </td>
                  <td className="card-uid">{event.card_uid}</td>
                  <td className="device-id">{event.device_id}</td>
                  <td className="fill-change">
                    {Math.round(event.fill_pct_before)}% → {Math.round(event.fill_pct_after)}%
                    <span
                      className={
                        event.fill_pct_after > event.fill_pct_before ? 'rise' : 'drop'
                      }
                    >
                      {' '}
                      (
                      {event.fill_pct_after > event.fill_pct_before ? '+' : ''}
                      {Math.round(event.fill_pct_after - event.fill_pct_before)}%)
                    </span>
                  </td>
                  <td className="points">
                    {event.points_awarded > 0 ? '+' : ''}
                    {event.points_awarded}
                  </td>
                  <td>
                    <span
                      className="confidence-badge"
                      style={{ backgroundColor: getConfidenceColor(event.confidence) }}
                    >
                      {event.confidence}
                    </span>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        ) : (
          <div className="no-data">No reward events match the selected filter</div>
        )}
      </div>

      <div className="audit-notes">
        <h4>Audit Notes:</h4>
        <ul>
          <li>
            <strong>Confirmed:</strong> Disposal detected (fill rise + vibration) within 10s
            window. Points awarded.
          </li>
          <li>
            <strong>No Disposal:</strong> Tap detected but no fill rise within window. May
            indicate accidental tap or fraud attempt.
          </li>
          <li>
            <strong>Rate Limited:</strong> Same card tapped at same bin within 5 minutes. No
            points awarded.
          </li>
          <li>
            High "no disposal" or "rate limited" rates for a specific card may indicate
            fraudulent behavior.
          </li>
          <li>Monitor for cards with unusually high reward frequencies across bins.</li>
        </ul>
      </div>
    </div>
  );
}

export default AdminAudit;
