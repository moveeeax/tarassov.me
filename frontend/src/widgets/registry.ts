import type { ComponentType } from 'react';

import { AuditRecent } from './AuditRecent';
import { JobsQueue } from './JobsQueue';
import { PostsSummary } from './PostsSummary';
import { ServiceHealth } from './ServiceHealth';
import { UsersRecent } from './UsersRecent';

export type WidgetComponent = ComponentType<{ options: Record<string, number> }>;

/**
 * Widget type → component. The KEYS must match Domain::Widgets::kCatalog on the
 * backend exactly; registry.test.ts pins that, because a type without a
 * component here renders an empty card and nothing else complains.
 */
export const WIDGET_COMPONENTS: Record<string, WidgetComponent> = {
  posts_summary: PostsSummary,
  jobs_queue: JobsQueue,
  audit_recent: AuditRecent,
  users_recent: UsersRecent,
  service_health: ServiceHealth,
};
