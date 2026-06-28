import { z } from 'zod';

/**
 * Admin-facing form schemas. zod is the source of truth on the client;
 * the backend re-validates (Api::Validation::*) on every request.
 */

export const inviteUserSchema = z.object({
  email: z.string().min(1, 'Email is required').email('Invalid email'),
  first_name: z.string().max(64).optional(),
  last_name: z.string().max(64).optional(),
  // RoleSelect renders a native <select> whose value is a string; empty
  // string means "use the default role".
  role_id: z.string().optional(),
});

export type InviteUserValues = z.infer<typeof inviteUserSchema>;
