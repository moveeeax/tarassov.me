import { useMemo, useState } from 'react';
import { Link } from 'react-router-dom';

import { DataTable, type Column } from '@/components/DataTable';
import { PaginationFooter } from '@/components/PaginationFooter';
import { Button } from '@/components/tabler/Button';
import { PageHeader } from '@/components/tabler/PageHeader';
import { Card, CardBody } from '@/components/tabler/Card';
import { Input } from '@/components/tabler/Input';
import { Label } from '@/components/tabler/Input';
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
      className: 'text-nowrap',
      cell: (e) => formatTimestamp(e.created_at),
    },
    {
      header: 'Actor',
      className: 'font-monospace small',
      cell: (e) => (e.actor_id ? `${e.actor_id.slice(0, 8)}…` : 'system'),
    },
    { header: 'Action', className: 'font-monospace small', cell: (e) => e.action },
    { header: 'Target', cell: (e) => e.target_type },
    {
      header: 'Target id',
      className: 'font-monospace small',
      cell: (e) => e.target_id || '—',
    },
    {
      header: 'Details',
      cell: (e) =>
        e.details && Object.keys(e.details).length > 0 ? (
          <span className="text-primary">view</span>
        ) : (
          <span className="text-secondary">—</span>
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
    <>
      <PageHeader
        title="Audit log"
        pretitle="Admin"
        actions={
          <Link to="/admin" className="btn btn-ghost-secondary">
            ← Admin
          </Link>
        }
      />
      <div className="container-xl vstack gap-3">
        <Card>
          <CardBody className="pt-4">
            <form
              className="row g-2 align-items-end"
              onSubmit={(ev) => {
                ev.preventDefault();
                apply();
              }}
            >
              <div className="col-sm-6 col-lg">
                <Label htmlFor="f-action">Action</Label>
                <Input
                  id="f-action"
                  placeholder="user.create"
                  value={draft.action}
                  onChange={(e) => setDraft({ ...draft, action: e.target.value.trim() })}
                />
              </div>
              <div className="col-sm-6 col-lg">
                <Label htmlFor="f-target">Target type</Label>
                <Input
                  id="f-target"
                  placeholder="user / role"
                  value={draft.target_type}
                  onChange={(e) => setDraft({ ...draft, target_type: e.target.value.trim() })}
                />
              </div>
              <div className="col-sm-6 col-lg">
                <Label htmlFor="f-actor">Actor id</Label>
                <Input
                  id="f-actor"
                  placeholder="uuid"
                  value={draft.actor_id}
                  onChange={(e) => setDraft({ ...draft, actor_id: e.target.value.trim() })}
                />
              </div>
              <div className="col-sm-6 col-lg">
                <Label htmlFor="f-from">From</Label>
                <Input
                  id="f-from"
                  type="datetime-local"
                  value={draft.from}
                  onChange={(e) => setDraft({ ...draft, from: e.target.value })}
                />
              </div>
              <div className="col-sm-6 col-lg">
                <Label htmlFor="f-to">To</Label>
                <Input
                  id="f-to"
                  type="datetime-local"
                  value={draft.to}
                  onChange={(e) => setDraft({ ...draft, to: e.target.value })}
                />
              </div>
              <div className="d-flex align-items-end gap-2 col-12">
                <Button type="submit">Apply</Button>
                <Button type="button" variant="ghost" onClick={clear}>
                  Clear
                </Button>
                {data && (
                  <span className="ms-auto align-self-center small text-secondary">
                    {data.total} total
                  </span>
                )}
              </div>
            </form>
          </CardBody>
        </Card>

        <Card>
          <CardBody className="table-responsive pt-4">
            <DataTable
              columns={columns}
              rows={data?.data}
              rowKey={(e) => e.id}
              isLoading={isLoading}
              error={error}
              emptyText="No audit entries match these filters."
              isPlaceholder={isPlaceholderData}
              hoverable
              rowProps={(e) => ({
                className: `cursor-pointer ${selected?.id === e.id ? 'table-active' : ''}`,
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
          </CardBody>
        </Card>

        {selected && <AuditDetailModal entry={selected} onClose={() => setSelected(null)} />}
      </div>
    </>
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
    <>
      <div className="modal-backdrop fade show" />
      <div className="modal modal-blur fade show d-block" tabIndex={-1} onClick={onClose}>
        <div
          className="modal-dialog modal-lg modal-dialog-centered modal-dialog-scrollable"
          role="document"
          onClick={(ev) => ev.stopPropagation()}
        >
          <div
            ref={ref}
            role="dialog"
            aria-modal="true"
            aria-labelledby="audit-detail-title"
            tabIndex={-1}
            className="modal-content"
          >
            <div className="modal-body vstack gap-3 small">
              <div className="d-flex align-items-start justify-content-between gap-4">
                <div>
                  <p id="audit-detail-title" className="font-monospace">
                    {entry.action}
                  </p>
                  <p className="text-secondary">
                    {formatTimestamp(entry.created_at)} · {entry.target_type}
                    {entry.target_id ? ` ${entry.target_id}` : ''}
                  </p>
                </div>
                <Button size="sm" variant="ghost" onClick={onClose}>
                  Close
                </Button>
              </div>
              <div>
                <p className="mb-1 fw-medium">Details</p>
                <pre className="table-responsive rounded bg-muted p-3 small">
                  {JSON.stringify(entry.details, null, 2)}
                </pre>
              </div>
            </div>
          </div>
        </div>
      </div>
    </>
  );
}
