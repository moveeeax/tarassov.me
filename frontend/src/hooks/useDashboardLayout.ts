import { useCallback, useEffect, useRef } from 'react';
import { useQuery, useQueryClient } from '@tanstack/react-query';

import {
  fetchCatalog,
  fetchLayout,
  saveLayout,
  type DashboardWidget,
  type WidgetPlacement,
} from '@/lib/api/dashboard';
import { qk } from '@/lib/api/queryKeys';
import { useApiMutation } from '@/hooks/useApiMutation';

/** Debounce for layout writes: dragging fires continuously, the server needs one PUT. */
const SAVE_DELAY_MS = 800;

/**
 * Reads the layout and the catalog, and writes the layout back debounced.
 *
 * Two properties the naive version got wrong:
 *  - `applyOptimistic` puts the caller's arrangement into the query cache right
 *    away, so a second edit inside the debounce window builds on the first
 *    instead of recomputing from stale server data and discarding it.
 *  - a pending write is FLUSHED on unmount, not dropped. Dragging a card and
 *    clicking a nav link within 800 ms used to lose the arrangement silently.
 */
export function useDashboardLayout() {
  const qc = useQueryClient();
  const layout = useQuery({ queryKey: qk.dashboard.layout(), queryFn: fetchLayout });
  const catalog = useQuery({ queryKey: qk.dashboard.catalog(), queryFn: fetchCatalog });

  const mutation = useApiMutation(saveLayout, { invalidate: [qk.dashboard.layout()] });

  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const pending = useRef<WidgetPlacement[] | null>(null);
  // The mutation object is new on every render; a ref keeps `save` stable so a
  // consumer can pass it straight to onLayoutChange without re-subscribing.
  const mutateRef = useRef(mutation.mutate);
  mutateRef.current = mutation.mutate;

  const flush = useCallback(() => {
    if (timer.current) {
      clearTimeout(timer.current);
      timer.current = null;
    }
    const widgets = pending.current;
    pending.current = null;
    if (widgets) mutateRef.current(widgets);
  }, []);

  const save = useCallback(
    (widgets: WidgetPlacement[]) => {
      pending.current = widgets;
      if (timer.current) clearTimeout(timer.current);
      timer.current = setTimeout(flush, SAVE_DELAY_MS);
    },
    [flush],
  );

  /** Show the caller's arrangement now; the PUT confirms it a moment later. */
  const applyOptimistic = useCallback(
    (widgets: DashboardWidget[]) => {
      qc.setQueryData(qk.dashboard.layout(), widgets);
    },
    [qc],
  );

  // A pending write outlives the page on purpose: see the docblock.
  const flushRef = useRef(flush);
  flushRef.current = flush;
  useEffect(() => () => flushRef.current(), []);

  return {
    layout: layout.data,
    catalog: catalog.data,
    isLoading: layout.isLoading || catalog.isLoading,
    error: layout.error ?? catalog.error,
    save,
    flush,
    applyOptimistic,
    isSaving: mutation.isPending,
    saveError: mutation.error,
  };
}
