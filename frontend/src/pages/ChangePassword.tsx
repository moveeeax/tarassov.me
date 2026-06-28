import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import type { z } from 'zod';

import { Button } from '@/components/ui/button';
import { FormField } from '@/components/FormField';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { useToast } from '@/components/ui/toaster';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useErrorToast } from '@/hooks/useErrorToast';
import { api } from '@/lib/api/client';
import { changePasswordSchema } from '@/lib/schemas/auth';

type FormValues = z.infer<typeof changePasswordSchema>;

export function ChangePasswordPage() {
  const toast = useToast();
  const {
    register,
    handleSubmit,
    reset,
    formState: { errors, isSubmitting },
  } = useForm<FormValues>({ resolver: zodResolver(changePasswordSchema) });

  const change = useApiMutation(
    (values: FormValues) =>
      api.postJson('/api/v1/account/change-password', {
        body: {
          old_password: values.old_password,
          new_password: values.new_password,
        },
      }),
    {
      onSuccess: () => {
        reset();
        toast.success('Password updated.');
      },
    },
  );
  useErrorToast(change.error);

  const onSubmit = handleSubmit((values) => change.mutate(values));

  return (
    <div className="container mx-auto max-w-md py-8">
      <Card>
        <CardHeader>
          <CardTitle>Change password</CardTitle>
        </CardHeader>
        <CardContent>
          <form className="space-y-4" onSubmit={onSubmit}>
            <FormField
              id="old_password"
              type="password"
              label="Current password"
              error={errors.old_password?.message}
              {...register('old_password')}
            />
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
            <Button type="submit" className="w-full" disabled={isSubmitting || change.isPending}>
              Update password
            </Button>
          </form>
        </CardContent>
      </Card>
    </div>
  );
}
