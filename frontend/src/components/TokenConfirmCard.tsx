import { Link, useParams } from 'react-router-dom';

import { Alert } from '@/components/tabler/Alert';
import { Button } from '@/components/tabler/Button';
import { Card, CardBody, CardSubtitle, CardHeader, CardTitle } from '@/components/tabler/Card';
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
    <div className="container-tight py-4">
      <Card>
        <CardHeader>
          <div>
            <CardTitle>{title}</CardTitle>
            <CardSubtitle>{description}</CardSubtitle>
          </div>
        </CardHeader>
        <CardBody className="vstack gap-3">
          {confirm.isSuccess && <Alert variant="success">{successMessage}</Alert>}
          {confirm.isError && <Alert variant="danger">{confirm.error ?? errorFallback}</Alert>}
          {confirm.isSuccess ? (
            <Link to="/login" className="btn btn-primary w-100">
              Continue to log in
            </Link>
          ) : (
            <Button className="w-100" disabled={confirm.isPending} onClick={() => confirm.mutate()}>
              {confirm.isPending ? 'Confirming…' : buttonLabel}
            </Button>
          )}
        </CardBody>
      </Card>
    </div>
  );
}
