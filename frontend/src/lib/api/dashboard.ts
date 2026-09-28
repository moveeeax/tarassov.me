import { api } from '@/lib/api/client';

/** One placed widget as the backend stores it. */
export interface DashboardWidget {
  id: string;
  widget_type: string;
  grid_x: number;
  grid_y: number;
  grid_w: number;
  grid_h: number;
  options: Record<string, number>;
  created_at: string;
  updated_at: string;
}

/** One type the caller may place, with its geometry defaults and option bounds. */
export interface CatalogEntry {
  type: string;
  title: string;
  description: string;
  default_w: number;
  default_h: number;
  min_w: number;
  min_h: number;
  options: Record<string, { type: 'int'; min: number; max: number; default: number }>;
}

/** What PUT accepts: no id, the server mints one per placement. */
export interface WidgetPlacement {
  widget_type: string;
  grid_x: number;
  grid_y: number;
  grid_w: number;
  grid_h: number;
  options?: Record<string, number>;
}

export async function fetchLayout(): Promise<DashboardWidget[]> {
  const body = await api.getJson<{ data: DashboardWidget[] }>('/api/v1/dashboard/layout');
  return body.data;
}

export async function fetchCatalog(): Promise<CatalogEntry[]> {
  const body = await api.getJson<{ data: CatalogEntry[] }>('/api/v1/dashboard/catalog');
  return body.data;
}

export async function saveLayout(widgets: WidgetPlacement[]): Promise<DashboardWidget[]> {
  const body = await api.putJson<{ data: DashboardWidget[] }>('/api/v1/dashboard/layout', {
    body: { widgets },
  });
  return body.data;
}
