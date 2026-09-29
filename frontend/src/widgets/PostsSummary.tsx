import { useQuery } from '@tanstack/react-query';
import { Link } from 'react-router-dom';

import { Placeholder } from '@/components/tabler/Placeholder';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';

interface PostRow {
  id: string;
  title: string;
  slug: string;
  status: string;
}

/** `options` arrives with every catalog default already filled in
 * (see withOptionDefaults in pages/admin/dashboardLayout.ts), so there is no
 * local fallback to drift from the advertised value. */
export function PostsSummary({ options }: { options: Record<string, number> }) {
  const limit = options.limit;
  const query = useQuery({
    queryKey: qk.admin.posts(`widget:${limit}`),
    queryFn: () =>
      api.getJson<{ data: PostRow[]; total: number }>('/api/v1/posts', {
        query: { limit, offset: 0 },
      }),
  });

  if (query.isLoading) return <Placeholder />;
  if (query.error) return <p className="text-danger mb-0">Failed to load posts.</p>;

  const rows = query.data?.data ?? [];
  const published = rows.filter((p) => p.status === 'published').length;

  return (
    <>
      <div className="d-flex align-items-baseline gap-3 mb-3">
        <span className="h1 mb-0">{query.data?.total ?? 0}</span>
        <span className="text-secondary small">
          {published} published in the latest {rows.length}
        </span>
      </div>
      <div className="list-group list-group-flush">
        {rows.map((p) => (
          <Link key={p.id} to="/admin/posts" className="list-group-item list-group-item-action">
            <span className="text-truncate d-block">{p.title}</span>
            <span className="small text-secondary">{p.status}</span>
          </Link>
        ))}
      </div>
    </>
  );
}
