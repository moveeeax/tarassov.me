import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { Link, useLocation, useNavigate } from 'react-router-dom';
import type { z } from 'zod';

import { Button } from '@/components/tabler/Button';
import { FormField } from '@/components/FormField';
import { Card, CardBody, CardSubtitle, CardHeader, CardTitle } from '@/components/tabler/Card';
import { useToast } from '@/components/tabler/Toaster';
import { useLogin } from '@/hooks/useAuthMutations';
import { apiErrorMessage } from '@/lib/api/client';
import { loginSchema } from '@/lib/schemas/auth';

type FormValues = z.infer<typeof loginSchema>;

export function LoginPage() {
  const navigate = useNavigate();
  const location = useLocation();
  const next = (location.state as { from?: string } | null)?.from ?? '/';

  const toast = useToast();
  const login = useLogin();
  const {
    register,
    handleSubmit,
    formState: { errors, isSubmitting },
  } = useForm<FormValues>({ resolver: zodResolver(loginSchema) });

  const onSubmit = handleSubmit(async (values) => {
    try {
      await login.mutateAsync(values);
      navigate(next, { replace: true });
    } catch (e) {
      toast.error(apiErrorMessage(e));
    }
  });

  return (
    <div className="container-tight py-4">
      <Card>
        <CardHeader>
          <div>
            <CardTitle>Log in</CardTitle>
            <CardSubtitle>Use your email and password.</CardSubtitle>
          </div>
        </CardHeader>
        <CardBody>
          <form className="vstack gap-4" onSubmit={onSubmit}>
            <FormField
              id="email"
              type="email"
              label="Email"
              autoComplete="email"
              error={errors.email?.message}
              {...register('email')}
            />
            <FormField
              id="password"
              type="password"
              label="Password"
              autoComplete="current-password"
              error={errors.password?.message}
              {...register('password')}
            />
            <Button type="submit" className="w-100" disabled={isSubmitting || login.isPending}>
              {login.isPending ? 'Signing in…' : 'Log in'}
            </Button>
            <div className="d-flex justify-content-between small text-secondary">
              <Link to="/account/reset-password">Forgot password?</Link>
              <Link to="/register">Create account</Link>
            </div>
          </form>
        </CardBody>
      </Card>
    </div>
  );
}
