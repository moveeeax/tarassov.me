import { useState } from 'react';
import { Link } from 'react-router-dom';
import { Trash2 } from 'lucide-react';

import { ConfirmDialog } from '@/components/ConfirmDialog';
import { DataTable, type Column } from '@/components/DataTable';
import { PaginationFooter } from '@/components/PaginationFooter';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useErrorToast } from '@/hooks/useErrorToast';
import { usePagedQuery } from '@/hooks/usePagedQuery';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';

const PER_PAGE = 50;

/**
 * AdminMediaPage — media library over /api/v1/admin/uploads. Lists the images
 * the post editor uploaded (newest first) and deletes them. There is no
 * usage tracking: deleting a file that a post still embeds leaves a broken
 * image — the confirm dialog says so.
 */

interface UploadItem {
  key: string;
  name: string;
  url: string;
  size_bytes: number;
  content_type: string;
  created_at: string;
}

function fmtSize(n: number): string {
  if (n >= 1024 * 1024) return `${(n / (1024 * 1024)).toFixed(1)} MB`;
  if (n >= 1024) return `${(n / 1024).toFixed(0)} KB`;
  return `${n} B`;
}

export function AdminMediaPage() {
  const [deleting, setDeleting] = useState<UploadItem | null>(null);

  const { data, isLoading, error, isPlaceholderData, page, setPage, totalPages } = usePagedQuery({
    queryKey: qk.admin.media(),
    queryFn: ({ limit, offset }) =>
      api.getJson<{ data: UploadItem[]; total: number }>('/api/v1/admin/uploads', {
        query: { limit, offset },
      }),
    perPage: PER_PAGE,
  });

  const remove = useApiMutation(
    (name: string) => api.deleteJson(`/api/v1/admin/uploads/${encodeURIComponent(name)}`),
    { invalidate: [qk.admin.media()], onSuccess: () => setDeleting(null) },
  );
  useErrorToast(remove.error);

  const columns: Column<UploadItem>[] = [
    {
      header: '',
      className: 'w-16',
      cell: (u) => (
        <a href={u.url} target="_blank" rel="noopener">
          <img src={u.url} alt={u.name} className="h-10 w-10 rounded object-cover" loading="lazy" />
        </a>
      ),
    },
    { header: 'Name', className: 'font-mono text-xs', cell: (u) => u.name },
    { header: 'Size', className: 'text-xs', cell: (u) => fmtSize(u.size_bytes) },
    { header: 'Type', className: 'text-xs', cell: (u) => u.content_type },
    { header: 'Uploaded', className: 'text-xs', cell: (u) => u.created_at.slice(0, 10) },
    {
      header: '',
      className: 'text-right',
      cell: (u) => (
        <Button size="sm" variant="ghost" onClick={() => setDeleting(u)}>
          <Trash2 className="h-3.5 w-3.5 text-destructive" />
        </Button>
      ),
    },
  ];

  return (
    <div className="container mx-auto max-w-4xl py-8 space-y-6">
      <div className="flex items-center justify-between">
        <div>
          <h1 className="text-3xl font-bold">Media</h1>
          <p className="text-sm text-muted-foreground">
            Images uploaded from the post editor. Deleting a file a post still embeds leaves a
            broken image.
          </p>
        </div>
        <Button asChild variant="ghost">
          <Link to="/admin">← Admin</Link>
        </Button>
      </div>

      <Card>
        <CardHeader>
          <CardTitle>{data ? `${data.total} file(s)` : 'Loading…'}</CardTitle>
        </CardHeader>
        <CardContent className="overflow-x-auto">
          <DataTable
            columns={columns}
            rows={data?.data}
            rowKey={(u) => u.key}
            isLoading={isLoading}
            error={error}
            emptyText="No uploads yet."
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

      {deleting && (
        <ConfirmDialog
          title="Delete file"
          description={`Delete "${deleting.name}"? Posts that embed it will show a broken image. This cannot be undone.`}
          confirmLabel="Delete file"
          destructive
          busy={remove.isPending}
          onConfirm={() => remove.mutate(deleting.name)}
          onClose={() => setDeleting(null)}
        />
      )}
    </div>
  );
}
