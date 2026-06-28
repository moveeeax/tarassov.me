import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { Link, useNavigate } from 'react-router-dom';
import type { z } from 'zod';

import { Button } from '@/components/ui/button';
import { FormField } from '@/components/FormField';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { useToast } from '@/components/ui/toaster';
import { useRegister } from '@/hooks/useAuthMutations';
import { apiErrorMessage } from '@/lib/api/client';
import { registerSchema } from '@/lib/schemas/auth';

type FormValues = z.infer<typeof registerSchema>;

export function RegisterPage() {
  const toast = useToast();
  const reg = useRegister();
  const navigate = useNavigate();
  const {
    register,
    handleSubmit,
    formState: { errors, isSubmitting },
  } = useForm<FormValues>({ resolver: zodResolver(registerSchema) });

  const onSubmit = handleSubmit(async (values) => {
    try {
      await reg.mutateAsync({
        email: values.email,
        password: values.password,
        first_name: values.first_name,
        last_name: values.last_name,
      });
      navigate('/account/check-email', {
        replace: true,
        state: { email: values.email },
      });
    } catch (e) {
      toast.error(apiErrorMessage(e));
    }
  });

  return (
    <div className="container mx-auto max-w-md py-8">
      <Card>
        <CardHeader>
          <CardTitle>Create your account</CardTitle>
          <CardDescription>You'll get a confirmation email after signing up.</CardDescription>
        </CardHeader>
        <CardContent>
          <form className="space-y-4" onSubmit={onSubmit}>
            <div className="grid grid-cols-1 sm:grid-cols-2 gap-3">
              <FormField id="first_name" label="First name" {...register('first_name')} />
              <FormField id="last_name" label="Last name" {...register('last_name')} />
            </div>
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
              autoComplete="new-password"
              error={errors.password?.message}
              {...register('password')}
            />
            <FormField
              id="password_confirm"
              type="password"
              label="Confirm password"
              autoComplete="new-password"
              error={errors.password_confirm?.message}
              {...register('password_confirm')}
            />
            <Button type="submit" className="w-full" disabled={isSubmitting || reg.isPending}>
              {reg.isPending ? 'Creating account…' : 'Register'}
            </Button>
            <div className="text-sm text-muted-foreground text-center">
              Already have an account?{' '}
              <Link to="/login" className="hover:underline">
                Log in
              </Link>
            </div>
          </form>
        </CardContent>
      </Card>
    </div>
  );
}
