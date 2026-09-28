import { api } from '@/lib/api/client';
import type { paths } from '@/lib/api/schema.gen';
import type { DashboardCatalogEntry, DashboardWidget } from '@/lib/api/types';

export type { DashboardCatalogEntry as CatalogEntry, DashboardWidget };

type LayoutPutBody = NonNullable<
  paths['/api/v1/dashboard/layout']['put']['requestBody']
>['content']['application/json'];

/**
 * What PUT accepts: no id, the server mints one per placement. Derived from the
 * generated request body so the shape cannot drift from docs/openapi.yaml.
 */
export type WidgetPlacement = NonNullable<LayoutPutBody['widgets']>[number];

export async function fetchLayout(): Promise<DashboardWidget[]> {
  const body = await api.getJson('/api/v1/dashboard/layout');
  return body.data;
}

export async function fetchCatalog(): Promise<DashboardCatalogEntry[]> {
  const body = await api.getJson('/api/v1/dashboard/catalog');
  return body.data;
}

export async function saveLayout(widgets: WidgetPlacement[]): Promise<DashboardWidget[]> {
  const body = await api.putJson('/api/v1/dashboard/layout', { body: { widgets } });
  return body.data;
}
