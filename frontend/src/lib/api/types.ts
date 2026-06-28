/**
 * Flat domain-type aliases over the generated OpenAPI tree.
 *
 * openapi-typescript emits indexed types (`components['schemas']['User']`)
 * into schema.gen.ts. Call sites want flat names (`User`, `Job`, …), so
 * this module re-exports thin aliases — change a shape in
 * docs/openapi.yaml, run `npm run gen:api`, and every page picks it up.
 *
 * Everything here is now generated: the me / user-detail / roles / invite
 * response envelopes earned named `components/schemas` entries in
 * docs/openapi.yaml, so there are no hand-written envelope types left to
 * drift against the backend.
 */
import type { components } from './schema.gen';

type Schemas = components['schemas'];

export type Role = Schemas['Role'];
export type User = Schemas['User'];
export type Job = Schemas['Job'];
export type AuditEntry = Schemas['AuditEntry'];
export type AuditListResponse = Schemas['AuditListResponse'];
export type UserListResponse = Schemas['UserListResponse'];
export type JobListResponse = Schemas['JobListResponse'];
export type DlqListResponse = Schemas['DlqListResponse'];
export type JobCreate = Schemas['JobCreate'];

/** GET /api/auth/me, POST /api/auth/login, POST /api/auth/refresh — { user }. */
export type MeResponse = Schemas['MeResponse'];
/** GET/PATCH /api/admin/users/{id}, POST /api/admin/users — { data: User }. */
export type UserDetailResponse = Schemas['UserDetailResponse'];
/** GET /api/admin/roles — { data: Role[] }. */
export type RolesResponse = Schemas['RolesResponse'];
/** POST/PATCH /api/admin/roles[/{id}] — { data: Role }. */
export type RoleDetailResponse = Schemas['RoleDetailResponse'];
/** POST /api/admin/invite — { data: User, message? }. */
export type InviteResponse = Schemas['InviteResponse'];
/** Generic { message } envelope (logout, delete, …). */
export type MessageResponse = Schemas['MessageResponse'];
