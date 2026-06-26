import { useState, type FormEvent } from 'react';
import { Link } from 'react-router-dom';
import { Trash2, Pencil } from 'lucide-react';

import { ConfirmDialog } from '@/components/ConfirmDialog';
import { DataTable, type Column } from '@/components/DataTable';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import { useAdminRoles } from '@/hooks/useAdminRoles';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useErrorToast } from '@/hooks/useErrorToast';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type { Role } from '@/lib/api/types';
import { PERMISSION_BITS, Permission } from '@/lib/auth/permissions';

const ADMIN_ALL_BITS = Permission.Administer;

interface RoleFormState {
  name: string;
  permissions: number;
  is_default: boolean;
}

export function AdminRolesPage() {
  const [editing, setEditing] = useState<Role | null>(null);
  const [creating, setCreating] = useState(false);
  const [deleting, setDeleting] = useState<Role | null>(null);

  const rolesQ = useAdminRoles();

  const create = useApiMutation(
    (form: RoleFormState) => api.postJson<{ data: Role }>('/api/v1/admin/roles', { body: form }),
    {
      invalidate: [qk.admin.roles()],
      onSuccess: () => setCreating(false),
    },
  );

  const update = useApiMutation(
    (vars: { id: number; form: RoleFormState }) =>
      api.patchJson<{ data: Role }>(`/api/v1/admin/roles/${vars.id}`, { body: vars.form }),
    {
      invalidate: [qk.admin.roles()],
      onSuccess: () => setEditing(null),
    },
  );

  const remove = useApiMutation((id: number) => api.deleteJson(`/api/v1/admin/roles/${id}`), {
    invalidate: [qk.admin.roles()],
    onSuccess: () => setDeleting(null),
  });

  // Surface whichever mutation last failed as a toast (out of flow).
  useErrorToast(create.error ?? update.error ?? remove.error);

  const columns: Column<Role>[] = [
    { header: 'Name', className: 'font-medium', cell: (r) => r.name },
    {
      header: 'Permissions',
      className: 'font-mono',
      cell: (r) =>
        r.permissions === ADMIN_ALL_BITS
          ? `0x${ADMIN_ALL_BITS.toString(16)} (all)`
          : `0x${r.permissions.toString(16)}`,
    },
    {
      header: 'Default?',
      cell: (r) => (
        <span aria-label={r.is_default ? 'Default role' : 'Not default'}>
          <span aria-hidden="true">{r.is_default ? '✓' : '—'}</span>
        </span>
      ),
    },
    {
      header: '',
      className: 'text-right space-x-1',
      cell: (r) => (
        <>
          <Button size="sm" variant="ghost" onClick={() => setEditing(r)}>
            <Pencil className="h-3.5 w-3.5" />
          </Button>
          <Button
            size="sm"
            variant="ghost"
            disabled={r.is_default}
            title={r.is_default ? 'Default role cannot be deleted' : ''}
            onClick={() => setDeleting(r)}
          >
            <Trash2 className="h-3.5 w-3.5 text-destructive" />
          </Button>
        </>
      ),
    },
  ];

  return (
    <div className="container mx-auto max-w-4xl py-8 space-y-6">
      <div className="flex items-center justify-between">
        <div>
          <h1 className="text-3xl font-bold">Roles</h1>
          <p className="text-sm text-muted-foreground">
            Permission bits map to <code>Domain::Permission::k*</code> on the backend.
          </p>
        </div>
        <div className="flex gap-2">
          <Button asChild variant="ghost">
            <Link to="/admin">← Admin</Link>
          </Button>
          <Button onClick={() => setCreating(true)}>New role</Button>
        </div>
      </div>

      <Card>
        <CardHeader>
          <CardTitle>{rolesQ.data ? `${rolesQ.data.data.length} role(s)` : 'Loading…'}</CardTitle>
        </CardHeader>
        <CardContent className="overflow-x-auto">
          <DataTable
            columns={columns}
            rows={rolesQ.data?.data}
            rowKey={(r) => r.id}
            isLoading={rolesQ.isLoading}
            error={rolesQ.error}
            emptyText="No roles defined."
          />
        </CardContent>
      </Card>

      {creating && (
        <RoleFormCard
          key="new"
          title="New role"
          initial={{ name: '', permissions: 0, is_default: false }}
          submitting={create.isPending}
          onSubmit={(form) => create.mutate(form)}
          onCancel={() => setCreating(false)}
        />
      )}
      {editing && (
        // key={editing.id}: remount the form when switching between roles —
        // useState(initial.*) only seeds on mount, so without the key the
        // previous role's edits would bleed into the next one.
        <RoleFormCard
          key={editing.id}
          title={`Edit role: ${editing.name}`}
          initial={{
            name: editing.name,
            permissions: editing.permissions,
            is_default: editing.is_default,
          }}
          submitting={update.isPending}
          onSubmit={(form) => update.mutate({ id: editing.id, form })}
          onCancel={() => setEditing(null)}
        />
      )}
      {deleting && (
        <ConfirmDialog
          title="Delete role"
          description={`Delete role "${deleting.name}"? Users referencing it must be reassigned first.`}
          confirmLabel="Delete role"
          destructive
          busy={remove.isPending}
          onConfirm={() => remove.mutate(deleting.id)}
          onClose={() => setDeleting(null)}
        />
      )}
    </div>
  );
}

interface RoleFormCardProps {
  title: string;
  initial: RoleFormState;
  submitting: boolean;
  onSubmit: (form: RoleFormState) => void;
  onCancel: () => void;
}

function RoleFormCard({ title, initial, submitting, onSubmit, onCancel }: RoleFormCardProps) {
  const [name, setName] = useState(initial.name);
  const [perms, setPerms] = useState(initial.permissions);
  const [isDefault, setIsDefault] = useState(initial.is_default);
  const [adminAll, setAdminAll] = useState(initial.permissions === ADMIN_ALL_BITS);

  const togglePerm = (bit: number) => {
    setPerms((p) => p ^ bit);
    setAdminAll(false);
  };

  const handleSubmit = (e: FormEvent) => {
    e.preventDefault();
    onSubmit({
      name: name.trim(),
      permissions: adminAll ? ADMIN_ALL_BITS : perms,
      is_default: isDefault,
    });
  };

  return (
    <Card>
      <CardHeader>
        <CardTitle>{title}</CardTitle>
      </CardHeader>
      <CardContent>
        <form className="space-y-4" onSubmit={handleSubmit}>
          <div className="space-y-2">
            <Label htmlFor="role-name">Name</Label>
            <Input
              id="role-name"
              value={name}
              onChange={(e) => setName(e.target.value)}
              required
              maxLength={64}
            />
          </div>

          <div className="space-y-2">
            <Label>Permissions</Label>
            <div className="flex items-center gap-2 mb-2">
              <input
                id="role-admin-all"
                type="checkbox"
                checked={adminAll}
                onChange={(e) => setAdminAll(e.target.checked)}
              />
              <label htmlFor="role-admin-all" className="text-sm">
                Administrator (all, 0x{ADMIN_ALL_BITS.toString(16)})
              </label>
            </div>
            {/* Each input below carries `disabled={adminAll}`, which removes
                it from the tab order and blocks interaction — so we only need
                the opacity here as a visual cue, not pointer-events-none
                (which would leave focusable controls silently inert). */}
            <div className={`grid grid-cols-2 gap-2 ${adminAll ? 'opacity-50' : ''}`}>
              {PERMISSION_BITS.map((p) => (
                <label key={p.bit} className="flex items-start gap-2 text-sm">
                  <input
                    type="checkbox"
                    checked={(perms & p.bit) !== 0}
                    onChange={() => togglePerm(p.bit)}
                    disabled={adminAll}
                    className="mt-0.5"
                  />
                  <span>
                    <span className="font-mono text-xs text-muted-foreground">
                      0x{p.bit.toString(16).padStart(2, '0')}
                    </span>{' '}
                    {p.label}
                    {p.hint && (
                      <span className="block text-xs text-muted-foreground">{p.hint}</span>
                    )}
                  </span>
                </label>
              ))}
            </div>
            <p className="text-xs text-muted-foreground font-mono">
              Bitmask: 0x{(adminAll ? ADMIN_ALL_BITS : perms).toString(16).padStart(2, '0')} (
              {adminAll ? ADMIN_ALL_BITS : perms})
            </p>
          </div>

          <div className="flex items-center gap-2">
            <input
              id="role-default"
              type="checkbox"
              checked={isDefault}
              onChange={(e) => setIsDefault(e.target.checked)}
            />
            <label htmlFor="role-default" className="text-sm">
              Default for new sign-ups (only one role can be default)
            </label>
          </div>

          <div className="flex gap-2">
            <Button type="submit" disabled={submitting || name.trim().length === 0}>
              {submitting ? 'Saving…' : 'Save'}
            </Button>
            <Button type="button" variant="ghost" onClick={onCancel}>
              Cancel
            </Button>
          </div>
        </form>
      </CardContent>
    </Card>
  );
}
