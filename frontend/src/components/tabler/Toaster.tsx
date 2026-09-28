import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useState,
  type ReactNode,
} from 'react';

import { IconAlertCircle, IconCircleCheck, IconInfoCircle } from '@tabler/icons-react';

import { cn } from '@/lib/utils';

type ToastVariant = 'error' | 'success' | 'info';

interface Toast {
  id: number;
  message: string;
  variant: ToastVariant;
}

interface ToastApi {
  show: (message: string, variant?: ToastVariant) => void;
  error: (message: string) => void;
  success: (message: string) => void;
  info: (message: string) => void;
}

const ToastContext = createContext<ToastApi | null>(null);
let counter = 0;

const ICONS = { error: IconAlertCircle, success: IconCircleCheck, info: IconInfoCircle } as const;
const ACCENT = { error: 'text-danger', success: 'text-green', info: 'text-primary' } as const;

/**
 * Toast notifications. Because the stack is `position: fixed`, toasts never
 * affect document layout — server errors and confirmations no longer shove the
 * form around (the whole point). Mounted once near the app root.
 */
export function ToastProvider({ children }: { children: ReactNode }) {
  const [toasts, setToasts] = useState<Toast[]>([]);
  const dismiss = useCallback((id: number) => setToasts((t) => t.filter((x) => x.id !== id)), []);
  const show = useCallback((message: string, variant: ToastVariant = 'info') => {
    counter += 1;
    const id = counter;
    setToasts((t) => [...t, { id, message, variant }]);
  }, []);
  const api = useMemo<ToastApi>(
    () => ({
      show,
      error: (m) => show(m, 'error'),
      success: (m) => show(m, 'success'),
      info: (m) => show(m, 'info'),
    }),
    [show],
  );

  return (
    <ToastContext.Provider value={api}>
      {children}
      <div className="toast-container position-fixed bottom-0 end-0 p-3">
        {toasts.map((t) => (
          <ToastItem key={t.id} toast={t} onDismiss={() => dismiss(t.id)} />
        ))}
      </div>
    </ToastContext.Provider>
  );
}

function ToastItem({ toast, onDismiss }: { toast: Toast; onDismiss: () => void }) {
  useEffect(() => {
    const timer = setTimeout(onDismiss, 5000);
    return () => clearTimeout(timer);
  }, [onDismiss]);
  const Icon = ICONS[toast.variant];
  return (
    <div
      className="toast show"
      role="status"
      aria-live={toast.variant === 'error' ? 'assertive' : 'polite'}
    >
      <div className="toast-body d-flex align-items-start gap-2">
        <Icon size={18} className={cn('flex-shrink-0', ACCENT[toast.variant])} aria-hidden />
        <span className="flex-fill">{toast.message}</span>
        <button
          type="button"
          className="btn-close"
          aria-label="Dismiss notification"
          onClick={onDismiss}
        />
      </div>
    </div>
  );
}

// eslint-disable-next-line react-refresh/only-export-components
export function useToast() {
  const ctx = useContext(ToastContext);
  if (!ctx) throw new Error('useToast must be used within ToastProvider');
  return ctx;
}
