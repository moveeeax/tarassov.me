import { Link } from 'react-router-dom';

import { Button } from '@/components/ui/button';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { useMe } from '@/hooks/useMe';

export function HomePage() {
  const user = useMe().data ?? null;
  return (
    <div className="container mx-auto py-8 max-w-3xl space-y-6">
      <Card>
        <CardHeader>
          <CardTitle>{user ? `Welcome back, ${user.full_name || user.email}` : 'Welcome'}</CardTitle>
          <CardDescription>
            {user
              ? 'You are logged in.'
              : 'Log in or register to access the rest of the app.'}
          </CardDescription>
        </CardHeader>
        <CardContent className="flex gap-2">
          {user ? (
            <Button asChild>
              <Link to="/account">My account</Link>
            </Button>
          ) : (
            <>
              <Button asChild>
                <Link to="/login">Log in</Link>
              </Button>
              <Button asChild variant="outline">
                <Link to="/register">Register</Link>
              </Button>
            </>
          )}
        </CardContent>
      </Card>
    </div>
  );
}
