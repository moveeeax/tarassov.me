import { Button } from '@/components/tabler/Button';

interface PaginationFooterProps {
  page: number;
  totalPages: number;
  /** True while the previous page is shown as a placeholder — blocks "Next". */
  isPlaceholderData: boolean;
  onPageChange: (page: number) => void;
}

/**
 * Shared Previous/Next footer for offset-paginated admin tables.
 * Renders nothing when everything fits on one page.
 */
export function PaginationFooter({
  page,
  totalPages,
  isPlaceholderData,
  onPageChange,
}: PaginationFooterProps) {
  if (totalPages <= 1) return null;
  return (
    <div className="mt-3 d-flex align-items-center justify-content-between">
      <p className="small text-secondary mb-0">
        Page {page} of {totalPages}
      </p>
      <div className="btn-list">
        <Button
          variant="outline"
          size="sm"
          aria-label="Previous page"
          disabled={page <= 1}
          onClick={() => onPageChange(page - 1)}
        >
          Previous
        </Button>
        <Button
          variant="outline"
          size="sm"
          aria-label="Next page"
          disabled={page >= totalPages || isPlaceholderData}
          onClick={() => onPageChange(page + 1)}
        >
          Next
        </Button>
      </div>
    </div>
  );
}
