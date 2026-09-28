import * as React from 'react';

import { cn } from '@/lib/utils';

/**
 * Tabler button. No `asChild`: the Radix Slot is gone, so a link that looks
 * like a button is a plain <Link className="btn btn-primary"> at the call site.
 */
export type ButtonVariant = 'primary' | 'secondary' | 'outline' | 'ghost' | 'danger' | 'link';
export type ButtonSize = 'sm' | 'lg' | 'icon';

const VARIANT_CLASS: Record<ButtonVariant, string> = {
  primary: 'btn-primary',
  secondary: 'btn-secondary',
  // Tabler's plain .btn already renders the bordered/outline look.
  outline: '',
  ghost: 'btn-ghost-secondary',
  danger: 'btn-danger',
  link: 'btn-link',
};

const SIZE_CLASS: Record<ButtonSize, string> = {
  sm: 'btn-sm',
  lg: 'btn-lg',
  icon: 'btn-icon',
};

export interface ButtonProps extends React.ButtonHTMLAttributes<HTMLButtonElement> {
  variant?: ButtonVariant;
  size?: ButtonSize;
}

export const Button = React.forwardRef<HTMLButtonElement, ButtonProps>(
  ({ className, variant = 'primary', size, type = 'button', ...props }, ref) => (
    <button
      ref={ref}
      type={type}
      className={cn('btn', VARIANT_CLASS[variant], size && SIZE_CLASS[size], className)}
      {...props}
    />
  ),
);
Button.displayName = 'Button';
