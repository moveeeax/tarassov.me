import type { ReactNode } from 'react';

import { Placeholder } from '@/components/tabler/Placeholder';
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
  /**
   * Row hover feedback. Only for tables whose rows are clickable (they pass
   * rowProps with onClick) — a hover highlight on a read-only table promises an
   * interaction that isn't there.
   */
  hoverable?: boolean;
}

function TableHead<Row>({ columns }: { columns: Column<Row>[] }) {
  return (
    <thead>
      <tr>
        {columns.map((c, i) => (
          <th key={i} scope="col" className={c.className}>
            {c.header}
          </th>
        ))}
      </tr>
    </thead>
  );
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
  hoverable,
}: DataTableProps<Row>) {
  if (error) return <p className="text-danger m-3">{apiErrorMessage(error, 'Failed to load.')}</p>;
  // Initial load (rows still undefined) renders placeholder rows so the header
  // and layout are stable from the first paint; pagination keeps the dimmed
  // previous-page body (isPlaceholder) instead.
  if (isLoading && !rows) {
    return (
      <table className="table table-vcenter card-table">
        <TableHead columns={columns} />
        <tbody>
          {Array.from({ length: 5 }).map((_, r) => (
            <tr key={r}>
              {columns.map((_, i) => (
                <td key={i}>
                  <Placeholder />
                </td>
              ))}
            </tr>
          ))}
        </tbody>
      </table>
    );
  }
  if (!rows) return null;
  if (rows.length === 0) return <p className="text-secondary m-3">{emptyText}</p>;

  return (
    <table
      className={`table table-vcenter card-table ${hoverable ? 'table-hover' : ''} ${
        isPlaceholder ? 'opacity-75' : ''
      }`}
    >
      <TableHead columns={columns} />
      <tbody>
        {rows.map((row) => {
          const extra = rowProps?.(row);
          const { className: extraClass, ...restProps } = extra ?? {};
          return (
            <tr key={rowKey(row)} className={extraClass} {...restProps}>
              {columns.map((c, i) => (
                <td key={i} className={c.className}>
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
