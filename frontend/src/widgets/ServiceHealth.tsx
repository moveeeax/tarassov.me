import { useQuery } from '@tanstack/react-query';

import { Placeholder } from '@/components/tabler/Placeholder';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';

export function ServiceHealth() {
  const query = useQuery({
    queryKey: qk.serviceHealth(),
    // /api/v1/health, not /health: nginx proxies only `location /api/`, so the
    // bare path is answered by the SPA fallback with index.html and every fetch
    // fails. The probe answers 503 when a critical component is down, so a
    // rejected request is itself an answer we render.
    queryFn: () => api.getJson('/api/v1/health'),
    retry: false,
  });

  if (query.isLoading) return <Placeholder />;

  const ok = !query.error && query.data?.status === 'ok';

  return (
    <div className="vstack gap-2">
      <div>
        <span className={ok ? 'badge bg-green' : 'badge bg-red'}>{ok ? 'ok' : 'degraded'}</span>
      </div>
      <div className="small text-secondary">
        version <span className="font-monospace">{query.data?.version ?? 'unknown'}</span>
      </div>
      {query.data?.components && (
        <div className="small text-secondary">
          {Object.keys(query.data.components).length} component(s) reporting
        </div>
      )}
    </div>
  );
}
