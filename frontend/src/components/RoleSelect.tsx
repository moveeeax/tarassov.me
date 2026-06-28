import { forwardRef, type SelectHTMLAttributes } from 'react';

import { useAdminRoles } from '@/hooks/useAdminRoles';

/**
 * Roles dropdown shared by InviteUser and UserDetail. Pulls the role
 * list from the shared useAdminRoles cache so both pages stay in sync.
 *
 * forwardRef + spread props so it slots into react-hook-form
 * (`{...register('role_id')}`) as well as uncontrolled defaultValue use.
 * Pass `includeDefaultOption` to prepend a "(default)" entry for forms
 * where role is optional.
 */
interface RoleSelectProps extends SelectHTMLAttributes<HTMLSelectElement> {
  includeDefaultOption?: boolean;
}

export const RoleSelect = forwardRef<HTMLSelectElement, RoleSelectProps>(function RoleSelect(
  { includeDefaultOption = false, className, ...props },
  ref,
) {
  const rolesQ = useAdminRoles();
  const roles = rolesQ.data?.data ?? [];
  const selectClass = `flex h-10 w-full rounded-md border border-input bg-background px-3 py-2 text-sm ${className ?? ''}`;

  // The <select> is UNCONTROLLED (defaultValue / react-hook-form register) and
  // its <option>s load asynchronously from useAdminRoles. If it mounts before
  // the options exist, an uncontrolled defaultValue has no matching <option> and
  // the browser falls back to the FIRST role — which made the user-edit page
  // always show "User" and could silently downgrade a role on save. Mount the
  // real select only once the options are present so defaultValue resolves.
  if (roles.length === 0) {
    return (
      <select disabled aria-busy={rolesQ.isLoading} className={selectClass}>
        <option>{rolesQ.isLoading ? 'Loading…' : 'No roles'}</option>
      </select>
    );
  }

  return (
    <select ref={ref} className={selectClass} {...props}>
      {includeDefaultOption && <option value="">(default)</option>}
      {roles.map((r) => (
        <option key={r.id} value={r.id}>
          {r.name}
        </option>
      ))}
    </select>
  );
});
