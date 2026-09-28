import * as React from 'react';

import { cn } from '@/lib/utils';

export interface InputProps extends React.InputHTMLAttributes<HTMLInputElement> {
  /** Renders Bootstrap's invalid state, which the .invalid-feedback text pairs with. */
  invalid?: boolean;
}

export const Input = React.forwardRef<HTMLInputElement, InputProps>(
  ({ className, invalid, ...props }, ref) => (
    <input
      ref={ref}
      className={cn('form-control', invalid && 'is-invalid', className)}
      {...props}
    />
  ),
);
Input.displayName = 'Input';

export const Label = React.forwardRef<
  HTMLLabelElement,
  React.LabelHTMLAttributes<HTMLLabelElement>
>(({ className, ...props }, ref) => (
  <label ref={ref} className={cn('form-label', className)} {...props} />
));
Label.displayName = 'Label';
