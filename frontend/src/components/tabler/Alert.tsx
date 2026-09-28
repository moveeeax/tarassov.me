import * as React from 'react';

import { cn } from '@/lib/utils';

export type AlertVariant = 'danger' | 'info' | 'success' | 'warning';

export interface AlertProps extends React.HTMLAttributes<HTMLDivElement> {
  variant?: AlertVariant;
}

export const Alert = React.forwardRef<HTMLDivElement, AlertProps>(
  ({ className, variant = 'info', role = 'alert', ...props }, ref) => (
    <div ref={ref} role={role} className={cn('alert', `alert-${variant}`, className)} {...props} />
  ),
);
Alert.displayName = 'Alert';
