import { useMemo, useState } from 'react';
import { Link } from 'react-router-dom';

import { DataTable, type Column } from '@/components/DataTable';
import { PaginationFooter } from '@/components/PaginationFooter';
import { Button } from '@/components/ui/button';
import { Card, CardContent } from '@/components/ui/card';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import { useFocusTrap } from '@/hooks/useFocusTrap';
import { usePagedQuery } from '@/hooks/usePagedQuery';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type { AuditEntry } from '@/lib/api/types';

const PER_PAGE = 50;

interface Filters {
  action: string;
  target_type: string;
  actor_id: string;
  from: string;
  to: string;
}

const EMPTY_FILTERS: Filters = { action: '', target_type: '', actor_id: '', from: '', to: '' };

function formatTimestamp(iso: string): string {
  const d = new Date(iso);
  return Number.isNaN(d.getTime()) ? iso : d.toLocaleString();
}

/**
 * A <input type="datetime-local"> emits `YYYY-MM-DDTHH:mm` (no zone). The
 * backend filter expects an RFC3339 date-time, so we let the browser resolve
 * the local value to an absolute instant (toISOString → UTC `Z`). An empty
 * field stays empty (and is dropped from the query below).
 */
function toRfc3339(local: string): string {
  if (!local) return '';
  const d = new Date(local);
  return Number.isNaN(d.getTime()) ? local : d.toISOString();
}

export function AdminAuditPage() {
  // Draft = what the inputs hold; applied = what the query runs with. The
  // query only re-fires when the user clicks "Apply" (or "Clear"), so typing
  // an actor uuid doesn't spray a request per keystroke.
  const [draft, setDraft] = useState<Filters>(EMPTY_FILTERS);
  const [applied, setApplied] = useState<Filters>(EMPTY_FILTERS);
  const [selected, setSelected] = useState<AuditEntry | null>(null);

  // Only non-empty filters reach the URL — and dates are normalised to RFC3339.
  const query = useMemo<Record<string, string>>(() => {
    const q: Record<string, string> = {};
    if (applied.action) q.action = applied.action;
    if (applied.target_type) q.target_type = applied.target_type;
    if (applied.actor_id) q.actor_id = applied.actor_id;
    if (applied.from) q.from = toRfc3339(applied.from);
    if (applied.to) q.to = toRfc3339(applied.to);
    return q;
  }, [applied]);

  const { data, isLoading, error, isPlaceholderData, page, setPage, totalPages } = usePagedQuery({
    queryKey: qk.admin.audit(query),
    queryFn: ({ limit, offset }) =>
      api.getJson('/api/v1/admin/audit', { query: { limit, offset, ...query } }),
    perPage: PER_PAGE,
  });

  const columns: Column<AuditEntry>[] = [
    {
      header: 'When',
      className: 'whitespace-nowrap',
      cell: (e) => formatTimestamp(e.created_at),
    },
    {
      header: 'Actor',
      className: 'font-mono text-xs',
      cell: (e) => (e.actor_id ? `${e.actor_id.slice(0, 8)}…` : 'system'),
    },
    { header: 'Action', className: 'font-mono text-xs', cell: (e) => e.action },
    { header: 'Target', cell: (e) => e.target_type },
    {
      header: 'Target id',
      className: 'font-mono text-xs',
      cell: (e) => e.target_id || '—',
    },
    {
      header: 'Details',
      cell: (e) =>
        e.details && Object.keys(e.details).length > 0 ? (
          <span className="text-primary underline-offset-2 hover:underline">view</span>
        ) : (
          <span className="text-muted-foreground">—</span>
        ),
    },
  ];

  function apply() {
    setApplied(draft);
    setPage(1);
  }

  function clear() {
    setDraft(EMPTY_FILTERS);
    setApplied(EMPTY_FILTERS);
    setPage(1);
  }

  return (
    <div className="container mx-auto py-8 space-y-6">
      <div className="flex items-center justify-between">
        <h1 className="text-3xl font-bold">Audit log</h1>
        <Button asChild variant="ghost">
          <Link to="/admin">← Admin</Link>
        </Button>
      </div>

      <Card>
        <CardContent className="pt-6">
          <form
            className="grid gap-4 sm:grid-cols-2 lg:grid-cols-5"
            onSubmit={(ev) => {
              ev.preventDefault();
              apply();
            }}
          >
            <div className="space-y-1">
              <Label htmlFor="f-action">Action</Label>
              <Input
                id="f-action"
                placeholder="user.create"
                value={draft.action}
                onChange={(e) => setDraft({ ...draft, action: e.target.value.trim() })}
              />
            </div>
            <div className="space-y-1">
              <Label htmlFor="f-target">Target type</Label>
              <Input
                id="f-target"
                placeholder="user / role"
                value={draft.target_type}
                onChange={(e) => setDraft({ ...draft, target_type: e.target.value.trim() })}
              />
            </div>
            <div className="space-y-1">
              <Label htmlFor="f-actor">Actor id</Label>
              <Input
                id="f-actor"
                placeholder="uuid"
                value={draft.actor_id}
                onChange={(e) => setDraft({ ...draft, actor_id: e.target.value.trim() })}
              />
            </div>
            <div className="space-y-1">
              <Label htmlFor="f-from">From</Label>
              <Input
                id="f-from"
                type="datetime-local"
                value={draft.from}
                onChange={(e) => setDraft({ ...draft, from: e.target.value })}
              />
            </div>
            <div className="space-y-1">
              <Label htmlFor="f-to">To</Label>
              <Input
                id="f-to"
                type="datetime-local"
                value={draft.to}
                onChange={(e) => setDraft({ ...draft, to: e.target.value })}
              />
            </div>
            <div className="flex items-end gap-2 sm:col-span-2 lg:col-span-5">
              <Button type="submit">Apply</Button>
              <Button type="button" variant="ghost" onClick={clear}>
                Clear
              </Button>
              {data && (
                <span className="ml-auto self-center text-sm text-muted-foreground">
                  {data.total} total
                </span>
              )}
            </div>
          </form>
        </CardContent>
      </Card>

      <Card>
        <CardContent className="overflow-x-auto pt-6">
          <DataTable
            columns={columns}
            rows={data?.data}
            rowKey={(e) => e.id}
            isLoading={isLoading}
            error={error}
            emptyText="No audit entries match these filters."
            isPlaceholder={isPlaceholderData}
            rowProps={(e) => ({
              className: `cursor-pointer hover:bg-accent ${selected?.id === e.id ? 'bg-accent' : ''}`,
              onClick: () => setSelected(selected?.id === e.id ? null : e),
            })}
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

      {selected && <AuditDetailModal entry={selected} onClose={() => setSelected(null)} />}
    </div>
  );
}

/**
 * Detail view as a centered modal over the page (was a card appended at the
 * bottom — you had to scroll past the whole table to see it). Closes on the
 * backdrop, the Close button, or Escape.
 */
function AuditDetailModal({ entry, onClose }: { entry: AuditEntry; onClose: () => void }) {
  const ref = useFocusTrap<HTMLDivElement>(onClose);

  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center bg-black/50 p-4"
      onClick={onClose}
    >
      <Card
        ref={ref}
        role="dialog"
        aria-modal="true"
        aria-labelledby="audit-detail-title"
        tabIndex={-1}
        className="max-h-[85vh] w-full max-w-xs sm:max-w-lg overflow-auto"
        onClick={(ev) => ev.stopPropagation()}
      >
        <CardContent className="space-y-3 pt-6 text-sm">
          <div className="flex items-start justify-between gap-4">
            <div>
              <p id="audit-detail-title" className="font-mono text-base">
                {entry.action}
              </p>
              <p className="text-muted-foreground">
                {formatTimestamp(entry.created_at)} · {entry.target_type}
                {entry.target_id ? ` ${entry.target_id}` : ''}
              </p>
            </div>
            <Button size="sm" variant="ghost" onClick={onClose}>
              Close
            </Button>
          </div>
          <div>
            <p className="mb-1 font-medium">Details</p>
            <pre className="overflow-x-auto rounded bg-muted p-3 text-xs">
              {JSON.stringify(entry.details, null, 2)}
            </pre>
          </div>
        </CardContent>
      </Card>
    </div>
  );
}
