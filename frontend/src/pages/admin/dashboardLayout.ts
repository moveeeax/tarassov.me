// v2 renamed the pair: `Layout` is the whole array, `LayoutItem` is one cell.
import type { LayoutItem } from 'react-grid-layout';

import type { CatalogEntry, DashboardWidget, WidgetPlacement } from '@/lib/api/dashboard';

/**
 * Stored widgets → grid items. The item key is the widget id.
 *
 * `minW` / `minH` come from the catalog because the server enforces a per-type
 * minimum: without them the grid happily resizes a card down to 1x1 and the PUT
 * that follows answers 400 while the card keeps the size on screen.
 */
export function toGridItems(
  widgets: readonly DashboardWidget[],
  catalog: readonly CatalogEntry[],
): LayoutItem[] {
  return widgets.map((w) => {
    const spec = catalog.find((c) => c.type === w.widget_type);
    const item: LayoutItem = { i: w.id, x: w.grid_x, y: w.grid_y, w: w.grid_w, h: w.grid_h };
    // An unknown type has no minimum to enforce; the card renders as "Unknown
    // widget" anyway, and inventing a bound would be worse than leaving it open.
    if (spec) {
      item.minW = spec.min_w;
      item.minH = spec.min_h;
    }
    return item;
  });
}

/** Widgets → what PUT accepts. The id is dropped: the server mints its own. */
export function toPlacements(widgets: readonly DashboardWidget[]): WidgetPlacement[] {
  return widgets.map((w) => ({
    widget_type: w.widget_type,
    grid_x: w.grid_x,
    grid_y: w.grid_y,
    grid_w: w.grid_w,
    grid_h: w.grid_h,
    options: w.options,
  }));
}

/**
 * Ids for widgets that exist on screen but not yet in the database. The server
 * mints the real uuid on the next PUT; until then the grid needs a stable key.
 */
let localSeq = 0;

/**
 * Append a widget from the catalog, below everything already placed.
 *
 * Every edit function here takes the current widget list and returns the next
 * one, so two edits inside the save debounce compose instead of the second
 * discarding the first.
 */
export function appendWidget(
  widgets: readonly DashboardWidget[],
  entry: CatalogEntry,
): DashboardWidget[] {
  localSeq += 1;
  const bottom = widgets.reduce((max, w) => Math.max(max, w.grid_y + w.grid_h), 0);
  // Option defaults come from the catalog, not from numbers re-typed per widget:
  // change a default on the backend and the client follows.
  const options = Object.fromEntries(
    Object.entries(entry.options).map(([key, spec]) => [key, spec.default]),
  );
  return [
    ...widgets,
    {
      id: `local-${localSeq}`,
      widget_type: entry.type,
      grid_x: 0,
      grid_y: bottom,
      grid_w: entry.default_w,
      grid_h: entry.default_h,
      options,
      // Server-assigned; empty until the next PUT returns the stored row.
      created_at: '',
      updated_at: '',
    },
  ];
}

/**
 * Remove a widget and close the vertical gap it leaves, so the stored rows match
 * what the grid renders (react-grid-layout compacts vertically by default).
 */
export function withoutWidget(widgets: readonly DashboardWidget[], id: string): DashboardWidget[] {
  const removed = widgets.find((w) => w.id === id);
  if (!removed) return [...widgets];
  const floor = removed.grid_y + removed.grid_h;
  return widgets
    .filter((w) => w.id !== id)
    .map((w) => (w.grid_y >= floor ? { ...w, grid_y: w.grid_y - removed.grid_h } : w));
}

/** Apply a grid arrangement onto the stored widgets, keeping their order. */
export function applyGridItems(
  widgets: readonly DashboardWidget[],
  items: readonly LayoutItem[],
): DashboardWidget[] {
  const byKey = new Map(items.map((item) => [item.i, item]));
  return widgets.map((w) => {
    const item = byKey.get(w.id);
    if (!item) return w;
    return { ...w, grid_x: item.x, grid_y: item.y, grid_w: item.w, grid_h: item.h };
  });
}

/**
 * A widget's options with every catalog default filled in.
 *
 * Widget components read their options straight from this, so none of them
 * carries its own copy of a default. Change `kPostsOptions` on the backend and
 * the client follows on the next request.
 */
export function withOptionDefaults(
  widget: DashboardWidget,
  catalog: readonly CatalogEntry[],
): Record<string, number> {
  const spec = catalog.find((c) => c.type === widget.widget_type);
  if (!spec) return widget.options;
  const defaults = Object.fromEntries(
    Object.entries(spec.options).map(([key, option]) => [key, option.default]),
  );
  return { ...defaults, ...widget.options };
}
