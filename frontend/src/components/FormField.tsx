import { forwardRef, type InputHTMLAttributes } from 'react';

import { Input, Label } from '@/components/tabler/Input';

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
    <div className="mb-3">
      <Label htmlFor={id}>{label}</Label>
      <Input
        id={id}
        ref={ref}
        invalid={!!error}
        aria-invalid={!!error}
        aria-describedby={error ? `${id}-error` : undefined}
        {...inputProps}
      />
      {error && (
        // d-block because Bootstrap only reveals .invalid-feedback next to a
        // .is-invalid sibling in a form-validated <form>; we drive it ourselves.
        <div id={`${id}-error`} role="alert" className="invalid-feedback d-block">
          {error}
        </div>
      )}
    </div>
  ),
);
FormField.displayName = 'FormField';
