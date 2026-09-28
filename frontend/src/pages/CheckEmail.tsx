import { Link, useLocation } from 'react-router-dom';

import { Alert } from '@/components/tabler/Alert';
import { Card, CardBody, CardSubtitle, CardHeader, CardTitle } from '@/components/tabler/Card';

/**
 * Static page shown right after Register. The backend has fired the
 * confirmation email but we don't auto-log-in (flask-base parity).
 */
export function CheckEmailPage() {
  const location = useLocation();
  const email = (location.state as { email?: string } | null)?.email;
  return (
    <div className="container-tight py-4">
      <Card>
        <CardHeader>
          <div>
            <CardTitle>Check your email</CardTitle>
            <CardSubtitle>
              {email ? `We sent a confirmation link to ${email}.` : 'We sent a confirmation link.'}
            </CardSubtitle>
          </div>
        </CardHeader>
        <CardBody>
          <Alert variant="info" className="mb-0">
            Didn&apos;t get one? Check spam, or <Link to="/login">log in</Link> and use &quot;Resend
            confirmation email&quot;.
          </Alert>
        </CardBody>
      </Card>
    </div>
  );
}
