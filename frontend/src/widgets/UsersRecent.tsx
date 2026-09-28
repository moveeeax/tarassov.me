import { useQuery } from '@tanstack/react-query';
import { Link } from 'react-router-dom';

import { Placeholder } from '@/components/tabler/Placeholder';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type { User } from '@/lib/api/types';

/** `options` arrives with every catalog default already filled in
 * (see withOptionDefaults in pages/admin/dashboardLayout.ts), so there is no
 * local fallback to drift from the advertised value. */
export function UsersRecent({ options }: { options: Record<string, number> }) {
  const limit = options.limit;
  const query = useQuery({
    queryKey: [...qk.admin.users(), 'widget', limit],
    queryFn: () => api.getJson('/api/v1/admin/users', { query: { limit, offset: 0 } }),
  });

  if (query.isLoading) return <Placeholder />;
  if (query.error) return <p className="text-danger mb-0">Failed to load users.</p>;

  const rows: User[] = query.data?.data ?? [];

  return (
    <>
      <div className="d-flex align-items-baseline gap-3 mb-3">
        <span className="h1 mb-0">{query.data?.total ?? 0}</span>
        <span className="text-secondary small">registered</span>
      </div>
      <div className="list-group list-group-flush">
        {rows.map((u) => (
          <Link
            key={u.id}
            to={`/admin/users/${u.id}`}
            className="list-group-item list-group-item-action"
          >
            <span className="text-truncate d-block font-monospace small">{u.email}</span>
            <span className="small text-secondary">{u.role?.name ?? u.role_id}</span>
          </Link>
        ))}
      </div>
    </>
  );
}
