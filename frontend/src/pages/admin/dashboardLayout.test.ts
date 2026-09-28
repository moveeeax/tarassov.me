import { describe, expect, it } from 'vitest';

import type { CatalogEntry, DashboardWidget } from '@/lib/api/dashboard';

import {
  appendWidget,
  applyGridItems,
  toGridItems,
  toPlacements,
  withOptionDefaults,
  withoutWidget,
} from './dashboardLayout';

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

function entry(over: Partial<CatalogEntry> = {}): CatalogEntry {
  return {
    type: 'posts_summary',
    title: 'Posts',
    description: '',
    default_w: 4,
    default_h: 3,
    min_w: 3,
    min_h: 2,
    options: { limit: { type: 'int', min: 1, max: 10, default: 5 } },
    ...over,
  };
}

describe('dashboard layout mapping', () => {
  it('maps stored widgets onto grid items keyed by id', () => {
    const items = toGridItems([widget(), widget({ id: 'w2', grid_x: 4, grid_w: 6 })], []);
    expect(items).toEqual([
      { i: 'w1', x: 0, y: 0, w: 4, h: 3 },
      { i: 'w2', x: 4, y: 0, w: 6, h: 3 },
    ]);
  });

  // Without minW/minH the grid lets a card shrink below the catalog minimum and
  // the PUT that follows answers 400 while the card stays small on screen.
  it('carries the catalog minimum onto the grid item so a resize cannot go below it', () => {
    const items = toGridItems([widget()], [entry()]);
    expect(items[0]).toEqual({ i: 'w1', x: 0, y: 0, w: 4, h: 3, minW: 3, minH: 2 });
  });

  it('leaves the minimum off when the catalog has no entry for the type', () => {
    const items = toGridItems([widget({ widget_type: 'gone' })], [entry()]);
    expect(items[0]).toEqual({ i: 'w1', x: 0, y: 0, w: 4, h: 3 });
  });

  it('maps widgets to placements, dropping the id and carrying options over', () => {
    const placements = toPlacements([
      widget({ grid_x: 2, grid_y: 1, grid_w: 6, grid_h: 4, options: { limit: 7 } }),
    ]);
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
});

describe('dashboard layout edits', () => {
  // Every edit returns the next full widget list, so two edits inside the save
  // debounce build on each other instead of the second discarding the first.
  it('appends a widget below everything already placed', () => {
    const next = appendWidget([widget({ grid_y: 0, grid_h: 3 })], entry({ type: 'jobs_queue' }));
    expect(next).toHaveLength(2);
    expect(next[1].widget_type).toBe('jobs_queue');
    expect(next[1].grid_y).toBe(3);
    expect(next[1].grid_w).toBe(4);
    expect(next[1].id).not.toBe(next[0].id);
  });

  it('seeds a new widget with the catalog option defaults rather than hardcoded numbers', () => {
    const next = appendWidget([], entry());
    expect(next[0].options).toEqual({ limit: 5 });
  });

  it('appending twice keeps both widgets', () => {
    const once = appendWidget([], entry());
    const twice = appendWidget(once, entry({ type: 'jobs_queue' }));
    expect(twice.map((w) => w.widget_type)).toEqual(['posts_summary', 'jobs_queue']);
  });

  // Deleting the top card used to leave the survivor stored at its old y, so the
  // render (which compacts) and the stored rows disagreed.
  it('closes the vertical gap left by a removed widget', () => {
    const rows = [
      widget({ id: 'a', grid_y: 0, grid_h: 3 }),
      widget({ id: 'b', grid_y: 3, grid_h: 3 }),
    ];
    const next = withoutWidget(rows, 'a');
    expect(next).toHaveLength(1);
    expect(next[0].id).toBe('b');
    expect(next[0].grid_y).toBe(0);
  });

  it('removing twice removes both', () => {
    const rows = [
      widget({ id: 'a' }),
      widget({ id: 'b', grid_y: 3 }),
      widget({ id: 'c', grid_y: 6 }),
    ];
    expect(withoutWidget(withoutWidget(rows, 'a'), 'b').map((w) => w.id)).toEqual(['c']);
  });

  it('applies a grid arrangement onto the stored widgets', () => {
    const rows = [widget({ id: 'a' }), widget({ id: 'b', grid_x: 4 })];
    const next = applyGridItems(rows, [
      { i: 'b', x: 0, y: 0, w: 6, h: 4 },
      { i: 'a', x: 6, y: 0, w: 6, h: 4 },
    ]);
    // Order follows the stored list; only geometry moves.
    expect(next.map((w) => [w.id, w.grid_x, w.grid_w])).toEqual([
      ['a', 6, 6],
      ['b', 0, 6],
    ]);
  });

  it('ignores grid items that no longer have a widget', () => {
    const next = applyGridItems([widget({ id: 'a' })], [{ i: 'ghost', x: 5, y: 5, w: 5, h: 5 }]);
    expect(next).toEqual([widget({ id: 'a' })]);
  });
});

describe('option defaults', () => {
  // The catalog publishes each option's default; a widget component must not
  // carry its own copy of the number, or changing it on the backend changes the
  // advertised value and nothing else.
  it('fills a missing option from the catalog default', () => {
    expect(withOptionDefaults(widget({ options: {} }), [entry()])).toEqual({ limit: 5 });
  });

  it('keeps a stored value over the default', () => {
    expect(withOptionDefaults(widget({ options: { limit: 9 } }), [entry()])).toEqual({ limit: 9 });
  });

  it('returns the stored options unchanged when the type is not in the catalog', () => {
    expect(
      withOptionDefaults(widget({ widget_type: 'gone', options: { x: 1 } }), [entry()]),
    ).toEqual({
      x: 1,
    });
  });
});
