import { Link } from 'react-router-dom';

import { DataTable, type Column } from '@/components/DataTable';
import { PaginationFooter } from '@/components/PaginationFooter';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { usePagedQuery } from '@/hooks/usePagedQuery';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type { User } from '@/lib/api/types';

const PER_PAGE = 20;

const columns: Column<User>[] = [
  { header: 'Email', cell: (u) => <span className="font-mono">{u.email}</span> },
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
    className: 'text-right',
    cell: (u) => (
      <Button variant="ghost" size="sm" asChild>
        <Link to={`/admin/users/${u.id}`}>Edit</Link>
      </Button>
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
    <div className="container mx-auto py-8 space-y-6">
      <div className="flex items-center justify-between">
        <h1 className="text-3xl font-bold">Users</h1>
        <Button asChild>
          <Link to="/admin/invite">Invite user</Link>
        </Button>
      </div>
      <Card>
        <CardHeader>
          <CardTitle>{data ? `${data.total} total` : 'Users'}</CardTitle>
        </CardHeader>
        <CardContent className="overflow-x-auto">
          <DataTable
            columns={columns}
            rows={data?.data}
            rowKey={(u) => u.id}
            isLoading={isLoading}
            error={error}
            emptyText="No users yet."
            isPlaceholder={isPlaceholderData}
          />
          {data && (
            <PaginationFooter
              page={page}
              totalPages={totalPages}
              isPlaceholderData={isPlaceholderData}
              onPageChange={setPage}
            />
          )}
        </CardContent>
      </Card>
    </div>
  );
}
