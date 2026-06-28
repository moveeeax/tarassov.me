import { useState } from 'react';
import { keepPreviousData, useQuery } from '@tanstack/react-query';

/**
 * Offset-paginated list query. Owns the page state, computes the
 * offset, keeps the previous page rendered while the next one loads
 * (placeholderData: keepPreviousData) and derives totalPages from the
 * backend's { total } field.
 *
 * The page number is appended to the query key, so ['admin','users']
 * becomes ['admin','users', 1] — invalidating the prefix still hits
 * every page.
 */
export function usePagedQuery<T extends { total: number }>(options: {
  /** Key WITHOUT the page number — it is appended automatically. */
  queryKey: readonly unknown[];
  queryFn: (params: { limit: number; offset: number }) => Promise<T>;
  perPage?: number;
  refetchInterval?: number;
}) {
  const { queryKey, queryFn, perPage = 20, refetchInterval } = options;
  const [page, setPage] = useState(1);
  const offset = (page - 1) * perPage;

  const query = useQuery({
    queryKey: [...queryKey, page],
    queryFn: () => queryFn({ limit: perPage, offset }),
    placeholderData: keepPreviousData,
    refetchInterval,
  });

  const totalPages = query.data ? Math.max(1, Math.ceil(query.data.total / perPage)) : 1;

  return { ...query, page, setPage, offset, perPage, totalPages };
}
