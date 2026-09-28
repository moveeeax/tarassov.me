import { describe, expect, it } from 'vitest';

import { WIDGET_COMPONENTS } from './registry';

/**
 * The backend catalog (Domain::Widgets::kCatalog) is the single source of widget
 * types. This test pins the frontend registry to it: a type added on the backend
 * without a component here renders an empty card and nothing else complains.
 */
const BACKEND_TYPES = [
  'posts_summary',
  'jobs_queue',
  'audit_recent',
  'users_recent',
  'service_health',
] as const;

describe('widget registry', () => {
  it('has a component for every catalog type', () => {
    for (const type of BACKEND_TYPES) {
      expect(WIDGET_COMPONENTS[type], `missing component for ${type}`).toBeDefined();
    }
  });

  it('has no component without a catalog entry', () => {
    expect(Object.keys(WIDGET_COMPONENTS).sort()).toEqual([...BACKEND_TYPES].sort());
  });
});
