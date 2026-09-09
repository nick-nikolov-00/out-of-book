import type { ReactNode } from 'react';

import { BucketSelector } from './BucketSelector';
import { BucketSheet } from './BucketSheet';
import { useMediaQuery } from '../hooks/useMediaQuery';

interface ViewControlsProps {
  /** The board's own side control: two sides on analysis, three on training. */
  sideControl: ReactNode;
  bucket: number;
  onBucket: (bucket: number) => void;
  totals: (number | null)[];
}

/**
 * The settings both boards carry: which side, and whose games.
 *
 * The side toggle stays inline at every width — it is a couple of buttons and
 * one tap, and hiding it would cost more than it saves. Only the nine rating
 * bands collapse, and only where they wrap.
 */
export function ViewControls({ sideControl, bucket, onBucket, totals }: ViewControlsProps) {
  const narrow = useMediaQuery('(max-width: 760px)');

  return (
    <div className="view-controls">
      {sideControl}

      {narrow ? (
        <BucketSheet bucket={bucket} onChange={onBucket} totals={totals} />
      ) : (
        <BucketSelector bucket={bucket} onChange={onBucket} totals={totals} />
      )}
    </div>
  );
}
