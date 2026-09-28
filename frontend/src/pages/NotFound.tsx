import { Link } from 'react-router-dom';

export function NotFoundPage() {
  return (
    <div className="container-tight py-4">
      <div className="empty">
        <div className="empty-header">404</div>
        <p className="empty-title">Page not found</p>
        <p className="empty-subtitle text-secondary">
          That page doesn&apos;t exist or has moved. Check the address, or head back home.
        </p>
        <div className="empty-action">
          <Link to="/" className="btn btn-primary">
            Back to home
          </Link>
        </div>
      </div>
    </div>
  );
}
