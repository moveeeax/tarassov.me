import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useState,
  type ReactNode,
} from 'react';

import { AlertCircle, CheckCircle2, Info, X } from 'lucide-react';

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

const ICONS = { error: AlertCircle, success: CheckCircle2, info: Info } as const;
const ACCENT = {
  error: 'text-destructive',
  success: 'text-emerald-500',
  info: 'text-primary',
} as const;

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
      <div className="pointer-events-none fixed bottom-4 right-4 z-[100] flex w-full max-w-sm flex-col gap-2">
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
      role="status"
      aria-live={toast.variant === 'error' ? 'assertive' : 'polite'}
      className="animate-in slide-in-from-right-4 fade-in pointer-events-auto flex items-start gap-3 rounded-md border border-border bg-card p-3 text-sm text-card-foreground shadow-lg"
    >
      <Icon className={cn('mt-0.5 h-4 w-4 shrink-0', ACCENT[toast.variant])} aria-hidden />
      <span className="flex-1 break-words">{toast.message}</span>
      <button
        type="button"
        onClick={onDismiss}
        aria-label="Dismiss notification"
        className="shrink-0 rounded text-muted-foreground transition-colors hover:text-foreground focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring"
      >
        <X className="h-4 w-4" />
      </button>
    </div>
  );
}

// eslint-disable-next-line react-refresh/only-export-components
export function useToast() {
  const ctx = useContext(ToastContext);
  if (!ctx) throw new Error('useToast must be used within ToastProvider');
  return ctx;
}
