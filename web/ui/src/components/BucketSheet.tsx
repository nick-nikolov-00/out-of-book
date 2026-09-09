import { useEffect, useId, useRef, useState } from 'react';

import { BUCKETS } from '../buckets';
import { BucketSelector } from './BucketSelector';

interface BucketSheetProps {
  bucket: number;
  onChange: (bucket: number) => void;
  /** Games recorded in each bucket for the position on the board. */
  totals: (number | null)[];
}

/**
 * The rating bands on a narrow screen.
 *
 * Nine buttons wrap to three rows there, and cost more space above the board
 * than the board's own controls, for something set once at the start of a
 * session and then left alone. So it collapses to the band you are on, and the
 * full grid opens over the page when asked for.
 *
 * That grid is the same component the wide layout renders inline, so the
 * per-band game counts survive the collapse rather than being the price of it.
 */
export function BucketSheet({ bucket, onChange, totals }: BucketSheetProps) {
  const [open, setOpen] = useState(false);
  const trigger = useRef<HTMLButtonElement>(null);
  const id = useId();

  const label = BUCKETS[bucket]?.label ?? '—';

  useEffect(() => {
    if (!open) return;

    const onKey = (event: KeyboardEvent) => {
      if (event.key === 'Escape') setOpen(false);
    };

    /* A backdrop the page scrolls behind feels broken on a phone, where the
     * scroll gesture starts on the backdrop. */
    const { overflow } = document.body.style;
    document.body.style.overflow = 'hidden';
    window.addEventListener('keydown', onKey);

    return () => {
      document.body.style.overflow = overflow;
      window.removeEventListener('keydown', onKey);
    };
  }, [open]);

  /* Focus returns to the pill, so dismissing does not drop the caret back at
   * the top of the document. */
  const close = () => {
    setOpen(false);
    trigger.current?.focus();
  };

  return (
    <>
      <button
        type="button"
        ref={trigger}
        className="bucket-pill"
        aria-expanded={open}
        aria-controls={id}
        /* Named here as well as in the label beside it, because on a phone
         * there is no room for that label and it is not rendered. */
        aria-label={`Rating band: ${label}`}
        title="Choose which rating band's games to read"
        onClick={() => setOpen((was) => !was)}
      >
        <span className="bucket-pill-label">Rating</span>
        <span className="bucket-pill-value">{label}</span>
      </button>

      {open && (
        <div className="sheet-backdrop" onClick={close}>
          <div
            id={id}
            className="sheet"
            role="dialog"
            aria-modal="true"
            aria-label="Rating band"
            onClick={(event) => event.stopPropagation()}
          >
            <header className="sheet-head">
              <h2>Rating band</h2>
              <button type="button" className="sheet-close" onClick={close} aria-label="Close">
                ✕
              </button>
            </header>

            <BucketSelector
              bucket={bucket}
              totals={totals}
              onChange={(next) => {
                onChange(next);
                close();
              }}
            />

            <p className="sheet-note">
              Under each band, the games it recorded in the position on the board.
            </p>
          </div>
        </div>
      )}
    </>
  );
}
