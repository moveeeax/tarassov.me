// v2 renamed the pair: `Layout` is the whole array, `LayoutItem` is one cell.
import type { LayoutItem } from 'react-grid-layout';

import type { DashboardWidget, WidgetPlacement } from '@/lib/api/dashboard';

/** Stored widgets → grid items. The item key is the widget id. */
export function toGridItems(widgets: DashboardWidget[]): LayoutItem[] {
  return widgets.map((w) => ({ i: w.id, x: w.grid_x, y: w.grid_y, w: w.grid_w, h: w.grid_h }));
}

/**
 * Grid items → what PUT accepts. An item whose widget is no longer in the map
 * is dropped: the alternative is guessing a widget_type, and the server would
 * reject the guess with a 400 that the user cannot act on.
 */
export function toPlacements(
  items: readonly LayoutItem[],
  byId: Map<string, DashboardWidget>,
): WidgetPlacement[] {
  const out: WidgetPlacement[] = [];
  for (const item of items) {
    const widget = byId.get(item.i);
    if (!widget) continue;
    out.push({
      widget_type: widget.widget_type,
      grid_x: item.x,
      grid_y: item.y,
      grid_w: item.w,
      grid_h: item.h,
      options: widget.options,
    });
  }
  return out;
}
