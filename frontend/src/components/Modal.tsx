import type { ReactNode } from 'react';

import { useFocusTrap } from '@/hooks/useFocusTrap';

interface ModalProps {
  onClose: () => void;
  children: ReactNode;
  /** Width class for the panel (default max-w-2xl). */
  className?: string;
}

/**
 * Focus-trapped overlay for large forms/content — backdrop click and Escape
 * close it, focus is restored on close, and it scrolls when taller than the
 * viewport (unlike ConfirmDialog, which centres a small fixed panel).
 */
export function Modal({ onClose, children, className = 'max-w-2xl' }: ModalProps) {
  const ref = useFocusTrap<HTMLDivElement>(onClose);
  return (
    <div
      className="fixed inset-0 z-50 flex items-start justify-center overflow-y-auto bg-black/50 p-4"
      onClick={onClose}
    >
      <div
        ref={ref}
        role="dialog"
        aria-modal="true"
        tabIndex={-1}
        className={`my-8 w-full ${className}`}
        onClick={(e) => e.stopPropagation()}
      >
        {children}
      </div>
    </div>
  );
}
