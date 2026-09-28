import { cn } from '@/lib/utils';

/**
 * Loading bar. Tabler's .placeholder needs a .placeholder-glow ancestor to
 * animate, so the wrapper is part of the component rather than a call-site
 * detail everyone forgets.
 */
export function Placeholder({ className }: { className?: string }) {
  return (
    <span className="placeholder-glow">
      <span className={cn('placeholder', className ?? 'col-12')} />
    </span>
  );
}
