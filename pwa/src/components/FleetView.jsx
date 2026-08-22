import { useState, useEffect } from 'react';
import { supabase } from '../lib/supabase';

function FleetView({ onSelectBin }) {
  const [bins, setBins] = useState([]);
  const [loading, setLoading] = useState(true);
  const [editingBin, setEditingBin] = useState(null);
  const [newLocation, setNewLocation] = useState('');

  useEffect(() => {
    fetchBins();

    // Subscribe to realtime updates
    const channel = supabase
      .channel('fleet-updates')
      .on(
        'postgres_changes',
        { event: '*', schema: 'public', table: 'bin_readings' },
        () => fetchBins()
      )
      .on(
        'postgres_changes',
        { event: '*', schema: 'public', table: 'devices' },
        () => fetchBins()
      )
      .subscribe();

    return () => {
      supabase.removeChannel(channel);
    };
  }, []);

  const fetchBins = async () => {
    try {
      // Get all devices
      const { data: devices, error: devicesError } = await supabase
        .from('devices')
        .select('*')
        .order('device_id');

      if (devicesError) throw devicesError;

      // Get latest reading for each device
      const binsWithStatus = await Promise.all(
        devices.map(async (device) => {
          const { data: readings } = await supabase
            .from('bin_readings')
            .select('fill_pct, timestamp')
            .eq('device_id', device.device_id)
            .order('timestamp', { ascending: false })
            .limit(1);

          const latestReading = readings?.[0];
          const isOffline = latestReading
            ? Date.now() - new Date(latestReading.timestamp).getTime() > 300000 // 5 min
            : true;

          return {
            ...device,
            fill_pct: latestReading?.fill_pct || 0,
            last_seen: latestReading?.timestamp,
            status: isOffline ? 'offline' : latestReading.fill_pct > 80 ? 'needs_pickup' : 'ok'
          };
        })
      );

      // Sort by fill % descending
      binsWithStatus.sort((a, b) => b.fill_pct - a.fill_pct);
      setBins(binsWithStatus);
    } catch (error) {
      console.error('Error fetching bins:', error);
    } finally {
      setLoading(false);
    }
  };

  const updateLocation = async (deviceId) => {
    try {
      const { error } = await supabase
        .from('devices')
        .update({ location: newLocation })
        .eq('device_id', deviceId);

      if (error) throw error;

      setEditingBin(null);
      setNewLocation('');
      fetchBins();
    } catch (error) {
      console.error('Error updating location:', error);
      alert('Failed to update location');
    }
  };

  const getStatusColor = (status) => {
    switch (status) {
      case 'needs_pickup':
        return '#ff4444';
      case 'offline':
        return '#888888';
      default:
        return '#44ff44';
    }
  };

  const getStatusLabel = (status) => {
    switch (status) {
      case 'needs_pickup':
        return 'Needs Pickup';
      case 'offline':
        return 'Offline';
      default:
        return 'OK';
    }
  };

  if (loading) {
    return <div className="loading">Loading fleet data...</div>;
  }

  return (
    <div className="fleet-view">
      <div className="fleet-summary">
        <div className="summary-card">
          <h3>Total Bins</h3>
          <p className="big-number">{bins.length}</p>
        </div>
        <div className="summary-card">
          <h3>Needs Pickup</h3>
          <p className="big-number" style={{ color: '#ff4444' }}>
            {bins.filter((b) => b.status === 'needs_pickup').length}
          </p>
        </div>
        <div className="summary-card">
          <h3>Offline</h3>
          <p className="big-number" style={{ color: '#888888' }}>
            {bins.filter((b) => b.status === 'offline').length}
          </p>
        </div>
        <div className="summary-card">
          <h3>Average Fill</h3>
          <p className="big-number">
            {bins.length > 0
              ? Math.round(bins.reduce((sum, b) => sum + b.fill_pct, 0) / bins.length)
              : 0}
            %
          </p>
        </div>
      </div>

      <div className="bins-grid">
        {bins.map((bin) => (
          <div key={bin.device_id} className="bin-card">
            <div className="bin-header">
              <div className="bin-id" onClick={() => onSelectBin(bin.device_id)}>
                {bin.device_id}
              </div>
              <div
                className="status-badge"
                style={{ backgroundColor: getStatusColor(bin.status) }}
              >
                {getStatusLabel(bin.status)}
              </div>
            </div>

            <div className="bin-location">
              {editingBin === bin.device_id ? (
                <div className="location-edit">
                  <input
                    type="text"
                    value={newLocation}
                    onChange={(e) => setNewLocation(e.target.value)}
                    placeholder="Enter location"
                    autoFocus
                  />
                  <button onClick={() => updateLocation(bin.device_id)}>Save</button>
                  <button onClick={() => setEditingBin(null)}>Cancel</button>
                </div>
              ) : (
                <div className="location-display">
                  <span>📍 {bin.location}</span>
                  <button
                    onClick={() => {
                      setEditingBin(bin.device_id);
                      setNewLocation(bin.location);
                    }}
                  >
                    Edit
                  </button>
                </div>
              )}
            </div>

            <div className="fill-gauge">
              <div className="gauge-bar">
                <div
                  className="gauge-fill"
                  style={{
                    width: `${bin.fill_pct}%`,
                    backgroundColor:
                      bin.fill_pct > 80 ? '#ff4444' : bin.fill_pct > 50 ? '#ffaa00' : '#44ff44'
                  }}
                />
              </div>
              <div className="gauge-label">{Math.round(bin.fill_pct)}% full</div>
            </div>

            {bin.last_seen && (
              <div className="last-seen">
                Last update: {new Date(bin.last_seen).toLocaleString()}
              </div>
            )}

            <button className="view-details-btn" onClick={() => onSelectBin(bin.device_id)}>
              View Details
            </button>
          </div>
        ))}
      </div>

      {bins.length === 0 && (
        <div className="empty-state">
          <p>No bins registered yet. Deploy a device to get started.</p>
        </div>
      )}
    </div>
  );
}

export default FleetView;
