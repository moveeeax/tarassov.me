import { Link } from 'react-router-dom';
import { Users, UserPlus, Shield, ListChecks, ScrollText, type LucideIcon } from 'lucide-react';

import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { useMe } from '@/hooks/useMe';
import { Permission, userCan } from '@/lib/auth/permissions';

interface AdminTile {
  to: string;
  title: string;
  description: string;
  icon: LucideIcon;
  /** Optional longer blurb under the description. */
  detail?: string;
  /** Extra permission beyond admin (e.g. AuditRead for audit-only roles). */
  requirePermission?: number;
}

const TILES: AdminTile[] = [
  {
    to: '/admin/users',
    title: 'Users',
    description: 'List, edit, and remove users.',
    icon: Users,
  },
  {
    to: '/admin/invite',
    title: 'Invite',
    description: 'Send an email invite.',
    icon: UserPlus,
  },
  {
    to: '/admin/roles',
    title: 'Roles',
    description: 'Create / edit / delete roles + permissions.',
    icon: Shield,
    detail: 'Each role is a set of permissions; assign them to users.',
  },
  {
    to: '/admin/jobs',
    title: 'Jobs',
    description: 'Queue statuses, payloads, DLQ requeue.',
    icon: ListChecks,
  },
  {
    to: '/admin/audit',
    title: 'Audit log',
    description: 'Read-only trail of admin actions.',
    icon: ScrollText,
    requirePermission: Permission.AuditRead,
  },
];

export function AdminDashboardPage() {
  const me = useMe();

  const tiles = TILES.filter(
    (t) => t.requirePermission === undefined || userCan(me.data, t.requirePermission),
  );

  return (
    <div className="container mx-auto py-8 space-y-6">
      <h1 className="text-3xl font-bold">Admin</h1>
      <div className="grid gap-4 sm:grid-cols-2 lg:grid-cols-3">
        {tiles.map((t) => {
          const Icon = t.icon;
          return (
            <Link key={t.to} to={t.to}>
              <Card className="hover:bg-accent transition-colors h-full">
                <CardHeader>
                  <Icon className="h-6 w-6 mb-2 text-muted-foreground" />
                  <CardTitle>{t.title}</CardTitle>
                  <CardDescription>{t.description}</CardDescription>
                </CardHeader>
                {t.detail && (
                  <CardContent className="text-sm text-muted-foreground">{t.detail}</CardContent>
                )}
              </Card>
            </Link>
          );
        })}
      </div>
    </div>
  );
}
