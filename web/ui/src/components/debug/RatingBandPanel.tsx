import { BUCKETS } from '../../buckets';
import { clearPreferredBucket, preferredBucket } from '../../storage/preferredBucket';

/**
 * The stored rating band, and the way to drop it.
 *
 * The band is asked for once on a first visit and then never shown again, so
 * this is the only place that says what is actually stored. Dropping it puts
 * that question straight back over the page, which is the screen that sets it —
 * there is no second control for the band here, because two ways to set one
 * value is how they end up disagreeing.
 *
 * A maintenance surface rather than part of training, so it is rendered only
 * behind the debug opt-in.
 */
export function RatingBandPanel() {
  const stored = preferredBucket();

  return (
    <section className="debug-panel">
      <header>
        <span className="summary-label">
          Rating band · {stored === null ? 'not stored' : BUCKETS[stored]?.label}
        </span>

        {stored !== null && (
          <button
            type="button"
            className="link"
            onClick={() => clearPreferredBucket()}
            title="Forget the stored band and be asked for it again"
          >
            Reset
          </button>
        )}
      </header>

      <p className="empty-note">
        {stored === null
          ? 'Nothing stored, so both boards open on the site default and the question is asked on the next load.'
          : 'Both boards open on this band. Resetting asks for it again now, which restarts the session behind the dialog.'}
      </p>
    </section>
  );
}
