import { Link } from 'react-router-dom';
import { Users, UserPlus, Shield, ListChecks, ScrollText } from 'lucide-react';

import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { useMe } from '@/hooks/useMe';
import { Permission, userCan } from '@/lib/auth/permissions';

export function AdminDashboardPage() {
  const me = useMe();
  const canAudit = userCan(me.data, Permission.AuditRead);
  return (
    <div className="container mx-auto py-8 space-y-6">
      <h1 className="text-3xl font-bold">Admin</h1>
      <div className="grid gap-4 sm:grid-cols-2 lg:grid-cols-3">
        <Link to="/admin/users">
          <Card className="hover:bg-accent transition-colors h-full">
            <CardHeader>
              <Users className="h-6 w-6 mb-2 text-muted-foreground" />
              <CardTitle>Users</CardTitle>
              <CardDescription>List, edit, and remove users.</CardDescription>
            </CardHeader>
          </Card>
        </Link>
        <Link to="/admin/invite">
          <Card className="hover:bg-accent transition-colors h-full">
            <CardHeader>
              <UserPlus className="h-6 w-6 mb-2 text-muted-foreground" />
              <CardTitle>Invite</CardTitle>
              <CardDescription>Send an email invite.</CardDescription>
            </CardHeader>
          </Card>
        </Link>
        <Link to="/admin/roles">
          <Card className="hover:bg-accent transition-colors h-full">
            <CardHeader>
              <Shield className="h-6 w-6 mb-2 text-muted-foreground" />
              <CardTitle>Roles</CardTitle>
              <CardDescription>Create / edit / delete roles + permissions.</CardDescription>
            </CardHeader>
            <CardContent className="text-sm text-muted-foreground">
              Each role is a set of permissions; assign them to users.
            </CardContent>
          </Card>
        </Link>
        <Link to="/admin/jobs">
          <Card className="hover:bg-accent transition-colors h-full">
            <CardHeader>
              <ListChecks className="h-6 w-6 mb-2 text-muted-foreground" />
              <CardTitle>Jobs</CardTitle>
              <CardDescription>Queue statuses, payloads, DLQ requeue.</CardDescription>
            </CardHeader>
          </Card>
        </Link>
        {canAudit && (
          <Link to="/admin/audit">
            <Card className="hover:bg-accent transition-colors h-full">
              <CardHeader>
                <ScrollText className="h-6 w-6 mb-2 text-muted-foreground" />
                <CardTitle>Audit log</CardTitle>
                <CardDescription>Read-only trail of admin actions.</CardDescription>
              </CardHeader>
            </Card>
          </Link>
        )}
      </div>
    </div>
  );
}
