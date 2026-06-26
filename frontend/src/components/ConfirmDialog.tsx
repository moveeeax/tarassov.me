import { Button } from '@/components/ui/button';
import { Card, CardContent } from '@/components/ui/card';
import { useFocusTrap } from '@/hooks/useFocusTrap';

interface ConfirmDialogProps {
  title: string;
  description?: string;
  confirmLabel?: string;
  destructive?: boolean;
  busy?: boolean;
  onConfirm: () => void;
  onClose: () => void;
}

/**
 * Accessible confirm modal replacing native confirm() — focus-trapped,
 * Escape/backdrop to cancel, returns focus on close.
 */
export function ConfirmDialog({
  title,
  description,
  confirmLabel = 'Confirm',
  destructive,
  busy,
  onConfirm,
  onClose,
}: ConfirmDialogProps) {
  const ref = useFocusTrap<HTMLDivElement>(onClose);
  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center bg-black/50 p-4"
      onClick={onClose}
    >
      <Card
        ref={ref}
        role="alertdialog"
        aria-modal="true"
        aria-labelledby="confirm-title"
        aria-describedby={description ? 'confirm-desc' : undefined}
        tabIndex={-1}
        className="w-full max-w-sm"
        onClick={(e) => e.stopPropagation()}
      >
        <CardContent className="space-y-4 pt-6">
          <div>
            <h2 id="confirm-title" className="text-lg font-semibold">
              {title}
            </h2>
            {description && (
              <p id="confirm-desc" className="mt-1 text-sm text-muted-foreground">
                {description}
              </p>
            )}
          </div>
          <div className="flex justify-end gap-2">
            <Button variant="ghost" onClick={onClose} disabled={busy}>
              Cancel
            </Button>
            <Button
              variant={destructive ? 'destructive' : 'default'}
              onClick={onConfirm}
              disabled={busy}
            >
              {busy ? 'Working…' : confirmLabel}
            </Button>
          </div>
        </CardContent>
      </Card>
    </div>
  );
}
