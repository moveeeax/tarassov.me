import { Link, useNavigate, useParams } from 'react-router-dom';
import { useQuery } from '@tanstack/react-query';

import { useState } from 'react';

import { ConfirmDialog } from '@/components/ConfirmDialog';
import { FormField } from '@/components/FormField';
import { RoleSelect } from '@/components/RoleSelect';
import { Button } from '@/components/tabler/Button';
import { Card, CardBody, CardHeader, CardTitle } from '@/components/tabler/Card';
import { Label } from '@/components/tabler/Input';
import { PageHeader } from '@/components/tabler/PageHeader';
import { Placeholder } from '@/components/tabler/Placeholder';
import { useToast } from '@/components/tabler/Toaster';
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

  if (userQ.isLoading)
    return (
      <div className="container-xl">
        <Placeholder />
      </div>
    );
  if (userQ.error || !userQ.data)
    return <p className="container-xl text-danger">User not found.</p>;

  const user = userQ.data.data;
  const isSelf = me?.id === user.id;

  return (
    <>
      <PageHeader
        title={user.email}
        pretitle="User"
        actions={
          <Link to="/admin/users" className="btn btn-ghost-secondary">
            ← Back
          </Link>
        }
      />
      <div className="container-xl vstack gap-3">
        <Card>
          <CardHeader>
            <CardTitle>Details</CardTitle>
          </CardHeader>
          <CardBody>
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
              className="vstack gap-3"
            >
              <FormField id="email" name="email" label="Email" defaultValue={user.email} />
              <div className="row">
                <div className="col-sm-6">
                  <FormField
                    id="first_name"
                    name="first_name"
                    label="First name"
                    defaultValue={user.first_name ?? ''}
                  />
                </div>
                <div className="col-sm-6">
                  <FormField
                    id="last_name"
                    name="last_name"
                    label="Last name"
                    defaultValue={user.last_name ?? ''}
                  />
                </div>
              </div>
              <div>
                <Label htmlFor="role_id">Role</Label>
                <RoleSelect
                  id="role_id"
                  name="role_id"
                  defaultValue={user.role_id}
                  disabled={isSelf}
                />
                {isSelf && (
                  <div className="form-hint">You cannot change the role of your own account.</div>
                )}
              </div>
              <div>
                <Button type="submit" disabled={update.isPending}>
                  {update.isPending ? 'Saving…' : 'Save changes'}
                </Button>
              </div>
            </form>
          </CardBody>
        </Card>
        <Card>
          <CardHeader>
            <CardTitle className="text-danger">Danger zone</CardTitle>
          </CardHeader>
          <CardBody>
            <Button variant="danger" disabled={isSelf} onClick={() => setConfirmDelete(true)}>
              Delete user
            </Button>
            {isSelf && (
              <div className="form-hint mt-2">
                You cannot delete your own account; ask another admin.
              </div>
            )}
          </CardBody>
        </Card>
      </div>
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
    </>
  );
}
