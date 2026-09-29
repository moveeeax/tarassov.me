import { Card, CardBody, CardSubtitle, CardHeader, CardTitle } from '@/components/tabler/Card';

export function AboutPage() {
  return (
    <div className="container-xl">
      <Card>
        <CardHeader>
          <div>
            <CardTitle>About</CardTitle>
            <CardSubtitle>
              React admin SPA on a C++ REST backend — full account and admin flows.
            </CardSubtitle>
          </div>
        </CardHeader>
        <CardBody className="vstack gap-2 small text-secondary">
          <p className="mb-0">
            Backend: Drogon, libpqxx, redis-plus-plus, libsodium argon2id, JWT in HttpOnly cookies.
          </p>
          <p className="mb-0">
            Frontend: Vite + React 18 + TanStack Query + react-hook-form + zod + Tabler.
          </p>
        </CardBody>
      </Card>
    </div>
  );
}
