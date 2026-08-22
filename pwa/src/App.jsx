import { useState, useEffect } from 'react';
import { supabase } from './lib/supabase';
import FleetView from './components/FleetView';
import BinDetail from './components/BinDetail';
import CitizenLookup from './components/CitizenLookup';
import RedeemFlow from './components/RedeemFlow';
import AdminAudit from './components/AdminAudit';
import './App.css';

function App() {
  const [view, setView] = useState('fleet'); // fleet, detail, citizen, redeem, admin
  const [selectedBin, setSelectedBin] = useState(null);

  useEffect(() => {
    // Request notification permission
    if ('Notification' in window && Notification.permission === 'default') {
      Notification.requestPermission();
    }

    // Subscribe to high-fill alerts
    const channel = supabase
      .channel('bin-alerts')
      .on(
        'postgres_changes',
        {
          event: 'INSERT',
          schema: 'public',
          table: 'bin_readings',
          filter: 'fill_pct=gt.80'
        },
        (payload) => {
          if ('Notification' in window && Notification.permission === 'granted') {
            new Notification('Bin Needs Pickup', {
              body: `${payload.new.device_id} is at ${payload.new.fill_pct}% capacity`,
              icon: '/bin-icon.png'
            });
          }
        }
      )
      .subscribe();

    return () => {
      supabase.removeChannel(channel);
    };
  }, []);

  const renderView = () => {
    switch (view) {
      case 'detail':
        return <BinDetail deviceId={selectedBin} onBack={() => setView('fleet')} />;
      case 'citizen':
        return <CitizenLookup onBack={() => setView('fleet')} />;
      case 'redeem':
        return <RedeemFlow onBack={() => setView('fleet')} />;
      case 'admin':
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
    <div className="app">
      <header className="header">
        <h1>🗑️ Smart Bin Fleet</h1>
        <nav className="nav">
          <button
            className={view === 'fleet' ? 'active' : ''}
            onClick={() => setView('fleet')}
          >
            Fleet
          </button>
          <button
            className={view === 'citizen' ? 'active' : ''}
            onClick={() => setView('citizen')}
          >
            Citizen Lookup
          </button>
          <button
            className={view === 'redeem' ? 'active' : ''}
            onClick={() => setView('redeem')}
          >
            Redeem Points
          </button>
          <button
            className={view === 'admin' ? 'active' : ''}
            onClick={() => setView('admin')}
          >
            Admin Audit
          </button>
        </nav>
      </header>

      <main className="main">{renderView()}</main>

      <footer className="footer">
        <p>Municipal Smart Bin System • {new Date().getFullYear()}</p>
      </footer>
    </div>
  );
}

export default App;
