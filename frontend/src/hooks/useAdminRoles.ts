import { useQuery } from '@tanstack/react-query';

import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';

/**
 * Shared admin roles query — Roles, UserDetail and InviteUser pages all
 * need the same list under the same cache key. The response type is
 * inferred from the OpenAPI `paths` tree (RolesResponse).
 */
export function useAdminRoles() {
  return useQuery({
    queryKey: qk.admin.roles(),
    queryFn: () => api.getJson('/api/v1/admin/roles'),
  });
}
