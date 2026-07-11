import { TokenConfirmCard } from '@/components/TokenConfirmCard';
import { api } from '@/lib/api/client';

/**
 * Hit by the link in the confirmation email. The POST is behind an
 * explicit button rather than a useEffect: email scanners prefetch
 * links (burning the one-shot token before the user arrives) and
 * React StrictMode double-runs effects (double POST).
 */
export function ConfirmEmailPage() {
  return (
    <TokenConfirmCard
      title="Confirm your account"
      description="Click the button below to activate your account."
      successMessage="Your account is confirmed. You can log in now."
      errorFallback="This confirmation link is invalid or has expired."
      buttonLabel="Confirm my account"
      mutate={(token) => api.postJson('/api/v1/account/confirm/' + encodeURIComponent(token))}
    />
  );
}
