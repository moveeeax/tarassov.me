import { Link } from 'react-router-dom';

import { Card, CardBody, CardHeader, CardTitle } from '@/components/tabler/Card';
import { useMe } from '@/hooks/useMe';

export function HomePage() {
  const user = useMe().data ?? null;
  return (
    <div className="container-xl">
      <Card>
        <CardHeader>
          <div>
            <CardTitle>
              {user ? `Welcome back, ${user.full_name || user.email}` : 'Welcome'}
            </CardTitle>
            <div className="text-secondary">
              {user ? 'You are logged in.' : 'Log in or register to access the rest of the app.'}
            </div>
          </div>
        </CardHeader>
        <CardBody className="btn-list">
          {user ? (
            <Link to="/account" className="btn btn-primary">
              My account
            </Link>
          ) : (
            <>
              <Link to="/login" className="btn btn-primary">
                Log in
              </Link>
              <Link to="/register" className="btn">
                Register
              </Link>
            </>
          )}
        </CardBody>
      </Card>
    </div>
  );
}
