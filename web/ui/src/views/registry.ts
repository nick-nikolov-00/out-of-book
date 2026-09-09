import type { ComponentType } from 'react';

import { AnalysisView } from './AnalysisView';
import { TrainingView } from './TrainingView';

/**
 * The screens the shell can show, and the routes they live at. A view owns its
 * own state, controls and layout and takes no props, so adding one here is the
 * whole wiring.
 */
export interface ViewDef {
  /** The route this view lives at, and its identity. */
  path: string;
  label: string;
  /** Shown next to the product name while the view is open. */
  subtitle: string;
  component: ComponentType;
}

export const VIEWS: ViewDef[] = [
  {
    path: '/analysis',
    label: 'Analysis',
    subtitle: 'Analysis Board',
    component: AnalysisView,
  },
  {
    path: '/training',
    label: 'Training',
    subtitle: 'Training',
    component: TrainingView,
  },
];

/**
 * The paths the tab bar advertises, which need not be every path. A view left
 * out of this list still routes and still renders; it simply has no tab
 * pointing at it, which suits a screen that is reachable by link but not worth
 * a permanent place in the bar.
 *
 * That is the weaker of the two switches, and it withholds nothing: the view is
 * still routed and its code is still in the bundle. To withhold a view
 * entirely, leave it out of VIEWS above so that its path is not a route and the
 * component is never imported.
 */
const LISTED_PATHS = [
  '/analysis',
  '/training',
];

export const LISTED_VIEWS: ViewDef[] = VIEWS.filter((view) => LISTED_PATHS.includes(view.path));

const [FIRST] = VIEWS;

if (!FIRST) throw new Error('no views registered');

export const DEFAULT_VIEW = FIRST;

/**
 * Null rather than a fallback, so the shell can tell "/" and a typo apart from a
 * real route and rewrite the address bar instead of leaving two URLs showing the
 * same screen.
 */
export function viewByPath(path: string): ViewDef | null {
  /* A trailing slash is the same place; anything else is not. */
  const normalised = path.length > 1 && path.endsWith('/') ? path.slice(0, -1) : path;

  return VIEWS.find((view) => view.path === normalised) ?? null;
}
