import { useState } from 'react';
import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { Link, useParams } from 'react-router-dom';
import type { z } from 'zod';

import { Button } from '@/components/tabler/Button';
import { FormField } from '@/components/FormField';
import { Card, CardBody, CardHeader, CardTitle } from '@/components/tabler/Card';
import { useToast } from '@/components/tabler/Toaster';
import { api, apiErrorMessage } from '@/lib/api/client';
import { resetPasswordSchema } from '@/lib/schemas/auth';

type FormValues = z.infer<typeof resetPasswordSchema>;

/** Outcome of accepting an invite — drives the success vs. error branch. */
export type JoinResult = { ok: true } | { ok: false; error: string };

/**
 * Submits the new password for an invite token. Pure enough to unit-test
 * (the page just maps the result onto state) — no @testing-library needed.
 * Returns ok:true on 2xx, ok:false with a user-facing message otherwise.
 */
export async function submitJoinFromInvite(
  token: string,
  newPassword: string,
): Promise<JoinResult> {
  const { error } = await api.POST(
    '/api/v1/account/join-from-invite/' + encodeURIComponent(token),
    { body: { new_password: newPassword } },
  );
  if (error) {
    return {
      ok: false,
      error: apiErrorMessage(error, 'This invitation link is invalid or has expired.'),
    };
  }
  return { ok: true };
}

export function JoinFromInvitePage() {
  const { token = '' } = useParams<{ token: string }>();
  const toast = useToast();
  const [done, setDone] = useState(false);
  const {
    register,
    handleSubmit,
    formState: { errors, isSubmitting },
  } = useForm<FormValues>({ resolver: zodResolver(resetPasswordSchema) });

  const onSubmit = handleSubmit(async (values) => {
    const result = await submitJoinFromInvite(token, values.new_password);
    if (!result.ok) {
      toast.error(result.error);
      return;
    }
    setDone(true);
  });

  return (
    <div className="container-tight py-4">
      <Card>
        <CardHeader>
          <CardTitle>Set your password</CardTitle>
        </CardHeader>
        <CardBody>
          {done ? (
            <div className="vstack gap-4">
              <p className="small text-secondary">Account ready. You can log in now.</p>
              <Link to="/login" className="btn btn-primary w-100">
                Continue to log in
              </Link>
            </div>
          ) : (
            <form className="vstack gap-4" onSubmit={onSubmit}>
              <p className="small text-secondary">
                Accept your invitation by choosing a password for your new account.
              </p>
              <FormField
                id="new_password"
                type="password"
                label="Password"
                error={errors.new_password?.message}
                {...register('new_password')}
              />
              <FormField
                id="new_password_confirm"
                type="password"
                label="Confirm password"
                error={errors.new_password_confirm?.message}
                {...register('new_password_confirm')}
              />
              <Button type="submit" className="w-100" disabled={isSubmitting}>
                Set password
              </Button>
            </form>
          )}
        </CardBody>
      </Card>
    </div>
  );
}
