import { useQuery } from '@tanstack/react-query';
import Chart from 'react-apexcharts';

import { Placeholder } from '@/components/tabler/Placeholder';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type { Job } from '@/lib/api/types';

/** Matches the Job.status enum in docs/openapi.yaml. */
const STATUSES = ['pending', 'processing', 'completed', 'failed', 'dead'] as const;

// Tabler's palette, so the donut matches the rest of the page.
const COLORS = ['#f59f00', '#4299e1', '#2fb344', '#d63939', '#66758c'];

/** GET /api/v1/jobs takes type/limit/offset but no status filter, so the split is
 *  counted client-side over one page — 200 is the endpoint's documented maximum. */
const PAGE = 200;

/** `options` arrives with every catalog default already filled in
 * (see withOptionDefaults in pages/admin/dashboardLayout.ts), so there is no
 * local fallback to drift from the advertised value. */
export function JobsQueue({ options }: { options: Record<string, number> }) {
  const windowDays = options.window_days;

  const jobs = useQuery({
    queryKey: qk.admin.jobs(`widget:${windowDays}`),
    queryFn: () => api.getJson('/api/v1/jobs', { query: { limit: PAGE, offset: 0 } }),
  });
  const dlq = useQuery({
    // The DLQ endpoint takes type/limit only and answers with { data, count, depth };
    // `depth` is the unfiltered queue depth, which is the number worth showing.
    queryKey: qk.admin.jobsDlq(),
    queryFn: () => api.getJson('/api/v1/jobs/dlq', { query: { limit: 1 } }),
  });

  if (jobs.isLoading) return <Placeholder />;
  if (jobs.error) return <p className="text-danger mb-0">Failed to load the queue.</p>;

  // created_at is epoch seconds (see the Job schema), so the window is arithmetic.
  const cutoff = Math.floor(Date.now() / 1000) - windowDays * 86400;
  const rows: Job[] = (jobs.data?.data ?? []).filter((j: Job) => j.created_at >= cutoff);
  const counts = STATUSES.map((s) => rows.filter((j) => j.status === s).length);
  const anything = counts.some((c) => c > 0);

  return (
    <>
      {anything ? (
        <Chart
          type="donut"
          height={180}
          series={counts}
          options={{
            labels: [...STATUSES],
            colors: COLORS,
            legend: { position: 'bottom' },
            dataLabels: { enabled: false },
            tooltip: { theme: 'dark' },
          }}
        />
      ) : (
        <p className="text-secondary">No jobs in the last {windowDays} day(s).</p>
      )}
      <div className="small text-secondary">
        DLQ: {dlq.isLoading ? '…' : (dlq.data?.depth ?? 0)} · window {windowDays} day(s) ·{' '}
        {rows.length} of {jobs.data?.total ?? 0} shown
      </div>
    </>
  );
}
