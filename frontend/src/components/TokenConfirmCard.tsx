import { Link, useParams } from 'react-router-dom';

import { Alert, AlertDescription } from '@/components/ui/alert';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { useApiMutation } from '@/hooks/useApiMutation';
import { qk } from '@/lib/api/queryKeys';

interface TokenConfirmCardProps {
  title: string;
  description: string;
  successMessage: string;
  errorFallback: string;
  buttonLabel: string;
  /** POST the one-shot token. Invalidates qk.me() on success. */
  mutate: (token: string) => Promise<unknown>;
}

/**
 * Shared UI for email-token confirmation pages (account confirm, change-email).
 * The POST is behind an explicit button so email scanners can't burn the
 * one-shot token and StrictMode can't double-fire it from an effect.
 */
export function TokenConfirmCard({
  title,
  description,
  successMessage,
  errorFallback,
  buttonLabel,
  mutate,
}: TokenConfirmCardProps) {
  const { token = '' } = useParams<{ token: string }>();
  const confirm = useApiMutation(() => mutate(token), { invalidate: [qk.me()] });

  return (
    <div className="container mx-auto max-w-md py-8">
      <Card>
        <CardHeader>
          <CardTitle>{title}</CardTitle>
          <CardDescription>{description}</CardDescription>
        </CardHeader>
        <CardContent className="space-y-4">
          {confirm.isSuccess && (
            <Alert variant="success">
              <AlertDescription>{successMessage}</AlertDescription>
            </Alert>
          )}
          {confirm.isError && (
            <Alert variant="destructive">
              <AlertDescription>{confirm.error ?? errorFallback}</AlertDescription>
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
              {confirm.isPending ? 'Confirming…' : buttonLabel}
            </Button>
          )}
        </CardContent>
      </Card>
    </div>
  );
}
