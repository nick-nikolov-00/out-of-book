import { useEffect, useReducer, useState } from 'react';

import { installCacheTools } from './api';
import { rememberBucket } from './bucketChoice';
import { BucketGate } from './components/BucketGate';
import { TabBar } from './components/TabBar';
import { installDebugSwitch } from './debug';
import { navigate, usePath } from './router';
import { installDevTools } from './storage/customLines';
import {
  installBandTools,
  onBandForgotten,
  preferredBucket,
  savePreferredBucket,
} from './storage/preferredBucket';
import { DEFAULT_VIEW, LISTED_VIEWS, viewByPath } from './views/registry';

/**
 * The shell: a tab bar and whichever view the URL names. It holds no chess
 * state of its own — each view owns everything it needs — so a new screen is a
 * file plus an entry in the registry.
 *
 * Views are mounted one at a time rather than hidden with CSS: a training
 * session that kept running behind the analysis board would go on fetching
 * positions and playing replies into a screen nobody is looking at.
 *
 * It also owns the one thing asked before a view can be used: the rating band.
 */
export function App() {
  const path = usePath();
  const view = viewByPath(path) ?? DEFAULT_VIEW;

  /* Read once at mount: this is a question with an answer, not a value to
   * track. Storing the band is what closes the gate for future visits, and
   * this state is what closes it for the visit that answered. */
  const [gated, setGated] = useState(() => preferredBucket() === null);

  const chooseBand = (bucket: number) => {
    savePreferredBucket(bucket);
    /* So the view that mounts next opens on it, exactly as a pick made on one
     * of the boards carries over to the other. */
    rememberBucket(bucket);
    setGated(false);
  };

  /* Dropping the stored band — from the debug panel, or from the console handle
   * — asks the question again straight away, rather than on a reload the person
   * who just pressed Reset has no reason to expect. */
  useEffect(() => onBandForgotten(() => setGated(true)), []);

  /*
   * The address bar is made to agree with what is on screen, so there is
   * exactly one URL per view: "/" and a mistyped path both land on the default
   * view rather than leaving the app blank. Replacing rather than pushing keeps
   * Back pointing at wherever the bad link was followed from, instead of
   * bouncing off the correction.
   */
  useEffect(() => {
    if (path !== view.path) navigate(view.path + window.location.search, { replace: true });
  }, [path, view.path]);

  /* The console handles are registered here rather than by a view, so they
   * exist whichever tab is open. Flipping the debug switch re-renders the
   * active view, since whether its panels are drawn is decided during render. */
  const [, redraw] = useReducer((n: number) => n + 1, 0);

  useEffect(() => {
    installDevTools();
    installCacheTools();
    installBandTools();
    installDebugSwitch(redraw);
  }, [redraw]);

  const Current = view.component;

  return (
    <>
      {/* Inert while the band is being asked for, so the dimmed shell behind
        * the dialog cannot be clicked into or tabbed through. */}
      <div className="app" inert={gated}>
        <header className="app-header">
          <h1>
            Out of Book <span className="app-header-sub">{view.subtitle}</span>
          </h1>

          <TabBar views={LISTED_VIEWS} active={view.path} />
        </header>

        {/* The view behind the dialog is mounted on the fallback band, since it
          * has to be drawn for there to be anything to dim. Keying it on whether
          * the question is still open restarts it once, on the band that was
          * actually chosen. */}
        <Current key={gated ? 'gated' : 'chosen'} />
      </div>

      {gated && <BucketGate onChoose={chooseBand} />}
    </>
  );
}
