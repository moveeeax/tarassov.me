import type { ReactNode } from 'react';

import { Skeleton } from '@/components/ui/skeleton';
import { apiErrorMessage } from '@/lib/api/client';

/**
 * Dumb, presentational table. No sorting/filtering engine — the page
 * owns data fetching (usePagedQuery) and any bespoke filters above it.
 * DataTable only renders columns, loading/error/empty states, and dims
 * the body while a paged query shows placeholder (previous-page) data.
 */
export interface Column<Row> {
  header: ReactNode;
  /** Cell renderer for a row. */
  cell: (row: Row) => ReactNode;
  /** Extra classes for both the <th> and the <td>. */
  className?: string;
}

interface DataTableProps<Row> {
  columns: Column<Row>[];
  rows: Row[] | undefined;
  rowKey: (row: Row) => string | number;
  isLoading?: boolean;
  error?: unknown;
  emptyText?: string;
  /** Dim the body while showing previous-page placeholder data. */
  isPlaceholder?: boolean;
  /** Per-row props (e.g. onClick / className) for selectable tables. */
  rowProps?: (row: Row) => React.HTMLAttributes<HTMLTableRowElement>;
}

export function DataTable<Row>({
  columns,
  rows,
  rowKey,
  isLoading,
  error,
  emptyText = 'Nothing here yet.',
  isPlaceholder,
  rowProps,
}: DataTableProps<Row>) {
  if (error) return <p className="text-destructive">{apiErrorMessage(error, 'Failed to load.')}</p>;
  // Initial load (rows still undefined) renders skeleton rows so the header
  // and layout are stable from the first paint; pagination keeps the dimmed
  // previous-page placeholder (isPlaceholder) instead.
  if (isLoading && !rows) {
    return (
      <table className="w-full text-sm">
        <thead>
          <tr className="border-b border-border text-left text-xs uppercase tracking-wide text-muted-foreground">
            {columns.map((c, i) => (
              <th key={i} scope="col" className={`py-1.5 pr-4 font-medium ${c.className ?? ''}`}>
                {c.header}
              </th>
            ))}
          </tr>
        </thead>
        <tbody>
          {Array.from({ length: 5 }).map((_, r) => (
            <tr key={r} className="border-b border-border last:border-0">
              {columns.map((_, i) => (
                <td key={i} className="py-1.5 pr-4">
                  <Skeleton className="h-4 w-full" />
                </td>
              ))}
            </tr>
          ))}
        </tbody>
      </table>
    );
  }
  if (!rows) return null;
  if (rows.length === 0) return <p className="text-muted-foreground">{emptyText}</p>;

  return (
    <table className={`w-full text-sm ${isPlaceholder ? 'opacity-50' : ''}`}>
      <thead>
        <tr className="border-b border-border text-left text-xs uppercase tracking-wide text-muted-foreground">
          {columns.map((c, i) => (
            <th key={i} scope="col" className={`py-1.5 pr-4 font-medium ${c.className ?? ''}`}>
              {c.header}
            </th>
          ))}
        </tr>
      </thead>
      <tbody>
        {rows.map((row) => {
          const extra = rowProps?.(row);
          const { className: extraClass, ...restProps } = extra ?? {};
          return (
            <tr
              key={rowKey(row)}
              className={`border-b border-border transition-colors last:border-0 hover:bg-muted/50 ${extraClass ?? ''}`}
              {...restProps}
            >
              {columns.map((c, i) => (
                <td key={i} className={`py-1.5 pr-4 ${c.className ?? ''}`}>
                  {c.cell(row)}
                </td>
              ))}
            </tr>
          );
        })}
      </tbody>
    </table>
  );
}
