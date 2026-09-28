import { useState } from 'react';
import { Link } from 'react-router-dom';
import { IconTrash } from '@tabler/icons-react';

import { ConfirmDialog } from '@/components/ConfirmDialog';
import { DataTable, type Column } from '@/components/DataTable';
import { PaginationFooter } from '@/components/PaginationFooter';
import { Button } from '@/components/tabler/Button';
import { Card, CardFooter } from '@/components/tabler/Card';
import { PageHeader } from '@/components/tabler/PageHeader';
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
      className: 'w-1',
      cell: (u) => (
        <a href={u.url} target="_blank" rel="noopener">
          <img
            src={u.url}
            alt={u.name}
            width={40}
            height={40}
            className="rounded object-cover"
            loading="lazy"
          />
        </a>
      ),
    },
    { header: 'Name', className: 'font-monospace small', cell: (u) => u.name },
    { header: 'Size', className: 'small', cell: (u) => fmtSize(u.size_bytes) },
    { header: 'Type', className: 'small', cell: (u) => u.content_type },
    { header: 'Uploaded', className: 'small', cell: (u) => u.created_at.slice(0, 10) },
    {
      header: '',
      className: 'text-end',
      cell: (u) => (
        <Button
          size="sm"
          variant="ghost"
          aria-label={`Delete ${u.name}`}
          onClick={() => setDeleting(u)}
        >
          <IconTrash size={16} className="text-danger" />
        </Button>
      ),
    },
  ];

  return (
    <>
      <PageHeader
        title="Media"
        pretitle={data ? `${data.total} file(s)` : 'Admin'}
        actions={
          <Link to="/admin" className="btn btn-ghost-secondary">
            ← Admin
          </Link>
        }
      />
      <div className="container-xl">
        <Card>
          <div className="card-body py-2 small text-secondary">
            Images uploaded from the post editor. Deleting a file a post still embeds leaves a
            broken image.
          </div>
          <div className="table-responsive">
            <DataTable
              columns={columns}
              rows={data?.data}
              rowKey={(u) => u.key}
              isLoading={isLoading}
              error={error}
              emptyText="No uploads yet."
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
    </>
  );
}
