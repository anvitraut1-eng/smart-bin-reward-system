import { useState } from 'react';
import { supabase } from '../lib/supabase';
import FleetView from './FleetView';
import BinDetail from './BinDetail';
import AdminAudit from './AdminAudit';
import './admin-reset.css';

function AdminDashboard({ profile, onLogout }) {
  const [view, setView] = useState('fleet');
  const [selectedBin, setSelectedBin] = useState(null);
  const [resetting, setResetting] = useState(false);
  const [resetMessage, setResetMessage] = useState('');

  const wipeProjectData = async () => {
    if (!window.confirm(
      'WIPE PROJECT DATA?\n\nThis deletes readings, empty events, reward history, RFID/card data, pending links, redemptions and bin records. User accounts are kept.\n\nContinue?'
    )) return;

    if (!window.confirm(
      'FINAL WARNING\n\nThis cannot be undone. Wipe all project data except accounts?'
    )) return;

    setResetting(true);
    setResetMessage('');

    try {
      const { data, error } = await supabase.rpc('reset_project_data');
      if (error) throw error;
      const result = Array.isArray(data) ? data[0] : data;
      if (result?.success === false) throw new Error(result.error_message || 'Reset failed');
      setResetMessage('Project data wiped successfully. Accounts were kept.');
    } catch (error) {
      console.error('Project reset failed:', error);
      setResetMessage(`Reset failed: ${error.message}`);
    } finally {
      setResetting(false);
    }
  };

  const renderView = () => {
    switch (view) {
      case 'detail':
        return <BinDetail deviceId={selectedBin} onBack={() => setView('fleet')} />;
      case 'audit':
        return <AdminAudit onBack={() => setView('fleet')} />;
      default:
        return (
          <FleetView
            onSelectBin={(deviceId) => {
              setSelectedBin(deviceId);
              setView('detail');
            }}
          />
        );
    }
  };

  return (
    <div className="admin-dashboard">
      <header className="dashboard-header">
        <div className="header-content">
          <div>
            <h1>Smart Bin Admin Panel</h1>
            <p className="user-info">
              <span className="user-name">{profile.full_name}</span>
              <span className="user-role">Admin</span>
            </p>
          </div>
          <div className="header-actions">
            <button className="danger-btn" onClick={wipeProjectData} disabled={resetting}>
              {resetting ? 'Wiping…' : '🗑️ Wipe Project Data'}
            </button>
            <button className="logout-btn" onClick={onLogout}>Logout</button>
          </div>
        </div>

        <nav className="admin-nav">
          <button className={view === 'fleet' ? 'active' : ''} onClick={() => setView('fleet')}>
            📊 Fleet Overview
          </button>
          <button className={view === 'audit' ? 'active' : ''} onClick={() => setView('audit')}>
            🔍 Reward Audit
          </button>
        </nav>

        {resetMessage && <div className="admin-reset-message" role="status">{resetMessage}</div>}
      </header>

      <main className="dashboard-main">{renderView()}</main>
    </div>
  );
}

export default AdminDashboard;
