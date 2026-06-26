import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';

export function AboutPage() {
  return (
    <div className="container mx-auto py-8 max-w-3xl">
      <Card>
        <CardHeader>
          <CardTitle>About</CardTitle>
          <CardDescription>
            React admin SPA on a C++ REST backend — full account and admin flows.
          </CardDescription>
        </CardHeader>
        <CardContent className="text-sm text-muted-foreground space-y-2">
          <p>
            Backend: Drogon, libpqxx, redis-plus-plus, libsodium argon2id, JWT in HttpOnly
            cookies.
          </p>
          <p>
            Frontend: Vite + React 18 + TanStack Query + react-hook-form + zod + Tailwind +
            shadcn/ui primitives.
          </p>
        </CardContent>
      </Card>
    </div>
  );
}
