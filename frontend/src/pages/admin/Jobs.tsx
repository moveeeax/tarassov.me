import { useState } from 'react';
import { Link } from 'react-router-dom';
import { useQuery } from '@tanstack/react-query';
import { RotateCcw } from 'lucide-react';

import { DataTable, type Column } from '@/components/DataTable';
import { PaginationFooter } from '@/components/PaginationFooter';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { Input } from '@/components/ui/input';
import { usePagedQuery } from '@/hooks/usePagedQuery';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useErrorToast } from '@/hooks/useErrorToast';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type { DlqListResponse, Job } from '@/lib/api/types';

const PER_PAGE = 20;

/**
 * Jaeger UI base for the "Open trace" deep link. The SPA image is built once and
 * deployed to every env, so a build-time URL can't be right everywhere. Resolve
 * at RUNTIME from the current origin using the cpp-env umbrella host convention
 * (`app.<env>.<domain>` → `jaeger.<env>.<domain>`); fall back to the docker-compose
 * Jaeger for local dev. A build-time VITE_TRACE_UI_URL still wins for custom infra.
 */
function resolveTraceUiUrl(): string {
  if (import.meta.env.VITE_TRACE_UI_URL) return import.meta.env.VITE_TRACE_UI_URL;
  if (typeof window === 'undefined') return 'http://localhost:16686';
  const { hostname, protocol } = window.location;
  if (hostname === 'localhost' || hostname === '127.0.0.1') return 'http://localhost:16686';
  const labels = hostname.split('.');
  labels[0] = 'jaeger'; // app.<env>.<domain> -> jaeger.<env>.<domain>
  return `${protocol}//${labels.join('.')}`;
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
      className={`inline-flex items-center rounded border px-2 py-0.5 text-xs font-medium ${STATUS_STYLES[status] ?? ''}`}
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
    <div className="container mx-auto py-8 space-y-6">
      <div className="flex items-center justify-between">
        <h1 className="text-3xl font-bold">Jobs</h1>
        <Button asChild variant="ghost">
          <Link to="/admin">← Admin</Link>
        </Button>
      </div>

      <div className="flex gap-2 border-b">
        {(['jobs', 'dlq'] as const).map((t) => (
          <button
            key={t}
            className={`px-4 py-2 text-sm font-medium border-b-2 -mb-px ${
              tab === t
                ? 'border-primary text-foreground'
                : 'border-transparent text-muted-foreground hover:text-foreground'
            }`}
            onClick={() => setTab(t)}
          >
            {t === 'jobs' ? 'All jobs' : 'Dead letter queue'}
          </button>
        ))}
      </div>

      {tab === 'jobs' ? <JobsTab /> : <DlqTab />}
    </div>
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
    { header: 'ID', className: 'font-mono text-xs', cell: (j) => `${j.id.slice(0, 8)}…` },
    { header: 'Type', className: 'font-mono', cell: (j) => j.type },
    { header: 'Status', cell: (j) => <StatusBadge status={j.status} /> },
    { header: 'Retries', cell: (j) => `${j.retry_count ?? 0}/${j.max_retries ?? 0}` },
    { header: 'Worker', className: 'font-mono text-xs', cell: (j) => j.worker_id || '—' },
    { header: 'Created', className: 'whitespace-nowrap', cell: (j) => formatEpoch(j.created_at) },
    { header: 'Updated', className: 'whitespace-nowrap', cell: (j) => formatEpoch(j.updated_at) },
  ];

  return (
    <div className="space-y-4">
      <div className="flex items-center gap-2">
        <Input
          placeholder="Filter by type (exact, e.g. account_email)"
          value={typeFilter}
          onChange={(e) => {
            setTypeFilter(e.target.value.trim());
            setPage(1);
          }}
          className="max-w-xs"
        />
        {data && <span className="text-sm text-muted-foreground">{data.total} total</span>}
      </div>

      <Card>
        <CardContent className="overflow-x-auto pt-6">
          <DataTable
            columns={columns}
            rows={data?.data}
            rowKey={(j) => j.id}
            isLoading={isLoading}
            error={error}
            emptyText="No jobs recorded yet."
            isPlaceholder={isPlaceholderData}
            rowProps={(j) => ({
              className: `cursor-pointer hover:bg-accent ${selected?.id === j.id ? 'bg-accent' : ''}`,
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
        </CardContent>
      </Card>

      {selected && <JobDetailCard job={selected} onClose={() => setSelected(null)} />}
    </div>
  );
}

function JobDetailCard({ job, onClose }: { job: Job; onClose: () => void }) {
  return (
    <Card>
      <CardHeader className="flex flex-row items-center justify-between space-y-0">
        <CardTitle className="font-mono text-base">{job.id}</CardTitle>
        <div className="flex gap-2">
          {job.trace_id && (
            <Button asChild size="sm" variant="outline">
              <a
                href={`${TRACE_UI_URL}/trace/${job.trace_id}`}
                target="_blank"
                rel="noopener noreferrer"
              >
                Open trace
              </a>
            </Button>
          )}
          <Button size="sm" variant="ghost" onClick={onClose}>
            Close
          </Button>
        </div>
      </CardHeader>
      <CardContent className="space-y-3 text-sm">
        <JobTimeline job={job} />
        <DetailJson label="Payload" value={job.payload} />
        {job.result != null && <DetailJson label="Result" value={job.result} />}
        {job.error && (
          <div>
            <p className="font-medium mb-1">Last error</p>
            <pre className="rounded bg-destructive/10 text-destructive p-3 text-xs whitespace-pre-wrap">
              {job.error}
            </pre>
          </div>
        )}
      </CardContent>
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
    <ol className="border-l pl-4 space-y-1">
      {events.map((e, i) => (
        <li key={i} className="text-muted-foreground">
          {e}
        </li>
      ))}
    </ol>
  );
}

function DetailJson({ label, value }: { label: string; value: unknown }) {
  return (
    <div>
      <p className="font-medium mb-1">{label}</p>
      <pre className="rounded bg-muted p-3 text-xs overflow-x-auto">
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
    { header: 'ID', className: 'font-mono text-xs', cell: (j) => `${j.id.slice(0, 8)}…` },
    { header: 'Type', className: 'font-mono', cell: (j) => j.type },
    {
      header: 'Error',
      className: 'max-w-md truncate text-destructive',
      cell: (j) => (
        <span title={j.error} className="block max-w-md truncate">
          {j.error || '—'}
        </span>
      ),
    },
    { header: 'Died at', className: 'whitespace-nowrap', cell: (j) => formatEpoch(j.updated_at) },
    {
      header: '',
      className: 'text-right',
      cell: (j) => (
        <Button
          size="sm"
          variant="outline"
          disabled={requeue.isPending}
          onClick={() => requeue.mutate(j.id)}
        >
          <RotateCcw className="h-3.5 w-3.5 mr-1" />
          Requeue
        </Button>
      ),
    },
  ];

  return (
    <div className="space-y-4">
      <Card>
        <CardHeader>
          <CardTitle>{data ? `${data.depth} job(s) in DLQ` : 'Dead letter queue'}</CardTitle>
        </CardHeader>
        <CardContent className="overflow-x-auto">
          <DataTable
            columns={columns}
            rows={data?.data}
            rowKey={(j) => j.id}
            isLoading={isLoading}
            error={loadError}
            emptyText="DLQ is empty — nothing exhausted its retries."
          />
        </CardContent>
      </Card>
    </div>
  );
}
