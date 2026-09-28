import { useState } from 'react';
import { Link } from 'react-router-dom';
import { useQuery } from '@tanstack/react-query';
import { IconRotateClockwise } from '@tabler/icons-react';

import { DataTable, type Column } from '@/components/DataTable';
import { PaginationFooter } from '@/components/PaginationFooter';
import { Button } from '@/components/tabler/Button';
import { PageHeader } from '@/components/tabler/PageHeader';
import { Card, CardBody, CardHeader, CardTitle } from '@/components/tabler/Card';
import { Input } from '@/components/tabler/Input';
import { usePagedQuery } from '@/hooks/usePagedQuery';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useErrorToast } from '@/hooks/useErrorToast';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type { DlqListResponse, Job } from '@/lib/api/types';

const PER_PAGE = 20;

/**
 * Jaeger UI base for the "Open trace" deep link, or null when this deployment
 * has no trace UI configured — in which case the button is not rendered at all.
 *
 * The URL must be configured explicitly (VITE_TRACE_UI_URL at build time); it
 * is NOT derived from the current origin. Swapping the first DNS label for
 * "jaeger" only holds for an `app.<env>.<domain>` deployment: on an apex domain
 * (`tarassov.me`) it resolves to `jaeger.me` — an unrelated third party — and
 * on any other shape to a host that does not exist. Local dev still falls back
 * to the docker-compose Jaeger.
 */
function resolveTraceUiUrl(): string | null {
  if (import.meta.env.VITE_TRACE_UI_URL) return import.meta.env.VITE_TRACE_UI_URL;
  if (typeof window === 'undefined') return null;
  const { hostname } = window.location;
  if (hostname === 'localhost' || hostname === '127.0.0.1') return 'http://localhost:16686';
  return null;
}

const TRACE_UI_URL = resolveTraceUiUrl();

// Thin-bordered, low-chroma badges that read on both themes. Dark-first
// (the app default), with a light-mode fallback that matches the palette.
const STATUS_STYLES: Record<Job['status'], string> = {
  pending:
    'border-amber-300 bg-amber-50 text-amber-700 dark:border-amber-500/30 dark:bg-amber-500/10 dark:text-amber-300',
  processing:
    'border-indigo-300 bg-indigo-50 text-indigo-700 dark:border-indigo-500/30 dark:bg-indigo-500/10 dark:text-indigo-300',
  completed:
    'border-emerald-300 bg-emerald-50 text-emerald-700 dark:border-emerald-500/30 dark:bg-emerald-500/10 dark:text-emerald-300',
  failed:
    'border-orange-300 bg-orange-50 text-orange-700 dark:border-orange-500/30 dark:bg-orange-500/10 dark:text-orange-300',
  dead: 'border-red-300 bg-red-50 text-red-700 dark:border-red-500/30 dark:bg-red-500/10 dark:text-red-300',
};

function StatusBadge({ status }: { status: Job['status'] }) {
  return (
    <span
      className={`d-inline-flex align-items-center rounded border px-2 py-1 small fw-medium ${STATUS_STYLES[status] ?? ''}`}
    >
      {status}
    </span>
  );
}

function formatEpoch(sec: number | undefined): string {
  return sec ? new Date(sec * 1000).toLocaleString() : '—';
}

export function AdminJobsPage() {
  const [tab, setTab] = useState<'jobs' | 'dlq'>('jobs');

  return (
    <>
      <PageHeader
        title="Jobs"
        pretitle="Admin"
        actions={
          <Link to="/admin" className="btn btn-ghost-secondary">
            ← Admin
          </Link>
        }
      />
      <div className="container-xl vstack gap-3">
        <ul className="nav nav-tabs" role="tablist">
          {(['jobs', 'dlq'] as const).map((t) => (
            <li key={t} className="nav-item" role="presentation">
              <button
                type="button"
                role="tab"
                aria-selected={tab === t}
                className={tab === t ? 'nav-link active' : 'nav-link'}
                onClick={() => setTab(t)}
              >
                {t === 'jobs' ? 'All jobs' : 'Dead letter queue'}
              </button>
            </li>
          ))}
        </ul>

        {tab === 'jobs' ? <JobsTab /> : <DlqTab />}
      </div>
    </>
  );
}

function JobsTab() {
  const [typeFilter, setTypeFilter] = useState('');
  const [selected, setSelected] = useState<Job | null>(null);

  const { data, isLoading, error, isPlaceholderData, page, setPage, totalPages } = usePagedQuery({
    queryKey: qk.admin.jobs(typeFilter),
    queryFn: ({ limit, offset }) =>
      api.getJson('/api/v1/jobs', {
        query: { limit, offset, ...(typeFilter ? { type: typeFilter } : {}) },
      }),
    perPage: PER_PAGE,
    refetchInterval: 5000,
  });

  const columns: Column<Job>[] = [
    { header: 'ID', className: 'font-monospace small', cell: (j) => `${j.id.slice(0, 8)}…` },
    { header: 'Type', className: 'font-monospace', cell: (j) => j.type },
    { header: 'Status', cell: (j) => <StatusBadge status={j.status} /> },
    { header: 'Retries', cell: (j) => `${j.retry_count ?? 0}/${j.max_retries ?? 0}` },
    { header: 'Worker', className: 'font-monospace small', cell: (j) => j.worker_id || '—' },
    { header: 'Created', className: 'text-nowrap', cell: (j) => formatEpoch(j.created_at) },
    { header: 'Updated', className: 'text-nowrap', cell: (j) => formatEpoch(j.updated_at) },
  ];

  return (
    <div className="vstack gap-4">
      <div className="d-flex align-items-center gap-2">
        <Input
          placeholder="Filter by type (exact, e.g. account_email)"
          value={typeFilter}
          onChange={(e) => {
            setTypeFilter(e.target.value.trim());
            setPage(1);
          }}
          className="w-auto"
        />
        {data && <span className="small text-secondary">{data.total} total</span>}
      </div>

      <Card>
        <CardBody className="table-responsive pt-4">
          <DataTable
            columns={columns}
            rows={data?.data}
            rowKey={(j) => j.id}
            isLoading={isLoading}
            error={error}
            emptyText="No jobs recorded yet."
            isPlaceholder={isPlaceholderData}
            hoverable
            rowProps={(j) => ({
              className: `cursor-pointer ${selected?.id === j.id ? 'table-active' : ''}`,
              onClick: () => setSelected(selected?.id === j.id ? null : j),
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

      {selected && <JobDetailCard job={selected} onClose={() => setSelected(null)} />}
    </div>
  );
}

function JobDetailCard({ job, onClose }: { job: Job; onClose: () => void }) {
  return (
    <Card>
      <CardHeader className="justify-content-between">
        <CardTitle className="font-monospace">{job.id}</CardTitle>
        <div className="btn-list">
          {job.trace_id && TRACE_UI_URL && (
            <a
              href={`${TRACE_UI_URL}/trace/${job.trace_id}`}
              target="_blank"
              rel="noopener noreferrer"
              className="btn btn-sm"
            >
              Open trace
            </a>
          )}
          {job.trace_id && !TRACE_UI_URL && (
            <span
              className="align-self-center user-select-all font-monospace small text-secondary"
              title="No trace UI configured (set VITE_TRACE_UI_URL at build time)"
            >
              {job.trace_id}
            </span>
          )}
          <Button size="sm" variant="ghost" onClick={onClose}>
            Close
          </Button>
        </div>
      </CardHeader>
      <CardBody className="vstack gap-3 small">
        <JobTimeline job={job} />
        <DetailJson label="Payload" value={job.payload} />
        {job.result != null && <DetailJson label="Result" value={job.result} />}
        {job.error && (
          <div>
            <p className="fw-medium mb-1">Last error</p>
            <pre
              className="rounded bg-danger-lt text-danger p-3 small"
              style={{ whiteSpace: 'pre-wrap' }}
            >
              {job.error}
            </pre>
          </div>
        )}
      </CardBody>
    </Card>
  );
}

/** Event timeline reconstructed from the job's own fields. */
function JobTimeline({ job }: { job: Job }) {
  const events: string[] = [`${formatEpoch(job.created_at)} — submitted (type ${job.type})`];
  if (job.worker_id) events.push(`picked by ${job.worker_id}`);
  if ((job.retry_count ?? 0) > 0)
    events.push(`retried ${job.retry_count}/${job.max_retries} time(s)`);
  events.push(`${formatEpoch(job.updated_at)} — ${job.status}`);
  return (
    <ol className="border-start ps-4 vstack gap-1">
      {events.map((e, i) => (
        <li key={i} className="text-secondary">
          {e}
        </li>
      ))}
    </ol>
  );
}

function DetailJson({ label, value }: { label: string; value: unknown }) {
  return (
    <div>
      <p className="fw-medium mb-1">{label}</p>
      <pre className="rounded bg-muted p-3 small table-responsive">
        {JSON.stringify(value, null, 2)}
      </pre>
    </div>
  );
}

function DlqTab() {
  const {
    data,
    isLoading,
    error: loadError,
  } = useQuery({
    queryKey: qk.admin.jobsDlq(),
    queryFn: () => api.getJson<DlqListResponse>('/api/v1/jobs/dlq?limit=100'),
    refetchInterval: 5000,
  });

  const requeue = useApiMutation(
    (id: string) => api.postJson(`/api/v1/jobs/dlq/${id}/requeue`, { body: {} }),
    // Refresh the DLQ so the requeued row disappears (preventing a double
    // requeue); also bump the jobs list since the job is back in flight.
    { invalidate: [qk.admin.jobsDlq(), qk.admin.jobs()] },
  );
  useErrorToast(requeue.error);

  const columns: Column<Job>[] = [
    { header: 'ID', className: 'font-monospace small', cell: (j) => `${j.id.slice(0, 8)}…` },
    { header: 'Type', className: 'font-monospace', cell: (j) => j.type },
    {
      header: 'Error',
      className: 'text-danger',
      cell: (j) => (
        <span title={j.error} className="d-block text-truncate" style={{ maxWidth: '24rem' }}>
          {j.error || '—'}
        </span>
      ),
    },
    { header: 'Died at', className: 'text-nowrap', cell: (j) => formatEpoch(j.updated_at) },
    {
      header: '',
      className: 'text-end',
      cell: (j) => (
        <Button
          size="sm"
          variant="outline"
          disabled={requeue.isPending}
          onClick={() => requeue.mutate(j.id)}
        >
          <IconRotateClockwise size={16} className="me-1" />
          Requeue
        </Button>
      ),
    },
  ];

  return (
    <div className="vstack gap-4">
      <Card>
        <CardHeader>
          <CardTitle>{data ? `${data.depth} job(s) in DLQ` : 'Dead letter queue'}</CardTitle>
        </CardHeader>
        <CardBody className="table-responsive">
          <DataTable
            columns={columns}
            rows={data?.data}
            rowKey={(j) => j.id}
            isLoading={isLoading}
            error={loadError}
            emptyText="DLQ is empty — nothing exhausted its retries."
          />
        </CardBody>
      </Card>
    </div>
  );
}
