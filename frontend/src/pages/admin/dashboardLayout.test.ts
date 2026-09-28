import { describe, expect, it } from 'vitest';

import type { DashboardWidget } from '@/lib/api/dashboard';

import { toGridItems, toPlacements } from './dashboardLayout';

function widget(over: Partial<DashboardWidget> = {}): DashboardWidget {
  return {
    id: 'w1',
    widget_type: 'posts_summary',
    grid_x: 0,
    grid_y: 0,
    grid_w: 4,
    grid_h: 3,
    options: {},
    created_at: '2026-09-28T00:00:00Z',
    updated_at: '2026-09-28T00:00:00Z',
    ...over,
  };
}

describe('dashboard layout mapping', () => {
  it('maps stored widgets onto grid items keyed by id', () => {
    const items = toGridItems([widget(), widget({ id: 'w2', grid_x: 4, grid_w: 6 })]);
    expect(items).toEqual([
      { i: 'w1', x: 0, y: 0, w: 4, h: 3 },
      { i: 'w2', x: 4, y: 0, w: 6, h: 3 },
    ]);
  });

  it('maps grid items back to placements, carrying options over', () => {
    const stored = new Map([['w1', widget({ options: { limit: 7 } })]]);
    const placements = toPlacements([{ i: 'w1', x: 2, y: 1, w: 6, h: 4 }], stored);
    expect(placements).toEqual([
      {
        widget_type: 'posts_summary',
        grid_x: 2,
        grid_y: 1,
        grid_w: 6,
        grid_h: 4,
        options: { limit: 7 },
      },
    ]);
  });

  it('drops grid items whose widget is gone instead of sending a bad type', () => {
    expect(toPlacements([{ i: 'ghost', x: 0, y: 0, w: 4, h: 3 }], new Map())).toEqual([]);
  });
});
