import { useState, useCallback } from 'react';
import { useMutation, useQueryClient, type UseMutationOptions } from '@tanstack/react-query';

import { apiErrorMessage } from '@/lib/api/client';

/**
 * useMutation wrapper that owns the two things every admin mutation was
 * hand-rolling: an `error` string derived from apiErrorMessage, and a
 * list of query keys to invalidate on success.
 *
 * - `invalidate`: query-key prefixes invalidated after a successful
 *   mutation (each via invalidateQueries — prefix match, so paged keys
 *   are covered).
 * - `error`: the latest error message (or null), reset on each mutate.
 * - `clearError`: drop the banner manually (e.g. when opening a form).
 *
 * There is no toast library in this project — surface `error` in an
 * <Alert variant="destructive">.
 */
export function useApiMutation<TData, TVariables = void>(
  mutationFn: (vars: TVariables) => Promise<TData>,
  options: {
    invalidate?: readonly (readonly unknown[])[];
    onSuccess?: (data: TData, vars: TVariables) => void;
    onError?: (message: string, error: unknown) => void;
  } & Omit<
    UseMutationOptions<TData, unknown, TVariables>,
    'mutationFn' | 'onMutate' | 'onSuccess' | 'onError'
  > = {},
) {
  const { invalidate, onSuccess, onError, ...rest } = options;
  const qc = useQueryClient();
  const [error, setError] = useState<string | null>(null);

  const clearError = useCallback(() => setError(null), []);

  const mutation = useMutation<TData, unknown, TVariables>({
    ...rest,
    mutationFn,
    onMutate: () => {
      setError(null);
    },
    onSuccess: (data, vars) => {
      invalidate?.forEach((queryKey) => qc.invalidateQueries({ queryKey }));
      onSuccess?.(data, vars);
    },
    onError: (e) => {
      const message = apiErrorMessage(e);
      setError(message);
      onError?.(message, e);
    },
  });

  return { ...mutation, error, clearError };
}
