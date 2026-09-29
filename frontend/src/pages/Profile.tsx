import { Link } from 'react-router-dom';

import { Card, CardBody, CardSubtitle, CardHeader, CardTitle } from '@/components/tabler/Card';
import { useMe } from '@/hooks/useMe';

export function ProfilePage() {
  const user = useMe().data ?? null;
  if (!user) return null;
  return (
    <div className="container-xl vstack gap-3">
      <Card>
        <CardHeader>
          <div>
            <CardTitle>Your account</CardTitle>
            <CardSubtitle>{user.email}</CardSubtitle>
          </div>
        </CardHeader>
        <CardBody className="vstack gap-1 small">
          <div>
            <span className="text-secondary">Name: </span>
            {user.full_name || '(not set)'}
          </div>
          <div>
            <span className="text-secondary">Role: </span>
            {user.role?.name ?? user.role_id}
          </div>
          <div>
            <span className="text-secondary">Confirmed: </span>
            {user.confirmed ? 'yes' : 'no'}
          </div>
        </CardBody>
      </Card>
      <div className="row row-cards">
        <div className="col-sm-6">
          <Link to="/account/change-password" className="btn w-100">
            Change password
          </Link>
        </div>
        <div className="col-sm-6">
          <Link to="/account/change-email" className="btn w-100">
            Change email
          </Link>
        </div>
      </div>
    </div>
  );
}
