import { Link, useParams } from 'react-router-dom';

import { Alert, AlertDescription } from '@/components/ui/alert';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { useApiMutation } from '@/hooks/useApiMutation';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';

/**
 * Hit by the link in the confirmation email. The POST is behind an
 * explicit button rather than a useEffect: email scanners prefetch
 * links (burning the one-shot token before the user arrives) and
 * React StrictMode double-runs effects (double POST). A click is the
 * only thing that consumes the token.
 *
 * Invalidates qk.me() on success: if the user happens to be signed in
 * (e.g. confirming from the same browser they registered in), their
 * cached `confirmed: false` is stale the moment this succeeds — refetch
 * /me so the guards stop redirecting them to /unconfirmed.
 */
export function ConfirmEmailPage() {
  const { token = '' } = useParams<{ token: string }>();

  const confirm = useApiMutation(
    () => api.postJson('/api/v1/account/confirm/' + encodeURIComponent(token)),
    { invalidate: [qk.me()] },
  );

  return (
    <div className="container mx-auto max-w-md py-8">
      <Card>
        <CardHeader>
          <CardTitle>Confirm your account</CardTitle>
          <CardDescription>
            Click the button below to activate your account.
          </CardDescription>
        </CardHeader>
        <CardContent className="space-y-4">
          {confirm.isSuccess && (
            <Alert variant="success">
              <AlertDescription>
                Your account is confirmed. You can log in now.
              </AlertDescription>
            </Alert>
          )}
          {confirm.isError && (
            <Alert variant="destructive">
              <AlertDescription>
                {confirm.error ?? 'This confirmation link is invalid or has expired.'}
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
              {confirm.isPending ? 'Confirming…' : 'Confirm my account'}
            </Button>
          )}
        </CardContent>
      </Card>
    </div>
  );
}
