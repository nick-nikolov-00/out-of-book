import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';

import { App } from './App';
import { installClientMetrics } from './clientMetrics';
import './styles/board.css';
import './styles/app.css';

const container = document.getElementById('root');

if (!container) throw new Error('missing #root element');

createRoot(container).render(
  <StrictMode>
    <App />
  </StrictMode>,
);

/* After the render call rather than before it, so the frame the page-load
 * timer waits for is the one this render produces. */
installClientMetrics();
