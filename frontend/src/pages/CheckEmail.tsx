import { Link, useLocation } from 'react-router-dom';

import { Alert, AlertDescription } from '@/components/ui/alert';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';

/**
 * Static page shown right after Register. The backend has fired the
 * confirmation email but we don't auto-log-in (flask-base parity).
 */
export function CheckEmailPage() {
  const location = useLocation();
  const email = (location.state as { email?: string } | null)?.email;
  return (
    <div className="container mx-auto max-w-md py-8">
      <Card>
        <CardHeader>
          <CardTitle>Check your email</CardTitle>
          <CardDescription>
            {email ? `We sent a confirmation link to ${email}.` : 'We sent a confirmation link.'}
          </CardDescription>
        </CardHeader>
        <CardContent>
          <Alert>
            <AlertDescription>
              Didn't get one? Check spam, or{' '}
              <Link to="/login" className="underline">
                log in
              </Link>{' '}
              and use "Resend confirmation email".
            </AlertDescription>
          </Alert>
        </CardContent>
      </Card>
    </div>
  );
}
