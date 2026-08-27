import { useState, useEffect } from 'react';
import { supabase } from '../lib/supabase';

function CardLinkModal({ userId, onClose }) {
  const [pendingCards, setPendingCards] = useState([]);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState(null);

  useEffect(() => {
    // Fetch pending card links for this user
    fetchPendingCards();

    // Subscribe to new pending card links
    const subscription = supabase
      .channel('pending-cards-changes')
      .on('postgres_changes', {
        event: 'INSERT',
        schema: 'public',
        table: 'pending_card_links',
        filter: 'claimed=eq.false'
      }, (payload) => {
        fetchPendingCards();
      })
      .subscribe();

    return () => {
      subscription.unsubscribe();
    };
  }, []);

  const fetchPendingCards = async () => {
    try {
      const { data, error } = await supabase
        .from('pending_card_links')
        .select('*')
        .eq('claimed', false)
        .gt('expires_at', new Date().toISOString())
        .order('created_at', { ascending: false })
        .limit(5);

      if (error) throw error;
      setPendingCards(data || []);
    } catch (err) {
      console.error('Error fetching pending cards:', err);
      setError(err.message);
    }
  };

  const handleClaim = async (cardUID) => {
    if (!confirm(`Link card ${cardUID} to your account?`)) return;

    setLoading(true);
    setError(null);

    try {
      const { data, error } = await supabase.rpc('claim_pending_card', {
        p_user_id: userId,
        p_card_uid: cardUID
      });

      if (error) throw error;

      if (data && data[0] && data[0].success) {
        alert('✅ Card linked successfully! You earned 10 points for this disposal.');
        fetchPendingCards();
        onClose();
      } else {
        setError(data[0]?.error_message || 'Failed to link card');
      }
    } catch (err) {
      console.error('Error claiming card:', err);
      setError(err.message);
    } finally {
      setLoading(false);
    }
  };

  return (
    <div className="modal-overlay" onClick={onClose}>
      <div className="modal-content" onClick={(e) => e.stopPropagation()}>
        <div className="modal-header">
          <h2>🎴 Link Your RFID Card</h2>
          <button className="close-btn" onClick={onClose}>×</button>
        </div>

        <div className="modal-body">
          <p className="info-text">
            We detected RFID card taps at smart bins. Is any of these your card?
          </p>

          {error && <div className="error-message">{error}</div>}

          {pendingCards.length === 0 ? (
            <p className="no-cards">No pending cards detected. Tap your card at a bin first!</p>
          ) : (
            <div className="card-list">
              {pendingCards.map((card) => (
                <div key={card.id} className="pending-card-item">
                  <div className="card-info">
                    <div className="card-uid">{card.card_uid}</div>
                    <div className="card-details">
                      📍 Bin: {card.device_id} • ⏰ {new Date(card.timestamp).toLocaleString()}
                    </div>
                  </div>
                  <button
                    className="claim-btn"
                    onClick={() => handleClaim(card.card_uid)}
                    disabled={loading}
                  >
                    Yes, Link to Me
                  </button>
                </div>
              ))}
            </div>
          )}

          <div className="modal-footer">
            <p className="help-text">
              💡 <strong>Tip:</strong> Your card will only be linked if you confirm here.
              Cards expire after 24 hours if not claimed.
            </p>
          </div>
        </div>
      </div>
    </div>
  );
}

export default CardLinkModal;
