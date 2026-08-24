import { useState } from 'react';
import FleetView from './FleetView';
import BinDetail from './BinDetail';
import AdminAudit from './AdminAudit';

function AdminDashboard({ profile, onLogout }) {
  const [view, setView] = useState('fleet'); // fleet, detail, audit
  const [selectedBin, setSelectedBin] = useState(null);

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
            <button className="logout-btn" onClick={onLogout}>
              Logout
            </button>
          </div>
        </div>

        <nav className="admin-nav">
          <button
            className={view === 'fleet' ? 'active' : ''}
            onClick={() => setView('fleet')}
          >
            📊 Fleet Overview
          </button>
          <button
            className={view === 'audit' ? 'active' : ''}
            onClick={() => setView('audit')}
          >
            🔍 Reward Audit
          </button>
        </nav>
      </header>

      <main className="dashboard-main">{renderView()}</main>
    </div>
  );
}

export default AdminDashboard;
