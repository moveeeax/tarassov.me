import type { HTMLAttributes } from 'react';

import { cn } from '@/lib/utils';

/** A pulsing placeholder block for loading states. */
export function Skeleton({ className, ...props }: HTMLAttributes<HTMLDivElement>) {
  return <div className={cn('animate-pulse rounded bg-muted', className)} {...props} />;
}
