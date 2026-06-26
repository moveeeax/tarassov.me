import { forwardRef, type InputHTMLAttributes } from 'react';

import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';

interface FormFieldProps extends InputHTMLAttributes<HTMLInputElement> {
  id: string;
  label: string;
  /** Validation message (usually `errors.<field>?.message` from RHF). */
  error?: string;
}

/**
 * Label + Input + error message — the triple every form page was repeating
 * three to four times. Forwards the ref so react-hook-form's `register()`
 * spread works directly:
 *
 *   <FormField id="email" type="email" label="Email"
 *              error={errors.email?.message} {...register('email')} />
 */
export const FormField = forwardRef<HTMLInputElement, FormFieldProps>(
  ({ id, label, error, ...inputProps }, ref) => (
    <div className="space-y-2">
      <Label htmlFor={id}>{label}</Label>
      <Input
        id={id}
        ref={ref}
        aria-invalid={!!error}
        aria-describedby={error ? `${id}-error` : undefined}
        {...inputProps}
      />
      {error && (
        <p id={`${id}-error`} role="alert" className="text-sm text-destructive">
          {error}
        </p>
      )}
    </div>
  ),
);
FormField.displayName = 'FormField';
