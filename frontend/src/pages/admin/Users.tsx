import { Link } from 'react-router-dom';

import { DataTable, type Column } from '@/components/DataTable';
import { PaginationFooter } from '@/components/PaginationFooter';
import { Card, CardFooter } from '@/components/tabler/Card';
import { PageHeader } from '@/components/tabler/PageHeader';
import { usePagedQuery } from '@/hooks/usePagedQuery';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type { User } from '@/lib/api/types';

const PER_PAGE = 20;

const columns: Column<User>[] = [
  { header: 'Email', cell: (u) => <span className="font-monospace">{u.email}</span> },
  { header: 'Name', cell: (u) => u.full_name },
  { header: 'Role', cell: (u) => u.role?.name ?? u.role_id },
  {
    header: 'Confirmed',
    cell: (u) => (
      <span aria-label={u.confirmed ? 'Confirmed' : 'Not confirmed'}>
        <span aria-hidden="true">{u.confirmed ? '✓' : '—'}</span>
      </span>
    ),
  },
  {
    header: '',
    className: 'text-end',
    cell: (u) => (
      <Link to={`/admin/users/${u.id}`} className="btn btn-ghost-secondary btn-sm">
        Edit
      </Link>
    ),
  },
];

export function AdminUsersPage() {
  const { data, isLoading, error, isPlaceholderData, page, setPage, totalPages } = usePagedQuery({
    queryKey: qk.admin.users(),
    queryFn: ({ limit, offset }) =>
      api.getJson('/api/v1/admin/users', { query: { limit, offset } }),
    perPage: PER_PAGE,
  });

  return (
    <>
      <PageHeader
        title="Users"
        pretitle={data ? `${data.total} total` : 'Admin'}
        actions={
          <Link to="/admin/invite" className="btn btn-primary">
            Invite user
          </Link>
        }
      />
      <div className="container-xl">
        <Card>
          <div className="table-responsive">
            <DataTable
              columns={columns}
              rows={data?.data}
              rowKey={(u) => u.id}
              isLoading={isLoading}
              error={error}
              emptyText="No users yet."
              isPlaceholder={isPlaceholderData}
            />
          </div>
          {data && totalPages > 1 && (
            <CardFooter>
              <PaginationFooter
                page={page}
                totalPages={totalPages}
                isPlaceholderData={isPlaceholderData}
                onPageChange={setPage}
              />
            </CardFooter>
          )}
        </Card>
      </div>
    </>
  );
}
