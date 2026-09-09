import { useEffect, useId, useRef } from 'react';

import { BUCKETS } from '../buckets';

interface BucketGateProps {
  /** The band the visitor picked. Answering is the only way out of here. */
  onChoose: (bucket: number) => void;
}

/**
 * The question a first-time visitor is asked before anything else: which
 * rating band's games should the site read?
 *
 * Every number on both boards comes from one band's tree, so a visitor who
 * never touches the band control has still been shown one particular field's
 * answers — and the field that suits them is something only they know. Asking
 * once is better than guessing for everyone.
 *
 * The site is drawn behind it, dimmed and inert, so what is being configured is
 * on screen while the question is answered. Nothing dismisses this but a pick:
 * there is no close button and Escape does nothing, because a band is not
 * optional and the alternative to choosing is the guess this exists to avoid.
 */
export function BucketGate({ onChoose }: BucketGateProps) {
  const dialog = useRef<HTMLDivElement>(null);
  const titleId = useId();

  useEffect(() => {
    /* The dialog itself takes focus, not the first band: with a button focused,
     * Enter would pick <1000 for anybody who answers the keyboard before
     * reading. From here Tab walks the bands, and cannot leave — the shell
     * behind is inert. */
    dialog.current?.focus();

    /* The page behind must not scroll under the dialog, which on a phone is
     * what the scroll gesture would otherwise do. */
    const { overflow } = document.body.style;
    document.body.style.overflow = 'hidden';

    return () => {
      document.body.style.overflow = overflow;
    };
  }, []);

  return (
    <div className="gate-backdrop">
      <div
        ref={dialog}
        tabIndex={-1}
        className="gate"
        role="dialog"
        aria-modal="true"
        aria-labelledby={titleId}
      >
        <h2 id={titleId}>Your rating band</h2>

        <p className="gate-note">
          Pick your rating band for the most accurate suggestions of what you can expect
          at that level. You can still read any of the others whenever you like.
        </p>

        <div className="gate-bands">
          {BUCKETS.map((entry, index) => (
            <button
              key={entry.label}
              type="button"
              className="gate-band"
              onClick={() => onChoose(index)}
            >
              {entry.label}
            </button>
          ))}
        </div>
      </div>
    </div>
  );
}
