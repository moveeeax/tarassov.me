import type { ReactNode } from 'react';

/**
 * The page-header block from the Tabler preview: pretitle, title and a right
 * aligned action slot. Every admin page opens with one, so the markup lives
 * here instead of being retyped 9 times.
 */
export function PageHeader({
  title,
  pretitle,
  actions,
}: {
  title: string;
  pretitle?: string;
  actions?: ReactNode;
}) {
  return (
    <div className="page-header d-print-none">
      <div className="container-xl">
        <div className="row align-items-center">
          <div className="col">
            {pretitle && <div className="page-pretitle">{pretitle}</div>}
            <h2 className="page-title">{title}</h2>
          </div>
          {actions && <div className="col-auto ms-auto d-print-none">{actions}</div>}
        </div>
      </div>
    </div>
  );
}
