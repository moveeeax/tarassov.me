import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import type { z } from 'zod';

import { Alert } from '@/components/tabler/Alert';
import { Button } from '@/components/tabler/Button';
import { FormField } from '@/components/FormField';
import { Card, CardBody, CardSubtitle, CardHeader, CardTitle } from '@/components/tabler/Card';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useErrorToast } from '@/hooks/useErrorToast';
import { api } from '@/lib/api/client';
import { changeEmailSchema } from '@/lib/schemas/auth';

type FormValues = z.infer<typeof changeEmailSchema>;

export function ChangeEmailPage() {
  const {
    register,
    handleSubmit,
    formState: { errors, isSubmitting },
  } = useForm<FormValues>({ resolver: zodResolver(changeEmailSchema) });

  const change = useApiMutation((values: FormValues) =>
    api.postJson('/api/v1/account/change-email-request', { body: values }),
  );
  useErrorToast(change.error);

  const onSubmit = handleSubmit((values) => change.mutate(values));

  return (
    <div className="container-tight py-4">
      <Card>
        <CardHeader>
          <div>
            <CardTitle>Change your email</CardTitle>
            <CardSubtitle>
              {' '}
              We'll send a confirmation link to the new address. Your current email stays active
              until you click it.{' '}
            </CardSubtitle>
          </div>
        </CardHeader>
        <CardBody>
          {change.isSuccess ? (
            <Alert variant="success">
              Confirmation email queued. Check the new address for a link.
            </Alert>
          ) : (
            <form className="vstack gap-4" onSubmit={onSubmit}>
              <FormField
                id="new_email"
                type="email"
                label="New email"
                error={errors.new_email?.message}
                {...register('new_email')}
              />
              <FormField
                id="password"
                type="password"
                label="Confirm with current password"
                error={errors.password?.message}
                {...register('password')}
              />
              <Button type="submit" className="w-100" disabled={isSubmitting || change.isPending}>
                Send confirmation link
              </Button>
            </form>
          )}
        </CardBody>
      </Card>
    </div>
  );
}
