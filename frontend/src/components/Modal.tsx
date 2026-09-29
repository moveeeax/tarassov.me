import { useCallback, useRef, type ReactNode } from 'react';

import { useFocusTrap } from '@/hooks/useFocusTrap';
import { cn } from '@/lib/utils';

interface ModalProps {
  onClose: () => void;
  children: ReactNode;
  /** Extra classes for the dialog (e.g. `modal-lg`). */
  className?: string;
  /**
   * Optional veto for the *implicit* closes (backdrop click, Escape). Return
   * false to keep the modal open — e.g. a form with unsaved changes that asks
   * for confirmation first. Omitted (the default) means always close.
   */
  confirmClose?: () => boolean;
}

/**
 * Focus-trapped overlay for large forms/content — backdrop click and Escape
 * close it (subject to `confirmClose`), focus is restored on close, and it
 * scrolls when taller than the viewport. Tabler markup, no Bootstrap JS: a live
 * modal is the `show d-block` pair plus its own backdrop element.
 */
export function Modal({ onClose, children, className = 'modal-lg', confirmClose }: ModalProps) {
  // Read the predicate through a ref so requestClose (and therefore the focus
  // trap's effect) does not re-subscribe on every parent render.
  const confirmRef = useRef(confirmClose);
  confirmRef.current = confirmClose;

  const requestClose = useCallback(() => {
    if (confirmRef.current && !confirmRef.current()) return;
    onClose();
  }, [onClose]);

  const ref = useFocusTrap<HTMLDivElement>(requestClose);

  // A drag that STARTS inside the panel (selecting text in the body textarea)
  // and ends on the backdrop still fires click on the backdrop — closing the
  // modal and discarding the draft. Only a press that both started and ended
  // on the backdrop itself counts as a dismiss.
  const pressedBackdrop = useRef(false);

  return (
    <>
      <div className="modal-backdrop fade show" />
      <div
        className="modal modal-blur fade show d-block"
        tabIndex={-1}
        onMouseDown={(e) => {
          pressedBackdrop.current = e.target === e.currentTarget;
        }}
        onClick={(e) => {
          const dismiss = pressedBackdrop.current && e.target === e.currentTarget;
          pressedBackdrop.current = false;
          if (dismiss) requestClose();
        }}
      >
        <div
          className={cn('modal-dialog modal-dialog-centered modal-dialog-scrollable', className)}
          role="document"
          onClick={(e) => e.stopPropagation()}
        >
          <div ref={ref} role="dialog" aria-modal="true" tabIndex={-1} className="modal-content">
            {children}
          </div>
        </div>
      </div>
    </>
  );
}
