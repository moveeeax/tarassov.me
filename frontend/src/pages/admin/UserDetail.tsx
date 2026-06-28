import { Link, useNavigate, useParams } from 'react-router-dom';
import { useQuery } from '@tanstack/react-query';

import { useState } from 'react';

import { ConfirmDialog } from '@/components/ConfirmDialog';
import { FormField } from '@/components/FormField';
import { RoleSelect } from '@/components/RoleSelect';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { Label } from '@/components/ui/label';
import { useToast } from '@/components/ui/toaster';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useErrorToast } from '@/hooks/useErrorToast';
import { useMe } from '@/hooks/useMe';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type { UserDetailResponse } from '@/lib/api/types';

export function AdminUserDetailPage() {
  const { id = '' } = useParams<{ id: string }>();
  const navigate = useNavigate();
  const toast = useToast();
  const [confirmDelete, setConfirmDelete] = useState(false);
  // Query-backed via the TanStack Query cache: the cache is empty for one
  // paint after a hard reload, which would briefly disable the
  // self-protection UI.
  const me = useMe().data ?? null;

  const userQ = useQuery({
    queryKey: qk.admin.user(id),
    queryFn: () => api.getJson<UserDetailResponse>('/api/v1/admin/users/' + id),
  });

  const update = useApiMutation(
    (patch: Record<string, unknown>) =>
      api.patchJson<UserDetailResponse>('/api/v1/admin/users/' + id, { body: patch }),
    {
      invalidate: [qk.admin.user(id), qk.admin.users()],
      onSuccess: () => toast.success('Changes saved.'),
    },
  );

  const remove = useApiMutation(() => api.deleteJson('/api/v1/admin/users/' + id), {
    invalidate: [qk.admin.users()],
    onSuccess: () => navigate('/admin/users'),
  });

  useErrorToast(update.error ?? remove.error);

  if (userQ.isLoading) return <p className="container py-8">Loading…</p>;
  if (userQ.error || !userQ.data)
    return <p className="container py-8 text-destructive">User not found.</p>;

  const user = userQ.data.data;
  const isSelf = me?.id === user.id;

  return (
    <div className="container mx-auto max-w-2xl py-8 space-y-6">
      <div className="flex items-center justify-between">
        <h1 className="text-2xl font-bold">{user.email}</h1>
        <Button variant="ghost" asChild>
          <Link to="/admin/users">← Back</Link>
        </Button>
      </div>
      <Card>
        <CardHeader>
          <CardTitle>Details</CardTitle>
        </CardHeader>
        <CardContent className="space-y-4">
          <form
            onSubmit={(e) => {
              e.preventDefault();
              const fd = new FormData(e.currentTarget);
              const patch: Record<string, unknown> = {};
              const newEmail = String(fd.get('email') || '');
              const newRoleId = Number(fd.get('role_id'));
              const newFirst = String(fd.get('first_name') || '');
              const newLast = String(fd.get('last_name') || '');
              if (newEmail && newEmail !== user.email) patch.email = newEmail;
              if (newRoleId && newRoleId !== user.role_id) patch.role_id = newRoleId;
              if (newFirst !== (user.first_name ?? '')) patch.first_name = newFirst;
              if (newLast !== (user.last_name ?? '')) patch.last_name = newLast;
              if (Object.keys(patch).length === 0) return;
              update.mutate(patch);
            }}
            className="space-y-3"
          >
            <FormField id="email" name="email" label="Email" defaultValue={user.email} />
            <div className="grid grid-cols-1 sm:grid-cols-2 gap-3">
              <FormField
                id="first_name"
                name="first_name"
                label="First name"
                defaultValue={user.first_name ?? ''}
              />
              <FormField
                id="last_name"
                name="last_name"
                label="Last name"
                defaultValue={user.last_name ?? ''}
              />
            </div>
            <div className="space-y-2">
              <Label htmlFor="role_id">Role</Label>
              <RoleSelect
                id="role_id"
                name="role_id"
                defaultValue={user.role_id}
                disabled={isSelf}
              />
              {isSelf && (
                <p className="text-xs text-muted-foreground">
                  You cannot change the role of your own account.
                </p>
              )}
            </div>
            <Button type="submit" disabled={update.isPending}>
              {update.isPending ? 'Saving…' : 'Save changes'}
            </Button>
          </form>
        </CardContent>
      </Card>
      <Card>
        <CardHeader>
          <CardTitle className="text-destructive">Danger zone</CardTitle>
        </CardHeader>
        <CardContent>
          <Button variant="destructive" disabled={isSelf} onClick={() => setConfirmDelete(true)}>
            Delete user
          </Button>
          {isSelf && (
            <p className="text-xs text-muted-foreground mt-2">
              You cannot delete your own account; ask another admin.
            </p>
          )}
        </CardContent>
      </Card>
      {confirmDelete && (
        <ConfirmDialog
          title="Delete user"
          description={`Delete user ${user.email}? This cannot be undone.`}
          confirmLabel="Delete user"
          destructive
          busy={remove.isPending}
          onConfirm={() => remove.mutate()}
          onClose={() => setConfirmDelete(false)}
        />
      )}
    </div>
  );
}
