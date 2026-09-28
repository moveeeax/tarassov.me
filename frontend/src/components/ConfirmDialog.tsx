import { Button } from '@/components/tabler/Button';
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
 * Escape/backdrop to cancel, returns focus on close. Tabler markup, no
 * Bootstrap JS: the `show d-block` pair is what a live modal looks like.
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
    <>
      <div className="modal-backdrop fade show" />
      <div className="modal modal-blur fade show d-block" tabIndex={-1} onClick={onClose}>
        <div
          className="modal-dialog modal-sm modal-dialog-centered"
          role="document"
          onClick={(e) => e.stopPropagation()}
        >
          <div
            ref={ref}
            role="alertdialog"
            aria-modal="true"
            aria-labelledby="confirm-title"
            aria-describedby={description ? 'confirm-desc' : undefined}
            tabIndex={-1}
            className="modal-content"
          >
            <div className="modal-header">
              <h5 id="confirm-title" className="modal-title">
                {title}
              </h5>
              <button
                type="button"
                className="btn-close"
                aria-label="Cancel"
                onClick={onClose}
                disabled={busy}
              />
            </div>
            {description && (
              <div className="modal-body">
                <p id="confirm-desc" className="text-secondary mb-0">
                  {description}
                </p>
              </div>
            )}
            <div className="modal-footer">
              <Button variant="ghost" onClick={onClose} disabled={busy}>
                Cancel
              </Button>
              <Button
                variant={destructive ? 'danger' : 'primary'}
                onClick={onConfirm}
                disabled={busy}
              >
                {busy ? 'Working…' : confirmLabel}
              </Button>
            </div>
          </div>
        </div>
      </div>
    </>
  );
}
