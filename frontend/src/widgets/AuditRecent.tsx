import { useQuery } from '@tanstack/react-query';

import { Placeholder } from '@/components/tabler/Placeholder';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type { AuditEntry } from '@/lib/api/types';

export function AuditRecent({ options }: { options: Record<string, number> }) {
  const limit = options.limit ?? 10;
  const query = useQuery({
    queryKey: qk.admin.audit({ widget: String(limit) }),
    queryFn: () => api.getJson('/api/v1/admin/audit', { query: { limit, offset: 0 } }),
  });

  if (query.isLoading) return <Placeholder />;
  if (query.error) return <p className="text-danger mb-0">Failed to load the audit trail.</p>;

  const rows: AuditEntry[] = query.data?.data ?? [];
  if (rows.length === 0) return <p className="text-secondary mb-0">Nothing recorded yet.</p>;

  return (
    <div className="table-responsive">
      <table className="table table-vcenter card-table">
        <thead>
          <tr>
            <th>When</th>
            <th>Action</th>
            <th>Target</th>
          </tr>
        </thead>
        <tbody>
          {rows.map((e) => (
            <tr key={e.id}>
              <td className="text-nowrap small text-secondary">
                {e.created_at.slice(0, 16).replace('T', ' ')}
              </td>
              <td className="font-monospace small">{e.action}</td>
              <td className="small text-secondary">{e.target_type}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}
