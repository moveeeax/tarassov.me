import { z } from 'zod';

/**
 * Mirrors Validation::* on the backend (src/api/Validation.hpp + the
 * register/login/reset handlers). zod is the source of truth on the
 * client; the backend re-validates on every request.
 */

export const loginSchema = z.object({
  email: z.string().min(1, 'Email is required').email('Invalid email'),
  password: z.string().min(1, 'Password is required'),
});

export const registerSchema = z
  .object({
    email: z.string().min(1, 'Email is required').email('Invalid email'),
    password: z
      .string()
      .min(8, 'Password must be at least 8 characters')
      .max(128, 'Password is too long'),
    password_confirm: z.string(),
    first_name: z.string().max(64).optional(),
    last_name: z.string().max(64).optional(),
  })
  .refine((d) => d.password === d.password_confirm, {
    path: ['password_confirm'],
    message: 'Passwords must match',
  });

export const requestResetSchema = z.object({
  email: z.string().min(1, 'Email is required').email('Invalid email'),
});

export const resetPasswordSchema = z
  .object({
    new_password: z
      .string()
      .min(8, 'Password must be at least 8 characters')
      .max(128, 'Password is too long'),
    new_password_confirm: z.string(),
  })
  .refine((d) => d.new_password === d.new_password_confirm, {
    path: ['new_password_confirm'],
    message: 'Passwords must match',
  });

export const changePasswordSchema = z
  .object({
    old_password: z.string().min(1, 'Current password is required'),
    new_password: z
      .string()
      .min(8, 'Password must be at least 8 characters')
      .max(128, 'Password is too long'),
    new_password_confirm: z.string(),
  })
  .refine((d) => d.new_password === d.new_password_confirm, {
    path: ['new_password_confirm'],
    message: 'Passwords must match',
  });

export const changeEmailSchema = z.object({
  new_email: z.string().min(1, 'Email is required').email('Invalid email'),
  password: z.string().min(1, 'Password is required'),
});
