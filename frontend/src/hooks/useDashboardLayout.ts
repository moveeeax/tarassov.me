import { useCallback, useEffect, useRef } from 'react';
import { useQuery } from '@tanstack/react-query';

import { fetchCatalog, fetchLayout, saveLayout, type WidgetPlacement } from '@/lib/api/dashboard';
import { qk } from '@/lib/api/queryKeys';
import { useApiMutation } from '@/hooks/useApiMutation';

/** Debounce for layout writes: dragging fires continuously, the server needs one PUT. */
const SAVE_DELAY_MS = 800;

/**
 * Reads the layout and the catalog, and writes the layout back debounced.
 * Only the arrangement at the END of a drag is worth a request, so `save`
 * coalesces calls and the timer is cleared on unmount — a pending write must
 * not fire after the page is gone.
 */
export function useDashboardLayout() {
  const layout = useQuery({ queryKey: qk.dashboard.layout(), queryFn: fetchLayout });
  const catalog = useQuery({ queryKey: qk.dashboard.catalog(), queryFn: fetchCatalog });

  const mutation = useApiMutation(saveLayout, { invalidate: [qk.dashboard.layout()] });

  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const pending = useRef<WidgetPlacement[] | null>(null);
  // The mutation object is new on every render; a ref keeps `save` stable so a
  // consumer can pass it straight to onLayoutChange without re-subscribing.
  const mutateRef = useRef(mutation.mutate);
  mutateRef.current = mutation.mutate;

  const save = useCallback((widgets: WidgetPlacement[]) => {
    pending.current = widgets;
    if (timer.current) clearTimeout(timer.current);
    timer.current = setTimeout(() => {
      timer.current = null;
      if (pending.current) mutateRef.current(pending.current);
      pending.current = null;
    }, SAVE_DELAY_MS);
  }, []);

  useEffect(
    () => () => {
      if (timer.current) clearTimeout(timer.current);
    },
    [],
  );

  return {
    layout: layout.data,
    catalog: catalog.data,
    isLoading: layout.isLoading || catalog.isLoading,
    error: layout.error ?? catalog.error,
    save,
    isSaving: mutation.isPending,
    saveError: mutation.error,
  };
}
