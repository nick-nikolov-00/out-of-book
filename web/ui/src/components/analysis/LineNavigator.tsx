import { useEffect, useRef } from 'react';

import type { Ply } from '../../hooks/useAnalysis';

interface LineNavigatorProps {
  line: Ply[];
  cursor: number;
  onSelect: (index: number) => void;
}

/** The played line, as clickable ply pairs. Index 0 is the starting position. */
export function LineNavigator({ line, cursor, onSelect }: LineNavigatorProps) {
  const active = useRef<HTMLButtonElement>(null);

  useEffect(() => {
    active.current?.scrollIntoView({ block: 'nearest' });
  }, [cursor]);

  if (line.length === 0) {
    return <div className="line-navigator empty">No moves played yet.</div>;
  }

  return (
    <div className="line-navigator">
      <button
        type="button"
        className={cursor === 0 ? 'ply start current' : 'ply start'}
        onClick={() => onSelect(0)}
        ref={cursor === 0 ? active : null}
      >
        start
      </button>

      {line.map((ply, index) => {
        const isWhiteMove = index % 2 === 0;
        const current = cursor === index + 1;

        return (
          <span className="ply-group" key={`${index}-${ply.uci}`}>
            {isWhiteMove && <span className="move-number">{index / 2 + 1}.</span>}
            <button
              type="button"
              className={current ? 'ply current' : 'ply'}
              onClick={() => onSelect(index + 1)}
              ref={current ? active : null}
            >
              {ply.san}
            </button>
          </span>
        );
      })}
    </div>
  );
}
