import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { Link, useParams } from 'react-router-dom';
import type { z } from 'zod';

import { Button } from '@/components/tabler/Button';
import { FormField } from '@/components/FormField';
import { Card, CardBody, CardHeader, CardTitle } from '@/components/tabler/Card';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useErrorToast } from '@/hooks/useErrorToast';
import { api } from '@/lib/api/client';
import { resetPasswordSchema } from '@/lib/schemas/auth';

type FormValues = z.infer<typeof resetPasswordSchema>;

export function ResetPasswordPage() {
  const { token = '' } = useParams<{ token: string }>();
  const {
    register,
    handleSubmit,
    formState: { errors, isSubmitting },
  } = useForm<FormValues>({ resolver: zodResolver(resetPasswordSchema) });

  const reset = useApiMutation((values: FormValues) =>
    api.postJson('/api/v1/account/reset-password/' + encodeURIComponent(token), {
      body: { new_password: values.new_password },
    }),
  );
  useErrorToast(reset.error);

  const onSubmit = handleSubmit((values) => reset.mutate(values));

  return (
    <div className="container-tight py-4">
      <Card>
        <CardHeader>
          <CardTitle>Set a new password</CardTitle>
        </CardHeader>
        <CardBody>
          {reset.isSuccess ? (
            <div className="vstack gap-4">
              <p className="small text-secondary">Password updated. You can log in now.</p>
              <Link to="/login" className="btn btn-primary w-100">
                Continue to log in
              </Link>
            </div>
          ) : (
            <form className="vstack gap-4" onSubmit={onSubmit}>
              <FormField
                id="new_password"
                type="password"
                label="New password"
                error={errors.new_password?.message}
                {...register('new_password')}
              />
              <FormField
                id="new_password_confirm"
                type="password"
                label="Confirm new password"
                error={errors.new_password_confirm?.message}
                {...register('new_password_confirm')}
              />
              <Button type="submit" className="w-100" disabled={isSubmitting || reset.isPending}>
                Update password
              </Button>
            </form>
          )}
        </CardBody>
      </Card>
    </div>
  );
}
