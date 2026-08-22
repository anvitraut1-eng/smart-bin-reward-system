import { useState, useEffect } from 'react';
import { supabase } from '../lib/supabase';
import { LineChart, Line, XAxis, YAxis, CartesianGrid, Tooltip, ResponsiveContainer } from 'recharts';

function BinDetail({ deviceId, onBack }) {
  const [binInfo, setBinInfo] = useState(null);
  const [chartData, setChartData] = useState([]);
  const [emptyEvents, setEmptyEvents] = useState([]);
  const [loading, setLoading] = useState(true);

  useEffect(() => {
    fetchBinData();

    // Subscribe to realtime updates
    const channel = supabase
      .channel(`bin-${deviceId}`)
      .on(
        'postgres_changes',
        {
          event: 'INSERT',
          schema: 'public',
          table: 'bin_readings',
          filter: `device_id=eq.${deviceId}`
        },
        () => fetchBinData()
      )
      .on(
        'postgres_changes',
        {
          event: 'INSERT',
          schema: 'public',
          table: 'empty_events',
          filter: `device_id=eq.${deviceId}`
        },
        () => fetchBinData()
      )
      .subscribe();

    return () => {
      supabase.removeChannel(channel);
    };
  }, [deviceId]);

  const fetchBinData = async () => {
    try {
      // Get device info
      const { data: device } = await supabase
        .from('devices')
        .select('*')
        .eq('device_id', deviceId)
        .single();

      // Get last 24h readings for chart
      const oneDayAgo = new Date(Date.now() - 24 * 60 * 60 * 1000).toISOString();
      const { data: readings } = await supabase
        .from('bin_readings')
        .select('fill_pct, timestamp')
        .eq('device_id', deviceId)
        .gte('timestamp', oneDayAgo)
        .order('timestamp', { ascending: true });

      // Get empty events
      const { data: events } = await supabase
        .from('empty_events')
        .select('*')
        .eq('device_id', deviceId)
        .order('timestamp', { ascending: false })
        .limit(10);

      // Get latest reading for current status
      const { data: latest } = await supabase
        .from('bin_readings')
        .select('fill_pct, timestamp')
        .eq('device_id', deviceId)
        .order('timestamp', { ascending: false })
        .limit(1);

      setBinInfo({
        ...device,
        current_fill: latest?.[0]?.fill_pct || 0,
        last_seen: latest?.[0]?.timestamp
      });

      setChartData(
        readings?.map((r) => ({
          time: new Date(r.timestamp).toLocaleTimeString(),
          fill: r.fill_pct
        })) || []
      );

      setEmptyEvents(events || []);
    } catch (error) {
      console.error('Error fetching bin data:', error);
    } finally {
      setLoading(false);
    }
  };

  if (loading) {
    return <div className="loading">Loading bin details...</div>;
  }

  if (!binInfo) {
    return (
      <div className="error">
        <p>Bin not found</p>
        <button onClick={onBack}>Back to Fleet</button>
      </div>
    );
  }

  return (
    <div className="bin-detail">
      <button className="back-btn" onClick={onBack}>
        ← Back to Fleet
      </button>

      <div className="detail-header">
        <h2>{binInfo.device_id}</h2>
        <p className="location">📍 {binInfo.location}</p>
      </div>

      <div className="detail-cards">
        <div className="detail-card">
          <h3>Current Fill</h3>
          <div className="fill-gauge-large">
            <div className="gauge-bar">
              <div
                className="gauge-fill"
                style={{
                  width: `${binInfo.current_fill}%`,
                  backgroundColor:
                    binInfo.current_fill > 80
                      ? '#ff4444'
                      : binInfo.current_fill > 50
                      ? '#ffaa00'
                      : '#44ff44'
                }}
              />
            </div>
            <div className="gauge-value">{Math.round(binInfo.current_fill)}%</div>
          </div>
        </div>

        <div className="detail-card">
          <h3>Last Seen</h3>
          <p className="last-seen-time">
            {binInfo.last_seen
              ? new Date(binInfo.last_seen).toLocaleString()
              : 'Never'}
          </p>
        </div>
      </div>

      <div className="chart-section">
        <h3>24-Hour Fill History</h3>
        {chartData.length > 0 ? (
          <ResponsiveContainer width="100%" height={300}>
            <LineChart data={chartData}>
              <CartesianGrid strokeDasharray="3 3" stroke="#333" />
              <XAxis dataKey="time" stroke="#ccc" />
              <YAxis stroke="#ccc" domain={[0, 100]} />
              <Tooltip
                contentStyle={{ backgroundColor: '#222', border: '1px solid #444' }}
              />
              <Line type="monotone" dataKey="fill" stroke="#00aaff" strokeWidth={2} />
            </LineChart>
          </ResponsiveContainer>
        ) : (
          <p className="no-data">No data for the last 24 hours</p>
        )}
      </div>

      <div className="events-section">
        <h3>Empty Events History</h3>
        {emptyEvents.length > 0 ? (
          <div className="events-list">
            {emptyEvents.map((event) => (
              <div key={event.id} className="event-card">
                <div className="event-header">
                  <span className={`event-type ${event.event_type}`}>
                    {event.event_type === 'emptied'
                      ? '✓ Emptied'
                      : event.event_type === 'emptied_unconfirmed'
                      ? '? Possibly Emptied'
                      : '⚠ Handling (No Empty)'}
                  </span>
                  <span className="event-time">
                    {new Date(event.timestamp).toLocaleString()}
                  </span>
                </div>
                <div className="event-details">
                  <span>
                    Fill: {Math.round(event.fill_pct_before)}% → {Math.round(event.fill_pct_after)}%
                  </span>
                  <span>Pulses: {event.pulse_count}</span>
                  <span>Duration: {event.active_duration_ms}ms</span>
                </div>
              </div>
            ))}
          </div>
        ) : (
          <p className="no-data">No empty events recorded</p>
        )}
      </div>
    </div>
  );
}

export default BinDetail;
