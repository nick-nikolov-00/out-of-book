import { isPlainLeftClick, navigate } from '../router';
import type { ViewDef } from '../views/registry';

interface TabBarProps {
  views: ViewDef[];
  /** Path of the view being shown. */
  active: string;
}

/**
 * Real links rather than buttons: middle click and ⌘/ctrl click open a view in
 * a new tab, the browser shows the target in the status bar, and Back does what
 * it says. That also makes `role="tab"` wrong — ARIA tabs describe panels
 * within one page, and these are pages.
 */
export function TabBar({ views, active }: TabBarProps) {
  return (
    <nav className="tab-bar" aria-label="Views">
      {views.map((view) => (
        <a
          key={view.path}
          href={view.path}
          className={view.path === active ? 'tab active' : 'tab'}
          aria-current={view.path === active ? 'page' : undefined}
          onClick={(event) => {
            if (!isPlainLeftClick(event)) return;

            event.preventDefault();
            navigate(view.path);
          }}
        >
          {view.label}
        </a>
      ))}
    </nav>
  );
}
