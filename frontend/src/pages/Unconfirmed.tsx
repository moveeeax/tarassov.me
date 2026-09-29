import { useState } from 'react';

import { Alert } from '@/components/tabler/Alert';
import { Button } from '@/components/tabler/Button';
import { Card, CardBody, CardSubtitle, CardHeader, CardTitle } from '@/components/tabler/Card';
import { useMe } from '@/hooks/useMe';
import { api } from '@/lib/api/client';

/**
 * Shown when the user is logged in but the access JWT carries
 * confirmed=false. flask-base parity: app/account/views.py
 * before_request blocks unconfirmed users from non-account routes
 * and redirects them to /unconfirmed.
 */
export function UnconfirmedPage() {
  const user = useMe().data ?? null;
  const [resent, setResent] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const resend = async () => {
    setError(null);
    const { error: e } = await api.POST('/api/v1/account/confirm-resend');
    if (e) {
      setError('Could not resend the confirmation email. Try again later.');
      return;
    }
    setResent(true);
  };

  return (
    <div className="container-tight py-4">
      <Card>
        <CardHeader>
          <div>
            <CardTitle>Confirm your email</CardTitle>
            <CardSubtitle>
              {' '}
              We sent a confirmation link to {user?.email ?? 'your email address'}. Click it to
              unlock the rest of the app.{' '}
            </CardSubtitle>
          </div>
        </CardHeader>
        <CardBody className="vstack gap-4">
          {resent && <Alert variant="success">A new confirmation link is on its way.</Alert>}
          {error && <Alert variant="danger">{error}</Alert>}
          <Button onClick={resend} className="w-100" variant="outline">
            Resend confirmation email
          </Button>
        </CardBody>
      </Card>
    </div>
  );
}
