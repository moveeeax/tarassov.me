import { Link, useParams } from 'react-router-dom';

import { Alert, AlertDescription } from '@/components/ui/alert';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { useApiMutation } from '@/hooks/useApiMutation';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';

/**
 * Hit by the link mailed to the NEW address during an email change
 * (backend: POST /api/account/change-email/{token}). Same pattern as
 * ConfirmEmailPage: the POST is behind an explicit button so email
 * scanners can't burn the one-shot token and StrictMode can't double
 * fire it from an effect.
 *
 * Invalidates qk.me() on success: the account's email just changed, so a
 * signed-in user's cached `email` is stale — refetch /me so the nav and
 * profile reflect the new address.
 */
export function ConfirmChangeEmailPage() {
  const { token = '' } = useParams<{ token: string }>();

  const confirm = useApiMutation(
    () => api.postJson('/api/v1/account/change-email/' + encodeURIComponent(token)),
    { invalidate: [qk.me()] },
  );

  return (
    <div className="container mx-auto max-w-md py-8">
      <Card>
        <CardHeader>
          <CardTitle>Confirm your new email</CardTitle>
          <CardDescription>
            Click the button below to switch your account to this address.
          </CardDescription>
        </CardHeader>
        <CardContent className="space-y-4">
          {confirm.isSuccess && (
            <Alert variant="success">
              <AlertDescription>
                Your email address has been updated. Log in with the new address from now on.
              </AlertDescription>
            </Alert>
          )}
          {confirm.isError && (
            <Alert variant="destructive">
              <AlertDescription>
                {confirm.error ?? 'This link is invalid or has expired.'}
              </AlertDescription>
            </Alert>
          )}
          {confirm.isSuccess ? (
            <Button asChild className="w-full">
              <Link to="/login">Continue to log in</Link>
            </Button>
          ) : (
            <Button
              className="w-full"
              disabled={confirm.isPending}
              onClick={() => confirm.mutate()}
            >
              {confirm.isPending ? 'Confirming…' : 'Confirm new email'}
            </Button>
          )}
        </CardContent>
      </Card>
    </div>
  );
}
