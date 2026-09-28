import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { Link } from 'react-router-dom';
import type { z } from 'zod';

import { Alert } from '@/components/tabler/Alert';
import { Button } from '@/components/tabler/Button';
import { FormField } from '@/components/FormField';
import { Card, CardBody, CardSubtitle, CardHeader, CardTitle } from '@/components/tabler/Card';
import { useApiMutation } from '@/hooks/useApiMutation';
import { api } from '@/lib/api/client';
import { requestResetSchema } from '@/lib/schemas/auth';

type FormValues = z.infer<typeof requestResetSchema>;

export function RequestResetPage() {
  const {
    register,
    handleSubmit,
    formState: { errors, isSubmitting },
  } = useForm<FormValues>({ resolver: zodResolver(requestResetSchema) });

  // The backend always returns 200 (no enumeration). We surface a generic
  // confirmation once the request has settled — success or failure — so the
  // UI never reveals whether the address is registered.
  const request = useApiMutation((values: FormValues) =>
    api.postJson('/api/v1/account/reset-password-request', { body: values }),
  );
  const sent = request.isSuccess || request.isError;

  const onSubmit = handleSubmit((values) => request.mutate(values));

  return (
    <div className="container-tight py-4">
      <Card>
        <CardHeader>
          <div>
            <CardTitle>Reset your password</CardTitle>
            <CardSubtitle>We'll email a reset link if the address is registered.</CardSubtitle>
          </div>
        </CardHeader>
        <CardBody>
          {sent ? (
            <Alert variant="info">If that email is registered, a reset link is on its way.</Alert>
          ) : (
            <form className="vstack gap-4" onSubmit={onSubmit}>
              <FormField
                id="email"
                type="email"
                label="Email"
                error={errors.email?.message}
                {...register('email')}
              />
              <Button type="submit" className="w-100" disabled={isSubmitting || request.isPending}>
                Send reset link
              </Button>
              <div className="small text-secondary text-center">
                <Link to="/login">Back to log in</Link>
              </div>
            </form>
          )}
        </CardBody>
      </Card>
    </div>
  );
}
