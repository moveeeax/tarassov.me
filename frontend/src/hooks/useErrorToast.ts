import { useEffect } from 'react';

import { useToast } from '@/components/ui/toaster';

/**
 * Surface a useApiMutation `error` string as a toast. Server feedback is
 * out-of-flow (fixed-position toast), so it never shoves the form around.
 * Fires whenever the error message changes to a non-empty value.
 */
export function useErrorToast(error: string | null | undefined) {
  const toast = useToast();
  useEffect(() => {
    if (error) toast.error(error);
  }, [error, toast]);
}
