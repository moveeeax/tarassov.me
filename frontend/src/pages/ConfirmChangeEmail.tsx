import { TokenConfirmCard } from '@/components/TokenConfirmCard';
import { api } from '@/lib/api/client';

/**
 * Hit by the link mailed to the NEW address during an email change.
 * Same pattern as ConfirmEmailPage: explicit button so scanners can't
 * burn the one-shot token.
 */
export function ConfirmChangeEmailPage() {
  return (
    <TokenConfirmCard
      title="Confirm your new email"
      description="Click the button below to switch your account to this address."
      successMessage="Your email address has been updated. Log in with the new address from now on."
      errorFallback="This link is invalid or has expired."
      buttonLabel="Confirm new email"
      mutate={(token) =>
        api.postJson('/api/v1/account/change-email/' + encodeURIComponent(token))
      }
    />
  );
}
